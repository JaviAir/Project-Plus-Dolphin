// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayController.h"

#include <utility>

#include "Common/Logging/Log.h"
#include "Core/Config/NetplaySettings.h"

QuickPlayController::QuickPlayController(StartHost start_host, CancelHost cancel_host,
                                         JoinHost join_host, QObject* parent)
    : QObject(parent), m_start_host(std::move(start_host)), m_cancel_host(std::move(cancel_host)),
      m_join_host(std::move(join_host))
{
  m_setup_timer.setSingleShot(true);
  m_setup_timer.setInterval(25000);
  connect(&m_setup_timer, &QTimer::timeout, this,
          [this] { Fail(tr("Quick Play room setup timed out. Cancel and try again.")); });
  m_poll_timer.setSingleShot(true);
  m_poll_timer.setInterval(1000);
  connect(&m_poll_timer, &QTimer::timeout, this, [this] {
    if (m_state == State::Searching)
      m_client.Poll();
    else if (m_state == State::WaitingForHost)
      m_client.PollMatch(m_match_id);
  });
}

QuickPlayController::~QuickPlayController()
{
  Cancel();
}

bool QuickPlayController::Start()
{
  // A result remains part of the attempt until the user dismisses it.
  if (m_state != State::Idle || m_creating_host || m_joining)
    return false;

  ++m_attempt;
  m_entered_queue = false;
  m_error_message.clear();
  m_region = QuickPlayClient::NormalizeRegion(
      QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_REGION)));
  SetState(State::Searching);
  m_client.Start(QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_COORDINATOR)),
                 m_region, [this, attempt = m_attempt](QuickPlayClient::Result result) {
                   if (attempt == m_attempt &&
                       (m_state == State::Searching || m_state == State::PublishingHostCode ||
                        m_state == State::WaitingForHost || m_state == State::ValidatingMatch))
                     HandleResult(std::move(result));
                 });
  return true;
}

std::uint64_t QuickPlayController::GetGuestCancelAttempt() const
{
  switch (m_state)
  {
  case State::Searching:
  case State::MatchedHost:
  case State::MatchedClient:
  case State::PreparingHandoff:
  case State::WaitingForCoreStop:
  case State::ValidatingMatch:
  case State::CreatingHost:
  case State::WaitingForTraversalCode:
  case State::PublishingHostCode:
  case State::WaitingForOpponent:
  case State::WaitingForHost:
  case State::Connecting:
    return m_attempt;
  default:
    return 0;
  }
}

void QuickPlayController::Cancel()
{
  const auto cancelled_attempt = m_attempt++;
  m_poll_timer.stop();
  m_setup_timer.stop();
  m_host_code.clear();
  m_match_id.clear();
  m_client.Cancel();
  if (!m_creating_host && !m_joining && m_state != State::NetPlayOwned)
    m_cancel_host(cancelled_attempt);
  if (m_state != State::Idle && m_state != State::NetPlayOwned)
    INFO_LOG_FMT(NETPLAY, "QuickPlay: cancelled");
  m_error_message.clear();
  SetState(State::Idle);
}

void QuickPlayController::HandleResult(QuickPlayClient::Result result)
{
  switch (result.state)
  {
  case QuickPlayClient::State::Searching:
    if (m_state != State::Searching)
      return;
    if (!m_entered_queue)
    {
      INFO_LOG_FMT(NETPLAY, "QuickPlay: entered queue");
      m_entered_queue = true;
    }
    // One poll a second after the previous response, never overlapping requests.
    m_poll_timer.start();
    break;
  case QuickPlayClient::State::MatchedHost:
  case QuickPlayClient::State::MatchedClient:
    if (m_state == State::Searching)
    {
      ClaimMatch(result);
    }
    else if (m_state == State::ValidatingMatch)
    {
      if (result.match_id != m_match_id ||
          (result.state == QuickPlayClient::State::MatchedHost) != m_is_host)
      {
        Fail(tr("The Quick Play assignment changed during shutdown. Cancel and try again."));
        return;
      }
      if (m_is_host)
      {
        QTimer::singleShot(0, this, [this, attempt = m_attempt] { BeginHost(attempt); });
      }
      else
      {
        SetState(State::WaitingForHost);
        m_client.PollMatch(m_match_id);
      }
    }
    break;
  case QuickPlayClient::State::WaitingForHost:
    if (m_state == State::WaitingForHost && result.match_id == m_match_id)
      m_poll_timer.start();
    break;
  case QuickPlayClient::State::HostReady:
    if (m_state != State::WaitingForHost || result.match_id != m_match_id)
      return;
    if (!QuickPlayClient::IsValidHostCode(result.host_code))
    {
      Fail(tr("Invalid Quick Play traversal host code."));
      return;
    }
    m_poll_timer.stop();
    m_setup_timer.stop();
    m_host_code = std::move(result.host_code);
    INFO_LOG_FMT(NETPLAY, "QuickPlay: host code ready");
    SetState(State::Connecting);
    QTimer::singleShot(0, this, [this, attempt = m_attempt] { BeginJoin(attempt); });
    break;
  case QuickPlayClient::State::HostPublished:
    if (m_state != State::PublishingHostCode)
      return;
    // Keep the bounded setup timeout until the opponent actually arrives.
    INFO_LOG_FMT(NETPLAY, "QuickPlay: host code published");
    INFO_LOG_FMT(NETPLAY, "QuickPlay: waiting for opponent");
    SetState(State::WaitingForOpponent);
    break;
  case QuickPlayClient::State::Expired:
  case QuickPlayClient::State::Error:
    Fail(std::move(result.error));
    break;
  }
}

void QuickPlayController::ClaimMatch(const QuickPlayClient::Result& result)
{
  // Ownership moves here, before any signal, dialog completion, or Core stop. Only host
  // values are retained; the old EXI endpoint and guest generation die with the Core.
  const auto attempt = m_attempt;
  m_match_id = result.match_id;
  m_is_host = result.state == QuickPlayClient::State::MatchedHost;
  m_poll_timer.stop();
  m_setup_timer.start();
  INFO_LOG_FMT(NETPLAY, "QuickPlay: claimed {} assignment, attempt {}",
               m_is_host ? "host" : "client", attempt);
  SetState(m_is_host ? State::MatchedHost : State::MatchedClient);
  if (attempt != m_attempt)
    return;
  SetState(State::PreparingHandoff);
  if (attempt != m_attempt)
    return;
  SetState(State::WaitingForCoreStop);
  if (attempt == m_attempt)
    emit PrepareHandoff(attempt);
}

void QuickPlayController::OnCoreStopped(std::uint64_t attempt, const QString& error)
{
  if (attempt != m_attempt || m_state != State::WaitingForCoreStop)
    return;
  if (!error.isEmpty())
  {
    Fail(error);
    return;
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay: old Core stopped, validating attempt {}", attempt);
  SetState(State::ValidatingMatch);
  // Recheck the real ticket after shutdown, before constructing any NetPlay object.
  // This catches coordinator invalidation/peer cancellation while the Core stopped.
  if (attempt == m_attempt)
    m_client.Poll();
}

void QuickPlayController::BeginJoin(std::uint64_t attempt)
{
  if (attempt != m_attempt || m_state != State::Connecting)
    return;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: invoking NetPlay join");
  m_joining = true;
  const bool joined = m_join_host(m_host_code);
  m_joining = false;
  // Existing synchronous NetPlay setup may enter a modal error loop. Cancel/close invalidates
  // this result there; never tear down an opened lobby or overwrite a newer attempt.
  if (attempt != m_attempt)
  {
    m_cancel_host(attempt);
    return;
  }
  if (!joined)
  {
    WARN_LOG_FMT(NETPLAY, "QuickPlay: NetPlay join failed");
    Fail(tr("Could not join the Quick Play host. The host may have left. Cancel and try again."));
    return;
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay: NetPlay join started");
  INFO_LOG_FMT(NETPLAY, "QuickPlay: client connection established");
  Complete();
}

void QuickPlayController::OnHostOpponentConnected(quint64 attempt)
{
  // The peer can arrive before the publication HTTP response reaches the GUI thread.
  if (attempt != m_attempt ||
      (m_state != State::PublishingHostCode && m_state != State::WaitingForOpponent))
    return;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: host opponent connected");
  Complete();
}

void QuickPlayController::Complete()
{
  m_poll_timer.stop();
  m_setup_timer.start();
  // No connected acknowledgement exists. One bounded DELETE releases the rendezvous;
  // readiness/launch remains attempt-owned until the normal BootGame callback.
  m_client.Cancel();
  m_match_id.clear();
  m_host_code.clear();
  SetState(State::NetPlayConnected);
}

void QuickPlayController::OnLaunchOwned(std::uint64_t attempt)
{
  if (attempt != m_attempt || m_state != State::NetPlayConnected)
    return;
  m_setup_timer.stop();
  ++m_attempt;  // Invalidate every queued setup callback before notifying observers.
  SetState(State::NetPlayOwned);
}

void QuickPlayController::OnLaunchFailed(std::uint64_t attempt, const QString& error)
{
  if (attempt == m_attempt && m_state != State::Idle && m_state != State::Error &&
      m_state != State::NetPlayOwned)
    Fail(error);
}

void QuickPlayController::BeginHost(std::uint64_t attempt)
{
  if (attempt != m_attempt || m_state != State::ValidatingMatch)
    return;
  SetState(State::CreatingHost);
  if (attempt != m_attempt)
    return;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: creating NetPlay host");
  m_creating_host = true;
  const auto error = m_start_host(attempt);
  m_creating_host = false;
  // The normal host path can enter a modal error dialog. Cancel may run inside it.
  if (attempt != m_attempt)
  {
    m_cancel_host(attempt);
    return;
  }
  if (!error.isEmpty())
  {
    Fail(error);
    return;
  }
  SetState(State::WaitingForTraversalCode);
  if (!m_host_code.isEmpty())
    OnHostTraversalChanged(attempt, m_host_code, false);
}

void QuickPlayController::OnHostTraversalChanged(quint64 attempt, const QString& code, bool failed)
{
  if (attempt != m_attempt ||
      (m_state != State::CreatingHost && m_state != State::WaitingForTraversalCode &&
       m_state != State::PublishingHostCode && m_state != State::WaitingForOpponent))
    return;
  if (failed)
  {
    Fail(tr("The Quick Play traversal connection failed. Cancel and try again."));
    return;
  }
  if (code.isEmpty())
    return;
  // Duplicate callbacks are harmless; a changed room code must never replace a published code.
  if (!m_host_code.isEmpty() && m_host_code != code)
  {
    Fail(tr("The Quick Play room connection changed. Cancel and try again."));
    return;
  }
  if (m_state == State::PublishingHostCode || m_state == State::WaitingForOpponent)
    return;
  m_host_code = code;
  if (m_state == State::CreatingHost)
    return;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: traversal ready");
  INFO_LOG_FMT(NETPLAY, "QuickPlay: host code available");
  SetState(State::PublishingHostCode);
  INFO_LOG_FMT(NETPLAY, "QuickPlay: publishing host code");
  m_client.PublishHostCode(m_match_id, m_host_code);
}

void QuickPlayController::OnHostClosed(std::uint64_t attempt)
{
  if (attempt == m_attempt && m_state != State::Idle && m_state != State::Error &&
      m_state != State::NetPlayOwned)
    Fail(tr("The Quick Play NetPlay room was closed. Cancel and try again."));
}

void QuickPlayController::Fail(QString error)
{
  WARN_LOG_FMT(NETPLAY, "QuickPlay: attempt failed");
  Cancel();
  m_error_message = std::move(error);
  SetState(State::Error);
}

void QuickPlayController::SetState(State state)
{
  if (m_state == state)
    return;

  m_state = state;
  emit StateChanged(state);
}
