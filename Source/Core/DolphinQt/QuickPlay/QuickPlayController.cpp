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
  SetState(State::Searching);
  m_client.Start(QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_COORDINATOR)),
                 QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_REGION)),
                 [this, attempt = m_attempt](QuickPlayClient::Result result) {
                   if (attempt == m_attempt &&
                       (m_state == State::Searching || m_state == State::PublishingHostCode ||
                        m_state == State::WaitingForHost))
                     HandleResult(std::move(result));
                 });
  return true;
}

void QuickPlayController::Cancel()
{
  const auto cancelled_attempt = m_attempt++;
  m_poll_timer.stop();
  m_setup_timer.stop();
  m_host_code.clear();
  m_match_id.clear();
  m_client.Cancel();
  if (!m_creating_host)
    m_cancel_host(cancelled_attempt);
  if (m_state != State::Idle)
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
    if (m_state != State::Searching)
      return;
    INFO_LOG_FMT(NETPLAY, "QuickPlay: assigned host");
    m_match_id = result.match_id;
    SetState(State::MatchedHost);
    m_setup_timer.start();
    QTimer::singleShot(0, this, [this, attempt = m_attempt] { BeginHost(attempt); });
    break;
  case QuickPlayClient::State::MatchedClient:
    if (m_state != State::Searching)
      return;
    INFO_LOG_FMT(NETPLAY, "QuickPlay: assigned client; waiting for host");
    m_match_id = result.match_id;
    SetState(State::MatchedClient);
    SetState(State::WaitingForHost);
    INFO_LOG_FMT(NETPLAY, "QuickPlay: waiting for host code");
    m_setup_timer.start();
    m_client.PollMatch(m_match_id);
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
    m_setup_timer.stop();
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
    return;
  if (!joined)
  {
    WARN_LOG_FMT(NETPLAY, "QuickPlay: NetPlay join failed");
    Fail(tr("Could not join the Quick Play host. The host may have left. Cancel and try again."));
    return;
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay: NetPlay join started");
  INFO_LOG_FMT(NETPLAY, "QuickPlay: client connection established");
  // No connected acknowledgement exists. One bounded DELETE releases the rendezvous;
  // the normal NetPlay session now owns the connection independently.
  m_client.Cancel();
  m_match_id.clear();
  m_host_code.clear();
  SetState(State::NetPlayConnected);
}

void QuickPlayController::BeginHost(std::uint64_t attempt)
{
  if (attempt != m_attempt || m_state != State::MatchedHost)
    return;
  SetState(State::CreatingHost);
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
  if (attempt == m_attempt && m_state != State::Idle && m_state != State::Error)
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
