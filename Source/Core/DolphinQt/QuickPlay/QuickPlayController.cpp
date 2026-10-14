// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayController.h"

#include <utility>

#include "Common/Logging/Log.h"
#include "Core/Config/NetplaySettings.h"

QuickPlayController::QuickPlayController(StartHost start_host, CancelHost cancel_host,
                                         JoinHost join_host, OriginDigest origin_digest,
                                         QObject* parent)
    : QObject(parent), m_start_host(std::move(start_host)), m_cancel_host(std::move(cancel_host)),
      m_join_host(std::move(join_host)), m_origin_digest(std::move(origin_digest))
{
  m_setup_timer.setSingleShot(true);
  m_setup_timer.setInterval(25000);
  connect(&m_setup_timer, &QTimer::timeout, this,
          [this] { Fail(tr("Quick Play room setup timed out. Cancel and try again.")); });
  m_poll_timer.setSingleShot(true);
  m_poll_timer.setInterval(1000);
  connect(&m_poll_timer, &QTimer::timeout, this, [this] {
    if (m_state == State::Searching)
      m_matchmaker->Poll();
    else if (m_state == State::WaitingForHost)
      m_matchmaker->PollMatch(m_match_id);
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
  m_regions = QuickPlayClient::LoadRegions();
  m_match_region.clear();
  // Loaded before SetState: the searching dialog reads it on the state change.
  m_host_preference = QuickPlayClient::LoadHostPreference();
  SetState(State::Searching);
  StartBackend(Config::Get(Config::NETPLAY_QUICKPLAY_MATCHMAKING) == "coordinator" ?
                   Backend::Coordinator :
                   Backend::Lobby);
  return true;
}

void QuickPlayController::StartBackend(Backend backend)
{
  m_backend = backend;
  m_matchmaker = backend == Backend::Lobby ? static_cast<QuickPlayMatchmaker*>(&m_lobby) :
                                             static_cast<QuickPlayMatchmaker*>(&m_coordinator);
  m_search_notice.clear();
  m_search_stage = QuickPlayMatchmaker::SearchStage::Normal;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: searching through the {}",
               backend == Backend::Lobby ? "Dolphin lobby" : "coordinator");
  m_matchmaker->Start(m_regions, m_host_preference, m_origin_digest(),
                      [this, attempt = m_attempt](QuickPlayMatchmaker::Result result) {
                        if (attempt == m_attempt &&
                            (m_state == State::Searching || m_state == State::PublishingHostCode ||
                             m_state == State::WaitingForHost || m_state == State::ValidatingMatch))
                          HandleResult(std::move(result));
                      });
}

QString QuickPlayController::GetBackendLabel() const
{
  return m_backend == Backend::Lobby ? tr("Dolphin lobby") : tr("custom coordinator");
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
  case State::WaitingForHandoff:
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
  m_matchmaker->Cancel();
  m_search_notice.clear();
  m_search_stage = QuickPlayMatchmaker::SearchStage::Normal;
  if (!m_creating_host && !m_joining && m_state != State::NetPlayOwned)
    m_cancel_host(cancelled_attempt);
  if (m_state != State::Idle && m_state != State::NetPlayOwned)
    INFO_LOG_FMT(NETPLAY, "QuickPlay: cancelled");
  m_error_message.clear();
  SetState(State::Idle);
}

void QuickPlayController::HandleResult(QuickPlayMatchmaker::Result result)
{
  switch (result.state)
  {
  case QuickPlayMatchmaker::State::Searching:
    if (m_state != State::Searching)
      return;
    if (!m_entered_queue)
    {
      INFO_LOG_FMT(NETPLAY, "QuickPlay: entered queue");
      m_entered_queue = true;
    }
    m_search_notice = std::move(result.notice);
    m_search_stage = result.stage;
    m_search_stage_seconds = result.stage_seconds;
    // One poll a second after the previous response, never overlapping requests.
    m_poll_timer.start();
    break;
  case QuickPlayMatchmaker::State::MatchedHost:
  case QuickPlayMatchmaker::State::MatchedClient:
    if (m_state == State::Searching)
    {
      ClaimMatch(result);
    }
    else if (m_state == State::ValidatingMatch)
    {
      if (result.match_id != m_match_id ||
          (result.state == QuickPlayMatchmaker::State::MatchedHost) != m_is_host)
      {
        Fail(tr("The Quick Play assignment changed during handoff. Cancel and try again."));
        return;
      }
      if (m_is_host)
      {
        QTimer::singleShot(0, this, [this, attempt = m_attempt] { BeginHost(attempt); });
      }
      else
      {
        SetState(State::WaitingForHost);
        m_matchmaker->PollMatch(m_match_id);
      }
    }
    break;
  case QuickPlayMatchmaker::State::WaitingForHost:
    if (m_state == State::WaitingForHost && result.match_id == m_match_id)
      m_poll_timer.start();
    break;
  case QuickPlayMatchmaker::State::HostReady:
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
  case QuickPlayMatchmaker::State::HostPublished:
    if (m_state != State::PublishingHostCode)
      return;
    // Keep the bounded setup timeout until the opponent actually arrives.
    INFO_LOG_FMT(NETPLAY, "QuickPlay: host code published, waiting for opponent");
    SetState(State::WaitingForOpponent);
    break;
  case QuickPlayMatchmaker::State::Expired:
  case QuickPlayMatchmaker::State::Error:
    // Only before any match: the lobby has no shared state to hand over.
    if (result.unreachable && m_backend == Backend::Lobby && m_state == State::Searching &&
        QuickPlayClient::HasConfiguredCoordinator())
    {
      WARN_LOG_FMT(NETPLAY, "QuickPlay: Dolphin lobby unavailable; using the configured "
                            "coordinator");
      m_lobby.Cancel();
      StartBackend(Backend::Coordinator);
      return;
    }
    Fail(std::move(result.error));
    break;
  }
}

void QuickPlayController::ClaimMatch(const QuickPlayMatchmaker::Result& result)
{
  // Claim the assignment before any signal or asynchronous handoff. Keep only
  // host-owned values; checkpoint restoration can invalidate guest endpoints.
  const auto attempt = m_attempt;
  m_match_id = result.match_id;
  m_match_region = result.region;
  m_is_host = result.state == QuickPlayMatchmaker::State::MatchedHost;
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
  SetState(State::WaitingForHandoff);
  if (attempt == m_attempt)
    emit PrepareHandoff(attempt);
}

void QuickPlayController::OnCoreStopped(std::uint64_t attempt, const QString& error)
{
  OnHandoffReady(attempt, error);
}

void QuickPlayController::OnHandoffReady(std::uint64_t attempt, const QString& error)
{
  if (attempt != m_attempt || m_state != State::WaitingForHandoff)
    return;
  if (!error.isEmpty())
  {
    Fail(error);
    return;
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay: boundary prepared, validating attempt {}", attempt);
  SetState(State::ValidatingMatch);
  // Recheck the match after the Core stopped, before constructing any NetPlay object.
  if (attempt == m_attempt)
    m_matchmaker->Poll();
}

void QuickPlayController::BeginJoin(std::uint64_t attempt)
{
  if (attempt != m_attempt || m_state != State::Connecting)
    return;
  emit PrepareConnection(attempt, false);
}

void QuickPlayController::ContinueConnection(std::uint64_t attempt, bool host, const QString& error)
{
  if (attempt != m_attempt || m_state != (host ? State::CreatingHost : State::Connecting))
    return;
  if (!error.isEmpty())
    return Fail(error);
  if (host)
    ConnectHost(attempt);
  else
    ConnectJoin(attempt);
}

void QuickPlayController::ConnectJoin(std::uint64_t attempt)
{
  m_joining = true;
  const auto error = m_join_host(m_host_code);
  m_joining = false;
  // Existing synchronous NetPlay setup may enter a modal error loop. Cancel/close invalidates
  // this result there; never tear down an opened lobby or overwrite a newer attempt.
  if (attempt != m_attempt)
  {
    m_cancel_host(attempt);
    return;
  }
  if (!error.isEmpty())
  {
    WARN_LOG_FMT(NETPLAY, "QuickPlay: NetPlay join failed");
    Fail(error);
    return;
  }
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
  m_matchmaker->Cancel();
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
  emit PrepareConnection(attempt, true);
}

void QuickPlayController::ConnectHost(std::uint64_t attempt)
{
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
  SetState(State::PublishingHostCode);
  INFO_LOG_FMT(NETPLAY, "QuickPlay: publishing host code");
  m_matchmaker->PublishHostCode(m_match_id, m_host_code);
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
