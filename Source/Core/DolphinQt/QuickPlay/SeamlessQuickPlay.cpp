// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/SeamlessQuickPlay.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <QApplication>
#include <QPointer>
#include <QTimer>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/SI/SI.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/ProjectPlusOnline/LocalCSS.h"
#include "Core/ProjectPlusOnline/LocalInput.h"
#include "Core/ProjectPlusOnline/SetupAction.h"
#include "Core/ProjectPlusOnline/Status.h"
#include "Core/QuickPlayRecipe.h"
#include "Core/System.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/HotkeyScheduler.h"
#include "DolphinQt/NetPlay/NetPlayDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QuickPlay/PersistentSession.h"
#include "DolphinQt/QuickPlay/QuickPlayLauncher.h"
#include "DolphinQt/QuickPlay/QuickPlaySession.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/Settings.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/Present.h"

namespace
{
// The trial declaration names the game list's own launcher entry, so the
// network-thread lookup compares the exact path string it will select.
static std::optional<QuickPlayContent::TrialLauncher>
QuickPlayTrialLauncher(const PersistentQuickPlaySession* self, const GameListModel& model)
{
  if (!self || !self->TrialLauncher())
    return std::nullopt;
  auto trial = *self->TrialLauncher();
  if (const auto launcher = FindPPlusNetplayLauncher(model).game)
    trial.path = launcher->GetFilePath();
  return trial;
}

// The single remote player of a two-player room, as untrusted plain text bounded
// for the compact in-game strip. Never the coordinator label, host role or slot.
static QString QuickPlayOpponentName(const std::optional<NetPlay::NetPlayClient::LobbyState>& lobby)
{
  if (!lobby || !lobby->connected || lobby->players.size() != 2)
    return {};
  const NetPlay::Player* opponent = nullptr;
  for (const auto& player : lobby->players)
  {
    if (player.pid == lobby->local_player)
      continue;
    if (opponent)
      return {};
    opponent = &player;
  }
  if (!opponent)
    return {};
  constexpr int MAX_DISPLAY_CHARACTERS = 20;
  QString name;
  int count = 0;
  for (const char32_t cp : QString::fromStdString(opponent->name).trimmed().toUcs4())
  {
    const auto category = QChar::category(cp);
    if (category == QChar::Other_Control || category == QChar::Other_Format ||
        category == QChar::Other_Surrogate || category == QChar::Separator_Line ||
        category == QChar::Separator_Paragraph)
      continue;
    if (count == MAX_DISPLAY_CHARACTERS)
    {
      name += QStringLiteral("…");
      break;
    }
    name += QString::fromUcs4(&cp, 1);
    ++count;
  }
  return name;
}
}  // namespace

SeamlessQuickPlay::SeamlessQuickPlay(QuickPlaySession& session, HotkeyScheduler* hotkey_scheduler)
    : m_session(session), m_window(session.m_window), m_system(session.m_system),
      m_dialog_parent(session.m_dialog_parent), m_netplay_dialog(session.m_netplay_dialog),
      m_game_list_model(session.m_game_list_model)
{
  qRegisterMetaType<ProjectPlusOnline::SetupCancelRequest>();
  connect(
      hotkey_scheduler, &HotkeyScheduler::CancelPausedQuickPlay, this,
      [this](ProjectPlusOnline::SetupCancelRequest request) {
        // Local controller 1, mapped or a Wii U adapter port; unsupported types are disconnected.
        const bool device_connected = ProjectPlusOnline::LocalCommandDeviceConnected();
        // Complete after releasing the input lock: a sampling gap while waiting revokes the ticket.
        const bool current = ProjectPlusOnline::setup_action.Complete(request);
        if (!current || !OwnsInGameQuickPlay() || !IsPersistentQuickPlayBoundary() ||
            request.context.identity != QuickPlayIdentity() || m_quickplay_validating ||
            m_quickplay_blocking_connection || m_quickplay_paused_css != request.context.identity ||
            !m_session.m_quickplay_setup ||
            m_session.m_quickplay_setup->attempt != request.context.identity.attempt ||
            m_session.m_quickplay_setup->window != m_window.GetRenderWidget() ||
            m_session.m_quickplay_setup->native_window !=
                static_cast<u64>(m_window.GetRenderWidget()->effectiveWinId()) ||
            m_session.m_quickplay_setup->client != Settings::Instance().GetNetPlayClient().get() ||
            m_session.m_quickplay_setup->server != Settings::Instance().GetNetPlayServer().get() ||
            QApplication::activeModalWidget() || m_window.IsStopConfirmShowing() ||
            !Host::GetInstance()->GetRenderFocus() ||
            ProjectPlusOnline::host_ui_input_blocked.load() || !device_connected ||
            !m_session.m_quickplay_controller ||
            m_session.m_quickplay_controller->GetGuestCancelAttempt() !=
                request.context.identity.attempt)
          return;
        m_quickplay_cancel_consumed = true;
        ProjectPlusOnline::setup_action.Invalidate();
        m_session.m_quickplay_controller->Cancel();
        PublishQuickPlayStatus();
      },
      Qt::QueuedConnection);
  connect(
      hotkey_scheduler, &HotkeyScheduler::LeaveQuickPlaySession, this,
      [this](ProjectPlusOnline::SetupCancelRequest request) {
        // Local controller 1 held L + R + D-pad Right; revalidate the owned session first.
        const bool device_connected = ProjectPlusOnline::LocalCommandDeviceConnected();
        const bool current = ProjectPlusOnline::leave_action.Complete(request);
        if (!current || !OwnsInGameQuickPlay() || request.context.identity != QuickPlayIdentity() ||
            m_quickplay_validating || m_quickplay_blocking_connection ||
            m_session.m_quickplay_recovery != QuickPlaySession::QuickPlayRecovery::None ||
            !Core::IsRunning(m_system) || QApplication::activeModalWidget() ||
            m_window.IsStopConfirmShowing() ||
            (!Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT) &&
             !Host::GetInstance()->GetRenderFocus()) ||
            ProjectPlusOnline::host_ui_input_blocked.load() || !device_connected)
          return;
        ProjectPlusOnline::leave_action.Invalidate();
        INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: local leave chord, game {}",
                     m_persistent_game);
        m_quickplay_left_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        m_window.NetPlayQuit();
        PublishQuickPlayStatus();
      },
      Qt::QueuedConnection);
  // The Core host job already marshals to this thread and validates its lifetime.
  // Do not queue again: a queued signal could outlive reset/load invalidation.
  connect(Host::GetInstance(), &Host::RequestQuickPlay, this,
          [this] { m_session.StartQuickPlayFrom(ProjectPlusOnline::EntrySource::InGame); });
  connect(
      Host::GetInstance(), &Host::RequestQuickPlayCancel, this,
      [this] {
        // Synchronous GUI delivery from the guarded Core host job. No event-loop
        // boundary between its identity check and the canonical desktop Cancel.
        const auto attempt = ProjectPlusOnline::guest_cancel_attempt.load();
        if (attempt && m_session.m_quickplay_controller &&
            m_session.m_quickplay_controller->GetGuestCancelAttempt() == attempt)
        {
          m_session.m_quickplay_controller->Cancel();
          PublishQuickPlayStatus();
        }
      },
      Qt::DirectConnection);

  connect(m_netplay_dialog, &NetPlayDialog::InGameError, this, [this](const QString& error) {
    m_session.ReportQuickPlayError(error);
    const auto identity = QuickPlayIdentity();
    if (m_quickplay_blocking_connection)
      return;  // Synchronous caller returns the retained cause after construction.
    QTimer::singleShot(0, this, [this, identity, error] {
      if (identity != QuickPlayIdentity() || !OwnsInGameQuickPlay())
        return;
      if (m_session.m_quickplay_controller &&
          m_session.m_quickplay_controller->OwnsAttempt(identity.attempt))
        m_session.m_quickplay_controller->OnLaunchFailed(identity.attempt, error);
      else if (m_persistent_quickplay)
        RetirePersistentQuickPlay(true);
    });
  });
  connect(m_netplay_dialog, &NetPlayDialog::PersistentStart, this,
          &SeamlessQuickPlay::PreparePersistentQuickPlay);
  connect(m_netplay_dialog, &NetPlayDialog::PersistentBarrier, this,
          &SeamlessQuickPlay::HandlePersistentBarrier);

  // Offline Launcher boots from this frontend prepare the in-game Quick Play origin.
  QuickPlayRecipe::SetFrontendSupported(true);
  QuickPlayRecipe::SetLocalSnapshotHandler([this] { TakeLocalQuickPlaySnapshot(); });

  auto* status_timer = new QTimer(this);
  connect(status_timer, &QTimer::timeout, this, &SeamlessQuickPlay::PublishQuickPlayStatus);
  status_timer->start(100);
  PublishQuickPlayStatus();
}

SeamlessQuickPlay::~SeamlessQuickPlay() = default;

bool SeamlessQuickPlay::IsSetupPaused() const
{
  return m_quickplay_validating || m_persistent_retiring ||
         (m_persistent_quickplay && m_session.m_quickplay_setup);
}

std::string SeamlessQuickPlay::OriginDigest() const
{
  // Only a ready recipe means a seamless search: the origin outlives a stopped game.
  return QuickPlayRecipe::IsReady() ? QuickPlayRecipe::GetOrigin().digest : std::string();
}

std::optional<QuickPlayContent::TrialLauncher> SeamlessQuickPlay::TrialLauncher() const
{
  return QuickPlayTrialLauncher(m_persistent_quickplay.get(), m_game_list_model);
}

std::optional<NetPlay::SyncIdentifier> SeamlessQuickPlay::DeclaredGame() const
{
  if (m_persistent_quickplay && m_persistent_quickplay->TrialLauncher())
    return m_persistent_quickplay->TrialLauncher()->identity;
  return std::nullopt;
}

QString SeamlessQuickPlay::RoomGameMismatch() const
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!m_persistent_quickplay || !client)
    return {};
  for (const auto& player : client->GetLobbyState().players)
  {
    if (player.game_status != NetPlay::SyncIdentifierComparison::SameGame &&
        player.game_status != NetPlay::SyncIdentifierComparison::Unknown)
    {
      INFO_LOG_FMT(NETPLAY, "QuickPlay: player {} reports a different room game ({})", player.pid,
                   static_cast<int>(player.game_status));
      return tr("Your opponent is on a different Quick Play build.");
    }
  }
  return {};
}

void SeamlessQuickPlay::Shutdown()
{
  InvalidateQuickPlayPresentation();
  if (m_persistent_quickplay)
    RetirePersistentQuickPlay(false);
}

void SeamlessQuickPlay::OnEmulationStateChanged(Core::State state)
{
  if (state == Core::State::Stopping && m_persistent_quickplay)
    RetirePersistentQuickPlay(false);
}

void SeamlessQuickPlay::OnRequestStop()
{
  // Revoke the deferred admission before a confirmation dialog can run its
  // nested event loop. Restore only the pause owned by this pending request.
  if (m_quickplay_validating)
  {
    CancelQuickPlayValidation();
    InvalidateQuickPlayPresentation();
  }
  ProjectPlusOnline::presentation.Invalidate();
}

bool SeamlessQuickPlay::HandleRequestStop()
{
  if (!m_persistent_quickplay && !m_persistent_retiring)
    return false;
  // Explicit application Stop/Close retires the session and stops this Core;
  // only a room/session leave performs clean local CSS recovery.
  m_window.ForceStop();
  return true;
}

void SeamlessQuickPlay::OnForceStop()
{
  InvalidateQuickPlayPresentation();
  if (m_persistent_quickplay || m_persistent_retiring)
    RetirePersistentQuickPlay(false);
}

void SeamlessQuickPlay::OnReset()
{
  CancelQuickPlayValidation();
  InvalidateQuickPlayPresentation();
}

bool SeamlessQuickPlay::HandleNetPlayStop()
{
  if (!m_persistent_quickplay && !m_persistent_retiring)
    return false;
  // A peer stop with no captured failure: the opponent left (local leave and
  // Stop retire first, so their own stop echo arrives while retiring).
  if (m_persistent_quickplay && !m_persistent_retiring &&
      m_netplay_dialog->SessionError().isEmpty())
    m_quickplay_opponent_left = true;
  RetirePersistentQuickPlay(true);
  return true;
}

bool SeamlessQuickPlay::HandleNetPlayQuit()
{
  if (m_session.m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame)
  {
    const auto cause = m_netplay_dialog->SessionError();
    if (!cause.isEmpty())
      m_session.ReportQuickPlayError(cause);
  }
  if (m_persistent_quickplay || m_persistent_retiring)
  {
    RetirePersistentQuickPlay(true);
    return true;
  }
  InvalidateQuickPlayPresentation();
  return false;
}

void SeamlessQuickPlay::OnClientCreated()
{
  if (m_persistent_quickplay)
  {
    m_persistent_client = m_session.m_quickplay_setup->client;
    m_persistent_server = m_session.m_quickplay_setup->server;
  }
}

void SeamlessQuickPlay::OnError(const QString& error)
{
  m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
  PublishQuickPlayStatus();
  const bool renderer_route =
      Core::IsRunning(m_system) && !m_system.IsDualCoreMode() &&
      PersistentQuickPlaySession::IsConfigured() && QuickPlaySession::IsQuickPlayWindowSystem() &&
      !m_window.IsRenderingToMain() && m_window.GetRenderWidget()->isVisible();
  if (m_session.m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame && !error.isEmpty() &&
      !renderer_route && !m_window.IsExitRequested() && !m_window.IsBootPending() &&
      Core::GetState(m_system) != Core::State::Stopping)
    m_netplay_dialog->ShowDiagnostics();
}

void SeamlessQuickPlay::OnShowDiagnostics()
{
  ProjectPlusOnline::setup_action.Invalidate();
}

bool SeamlessQuickPlay::StartSearch()
{
  m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
  m_quickplay_cancelled_until = {};
  m_quickplay_left_until = {};
  m_quickplay_opponent_left_until = {};
  ++m_quickplay_room_generation;
  if (!Core::IsRunning(m_system) ||
      (!PersistentQuickPlaySession::IsConfigured() &&
       QuickPlayContent::SelectedPolicy() != QuickPlayContent::Policy::DeclaredBundleTrialV1))
    return false;
  ValidateQuickPlayContent(0, [this](bool valid, QString error) {
    if (!valid)
    {
      m_session.ReportQuickPlayError(error);
      PublishQuickPlayStatus();
      if (m_session.m_quickplay_entry == ProjectPlusOnline::EntrySource::Desktop)
        ModalMessageBox::critical(m_dialog_parent, tr("Quick Play"),
                                  m_session.m_quickplay_admission_error);
      return;
    }
    m_session.BeginQuickPlaySearch();
  });
  return true;
}

void SeamlessQuickPlay::OnStateChanged(QuickPlayController::State state,
                                       QuickPlayController::State old_state)
{
  if (state == QuickPlayController::State::Searching)
    m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
  if (state == QuickPlayController::State::Idle)
  {
    m_quickplay_cancelled_until = old_state != QuickPlayController::State::NetPlayOwned &&
                                          old_state != QuickPlayController::State::Error ?
                                      std::chrono::steady_clock::now() + std::chrono::seconds(2) :
                                      std::chrono::steady_clock::time_point{};
  }
  PublishQuickPlayStatus();
}

void SeamlessQuickPlay::OnHandoff()
{
  ProjectPlusOnline::presentation.Invalidate();
  ProjectPlusOnline::setup_action.Invalidate();
  ProjectPlusOnline::leave_action.Invalidate();
  m_quickplay_paused_css.reset();
  ++m_quickplay_room_generation;
}

bool SeamlessQuickPlay::PrepareHandoff(u64 attempt)
{
  if (!m_persistent_candidate && !PersistentQuickPlaySession::IsConfigured() &&
      QuickPlayContent::SelectedPolicy() != QuickPlayContent::Policy::DeclaredBundleTrialV1)
    return false;
  ValidateQuickPlayContent(attempt, [this, attempt](bool valid, QString error) {
    if (!m_session.m_quickplay_controller->OwnsAttempt(attempt))
      return;
    if (!valid)
    {
      m_session.m_quickplay_controller->OnHandoffReady(attempt, error);
      return;
    }
    m_persistent_quickplay = std::move(m_persistent_candidate);
    m_session.m_quickplay_retain_window = true;
    m_session.m_quickplay_setup->window = m_window.GetRenderWidget();
    m_session.m_quickplay_setup->native_window =
        static_cast<u64>(m_window.GetRenderWidget()->winId());
    ProjectPlusOnline::persistent_local_input = false;
    ++ProjectPlusOnline::persistent_input_epoch;
    const auto* session_owner = m_persistent_quickplay.get();
    const auto request = m_quickplay_validation_request;
    const auto room = m_quickplay_room_generation;
    m_persistent_quickplay->Restore(m_system, [self = QPointer<SeamlessQuickPlay>(this), attempt,
                                               session_owner, request, room](bool ok) {
      if (!self || self->m_persistent_quickplay.get() != session_owner ||
          self->m_persistent_retiring || request != self->m_quickplay_validation_request ||
          room != self->m_quickplay_room_generation ||
          !self->m_session.m_quickplay_controller->OwnsAttempt(attempt))
        return;
      self->m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
      INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: paused handoff attempt {} restored {}", attempt,
                   ok);
      if (ok)
        self->ObservePausedQuickPlayCSS();
      self->m_session.m_quickplay_controller->OnHandoffReady(
          attempt, ok ? QString{} : tr("The common CSS could not be restored."));
    });
  });
  return true;
}

void SeamlessQuickPlay::PrepareConnection(std::function<bool(bool)> connect)
{
  m_quickplay_blocking_connection = true;
  ProjectPlusOnline::setup_action.Invalidate();
  PublishQuickPlayStatus();
  const auto fence = ProjectPlusOnline::PrepaintFence{
      QuickPlayIdentity(), ++m_quickplay_validation_request,
      static_cast<std::uintptr_t>(m_window.GetRenderWidget()->effectiveWinId())};
  const auto connect_room = [this, fence, connect = std::move(connect)](bool painted) {
    const auto current = ProjectPlusOnline::PrepaintFence{
        QuickPlayIdentity(), m_quickplay_validation_request,
        static_cast<std::uintptr_t>(m_window.GetRenderWidget()->effectiveWinId())};
    if (fence != current)
      return;
    const bool supported =
        !m_persistent_quickplay ||
        (IsPersistentQuickPlayBoundary() && !m_system.IsDualCoreMode() &&
         QuickPlaySession::IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
         m_window.GetRenderWidget()->isVisible());
    if (!connect(painted && supported))
      return;
    m_quickplay_blocking_connection = false;
    ProjectPlusOnline::setup_action.Invalidate();  // A fresh B release is required after blocking.
    PublishQuickPlayStatus();
  };
  if (m_persistent_quickplay)
    RedrawQuickPlayPresentation(connect_room);
  else
    connect_room(true);  // Ordinary Desktop setup retains its synchronous behavior.
}

bool SeamlessQuickPlay::CancelHost(u64 attempt)
{
  const auto& setup = m_session.m_quickplay_setup;
  if (m_quickplay_validating && setup && setup->attempt == attempt)
    CancelQuickPlayValidation();
  if (m_persistent_quickplay && setup && setup->attempt == attempt)
  {
    RetirePersistentQuickPlay(true);
    return true;
  }
  return false;
}

void SeamlessQuickPlay::PublishQuickPlayStatus()
{
  ProjectPlusOnline::persistent_setup_pause =
      m_quickplay_validating || m_persistent_retiring ||
      (m_persistent_quickplay && m_session.m_quickplay_setup);
  Common::ScopeGuard publish_presentation([this] { PublishQuickPlayPresentation(); });
  using ProjectPlusOnline::GuestStatus;
  using State = QuickPlayController::State;
  ProjectPlusOnline::persistent_local_input =
      PersistentQuickPlaySession::IsConfigured() && !m_persistent_quickplay &&
      !m_persistent_retiring && !NetPlay::IsNetPlayRunning() && !m_window.IsExitRequested() &&
      !m_quickplay_validating;
  const auto publish = [this](GuestStatus status) {
    const auto attempt =
        m_session.m_quickplay_controller &&
                m_session.m_quickplay_recovery == QuickPlaySession::QuickPlayRecovery::None &&
                !m_quickplay_validating && !m_quickplay_blocking_connection &&
                !m_persistent_retiring && ProjectPlusOnline::IsCancellable(status) ?
            m_session.m_quickplay_controller->GetGuestCancelAttempt() :
            0;
    if (ProjectPlusOnline::guest_cancel_attempt.load() != attempt)
      ProjectPlusOnline::guest_cancel_attempt = 0;
    ProjectPlusOnline::guest_cancel_attempt = attempt;
  };
  if (m_session.m_quickplay_recovery != QuickPlaySession::QuickPlayRecovery::None)
    return publish(GuestStatus::Unavailable);
  const auto client = Settings::Instance().GetNetPlayClient();
  if (client)
  {
    const auto lobby = client->GetLobbyState();
    if (lobby.connected && lobby.players.size() >= 2)
      return publish(GuestStatus::Connected);
  }
  if (client || Settings::Instance().GetNetPlayServer() || m_session.m_quickplay_session)
    return publish(GuestStatus::Connecting);
  if (m_session.m_quickplay_stop_attempt && m_session.m_quickplay_setup &&
      m_session.m_quickplay_controller->OwnsAttempt(m_session.m_quickplay_stop_attempt))
  {
    if (m_session.m_quickplay_presentation == QuickPlaySession::QuickPlayPresentation::Matched)
      return publish(GuestStatus::Matched);
    if (m_session.m_quickplay_presentation == QuickPlaySession::QuickPlayPresentation::Connecting)
      return publish(GuestStatus::Connecting);
  }
  const auto state =
      m_session.m_quickplay_controller ? m_session.m_quickplay_controller->GetState() : State::Idle;
  switch (state)
  {
  case State::Idle:
    if (Core::GetState(m_system) == Core::State::Stopping)
      return publish(GuestStatus::Unavailable);
    return publish(m_session.m_quickplay_setup ? GuestStatus::Connecting : GuestStatus::Idle);
  case State::Searching:
    return publish(GuestStatus::Searching);
  case State::MatchedHost:
  case State::MatchedClient:
    return publish(GuestStatus::Matched);
  case State::Error:
    return publish(GuestStatus::Error);
  case State::NetPlayConnected:
  case State::NetPlayOwned:
    return publish(GuestStatus::Unavailable);
  default:
    return publish(GuestStatus::Connecting);
  }
}

void SeamlessQuickPlay::InvalidateQuickPlayPresentation()
{
  ProjectPlusOnline::presentation.Invalidate();
  ProjectPlusOnline::setup_action.Invalidate();
  ProjectPlusOnline::leave_action.Invalidate();
  m_quickplay_paused_css.reset();
  ++m_quickplay_room_generation;
  ++m_quickplay_validation_request;
  m_quickplay_validating = false;
  m_quickplay_blocking_connection = false;
  m_quickplay_validation_fence.reset();
  m_session.m_quickplay_admission_error.clear();
  m_quickplay_cancelled_until = {};
}

void SeamlessQuickPlay::CancelQuickPlayValidation()
{
  if (!m_quickplay_validating || !m_quickplay_validation_fence)
    return;
  const auto fence = *m_quickplay_validation_fence;
  ++m_quickplay_validation_request;
  m_quickplay_validating = false;
  m_quickplay_validation_fence.reset();
  ProjectPlusOnline::presentation.Invalidate();
  if (m_quickplay_validation_was_running &&
      fence.identity.core == ProjectPlusOnline::presentation_core_generation.load() &&
      fence.identity.input == ProjectPlusOnline::persistent_input_epoch.load() &&
      fence.window == static_cast<std::uintptr_t>(m_window.GetRenderWidget()->effectiveWinId()) &&
      Core::GetState(m_system) == Core::State::Paused)
    Core::SetState(m_system, Core::State::Running);
}

ProjectPlusOnline::PresentationIdentity SeamlessQuickPlay::QuickPlayIdentity() const
{
  return {ProjectPlusOnline::presentation_core_generation.load(),
          ProjectPlusOnline::persistent_input_epoch.load(), m_session.m_quickplay_owned_attempt,
          m_quickplay_room_generation, m_persistent_game};
}

bool SeamlessQuickPlay::OwnsInGameQuickPlay() const
{
  return m_session.m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame &&
         m_quickplay_owner_core == ProjectPlusOnline::presentation_core_generation.load() &&
         m_persistent_quickplay && !m_persistent_retiring && m_session.m_quickplay_owned_attempt &&
         m_persistent_client == Settings::Instance().GetNetPlayClient().get() &&
         m_persistent_server == Settings::Instance().GetNetPlayServer().get();
}

void SeamlessQuickPlay::ObservePausedQuickPlayCSS()
{
  m_quickplay_paused_css.reset();
  ProjectPlusOnline::setup_action.Invalidate();
  const auto identity = QuickPlayIdentity();
  const auto self = QPointer<SeamlessQuickPlay>(this);
  Core::RunOnCPUThread(m_system, [self, identity, &system = m_system] {
    if (identity.core != ProjectPlusOnline::presentation_core_generation.load() ||
        identity.input != ProjectPlusOnline::persistent_input_epoch.load() ||
        Core::GetState(system) != Core::State::Paused)
      return;
    const bool css =
        ProjectPlusOnline::ObserveLocalCSS(system) && !system.GetMovie().IsMovieActive();
    QueueOnObject(QApplication::instance(), [self, identity, css] {
      if (!self || identity != self->QuickPlayIdentity() || !self->IsPersistentQuickPlayBoundary())
        return;
      if (css)
        self->m_quickplay_paused_css = identity;
      self->PublishQuickPlayStatus();
    });
  });
}

void SeamlessQuickPlay::PublishQuickPlayPresentation()
{
  using namespace ProjectPlusOnline;
  using State = QuickPlayController::State;
  PresentationSnapshot value;
  const auto observed_revision = presentation.Revision();
  const auto core_state = Core::GetState(m_system);
  const auto core = presentation_core_generation.load();
  const auto input = persistent_input_epoch.load();
  value.identity = QuickPlayIdentity();
  value.entry = m_session.m_quickplay_entry;
  const auto client = Settings::Instance().GetNetPlayClient();
  const auto lobby = client ? std::optional{client->GetLobbyState()} : std::nullopt;
  const bool disconnected = lobby && !lobby->connected;
  const bool supported = Core::IsRunning(m_system) && PersistentQuickPlaySession::IsConfigured();
  const bool eligible = local_css.Read().IsFresh(core, input, std::chrono::steady_clock::now()) &&
                        QuickPlaySession::IsQuickPlayWindowSystem() &&
                        !m_window.IsRenderingToMain() && m_window.GetRenderWidget()->isVisible() &&
                        !client && !Settings::Instance().GetNetPlayServer();
  const auto state =
      m_session.m_quickplay_controller ? m_session.m_quickplay_controller->GetState() : State::Idle;
  const bool paused_action =
      OwnsInGameQuickPlay() && IsPersistentQuickPlayBoundary() &&
      m_quickplay_paused_css == value.identity && m_session.m_quickplay_setup &&
      m_session.m_quickplay_setup->attempt == value.identity.attempt &&
      m_session.m_quickplay_setup->window == m_window.GetRenderWidget() &&
      m_session.m_quickplay_setup->native_window ==
          static_cast<u64>(m_window.GetRenderWidget()->effectiveWinId()) &&
      m_session.m_quickplay_controller &&
      m_session.m_quickplay_controller->OwnsAttempt(value.identity.attempt) &&
      m_session.m_quickplay_controller->GetGuestCancelAttempt() == value.identity.attempt &&
      guest_cancel_attempt.load() == value.identity.attempt &&
      (!lobby || (lobby->connected && lobby->current_game == value.identity.game)) && supported &&
      QuickPlaySession::IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
      m_window.GetRenderWidget()->isVisible() && !m_window.IsExitRequested() &&
      !m_window.IsBootPending() && !m_window.IsStopConfirmShowing() &&
      !QApplication::activeModalWidget() && !m_quickplay_validating &&
      !m_quickplay_blocking_connection &&
      m_session.m_quickplay_recovery == QuickPlaySession::QuickPlayRecovery::None;
  const auto action = setup_action.Publish(value.identity, paused_action);
  // Leave is available for the whole owned in-game session once the opponent has
  // joined: paused setup, shared CSS, stage select, gameplay and results.
  const bool leave_action_enabled =
      OwnsInGameQuickPlay() && lobby && lobby->connected && lobby->players.size() >= 2 &&
      supported && QuickPlaySession::IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
      m_window.GetRenderWidget()->isVisible() && !m_window.IsExitRequested() &&
      !m_window.IsBootPending() && !m_window.IsStopConfirmShowing() &&
      !QApplication::activeModalWidget() && !m_quickplay_validating &&
      !m_quickplay_blocking_connection &&
      m_session.m_quickplay_recovery == QuickPlaySession::QuickPlayRecovery::None &&
      (core_state == Core::State::Running || core_state == Core::State::Paused);
  leave_action.Publish(value.identity, leave_action_enabled);
  const QString opponent = QuickPlayOpponentName(lobby);
  const bool left = std::chrono::steady_clock::now() < m_quickplay_left_until;
  const bool opponent_left = std::chrono::steady_clock::now() < m_quickplay_opponent_left_until;
  QString text;
  const auto recipe = QuickPlayRecipe::GetPhase();
  if (recipe != QuickPlayRecipe::Phase::Failed)
    m_quickplay_recipe_failure_until.reset();
  else if (!m_quickplay_recipe_failure_until)
    m_quickplay_recipe_failure_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  if (!supported && Core::IsRunning(m_system) && !m_window.IsExitRequested() &&
      recipe == QuickPlayRecipe::Phase::Preparing)
  {
    // Search stays unavailable until this Core has captured its origin.
    value.phase = PresentationPhase::Checking;
    text = tr("Preparing Quick Play…");
  }
  else if (!supported && Core::IsRunning(m_system) && !m_window.IsExitRequested() &&
           recipe == QuickPlayRecipe::Phase::Failed &&
           std::chrono::steady_clock::now() < *m_quickplay_recipe_failure_until)
  {
    value.phase = PresentationPhase::Error;
    text = tr("Quick Play unavailable: %1. Local play continues.")
               .arg(QString::fromStdString(QuickPlayRecipe::FailureReason()));
  }
  else if (supported && !m_window.IsExitRequested() && m_persistent_retiring &&
           (left || m_quickplay_opponent_left))
  {
    value.phase = PresentationPhase::Setup;
    text = left ? tr("Leaving…") : tr("Opponent left the session");
  }
  else if (supported && !m_window.IsExitRequested() && !m_persistent_retiring &&
           m_session.m_quickplay_recovery == QuickPlaySession::QuickPlayRecovery::None)
  {
    if (m_quickplay_validating)
    {
      value.phase = PresentationPhase::Checking;
      text = QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::Strict ?
                 tr("Checking game files…") :
                 tr("Preparing session…");
    }
    else if (!m_session.m_quickplay_admission_error.isEmpty() &&
             value.entry == EntrySource::InGame && m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Error;
      text = m_session.m_quickplay_admission_error + tr("  Tools > Quick Play Diagnostics");
    }
    else if (state == State::Error && value.entry == EntrySource::InGame &&
             m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Error;
      text = m_session.m_quickplay_controller->GetErrorMessage();
    }
    else if (!disconnected && state != State::Idle && value.entry == EntrySource::InGame &&
             m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Setup;
      switch (state)
      {
      case State::Searching:
      {
        value.phase = PresentationPhase::Searching;
        // The regions captured when this attempt started, not the current setting.
        const auto& regions = m_session.m_quickplay_controller->GetRegions();
        const auto backend = m_session.m_quickplay_controller->GetBackendLabel();
        // Kept short: the overlay has two lines, and the cancel hint follows the text.
        const auto stage = m_session.m_quickplay_controller->GetSearchStage();
        const QString where = regions.size() == 1 ?
                                  QuickPlayMatchmaker::GetRegionLabel(regions.front()) :
                                  tr("%n regions", "", regions.size());
        if (!m_session.m_quickplay_controller->GetSearchNotice().isEmpty())
          text = m_session.m_quickplay_controller->GetSearchNotice();
        else if (regions.isEmpty())
          text = tr("Searching (%1)…").arg(backend);
        else if (stage == QuickPlayMatchmaker::SearchStage::JoinOnly)
          text = tr("Looking for a host in %1 (%2)… %3 s")
                     .arg(where, backend)
                     .arg(m_session.m_quickplay_controller->GetSearchStageSeconds());
        else if (stage == QuickPlayMatchmaker::SearchStage::JoinListed)
          text = tr("Searching in %1 (%2), now listed…").arg(where, backend);
        else
          text = tr("Searching in %1 (%2)…").arg(where, backend);
        break;
      }
      case State::MatchedHost:
      case State::MatchedClient:
      case State::PreparingHandoff:
      case State::WaitingForHandoff:
        text = tr("Opponent found. Preparing session…");
        break;
      case State::ValidatingMatch:
        text = tr("Checking match…");
        break;
      case State::CreatingHost:
        text = tr("Creating room…");
        break;
      case State::WaitingForTraversalCode:
      case State::PublishingHostCode:
        text = tr("Waiting for connection…");
        break;
      case State::WaitingForOpponent:
        text = tr("Waiting for opponent…");
        break;
      case State::WaitingForHost:
        text = tr("Waiting for host…");
        break;
      case State::Connecting:
        text = tr("Connecting…");
        break;
      case State::NetPlayConnected:
        text = tr("Preparing shared session…");
        break;
      case State::NetPlayOwned:
        if (m_persistent_quickplay && m_persistent_game && lobby &&
            lobby->current_game == m_persistent_game && NetPlay::IsNetPlayRunning())
        {
          value.phase = PresentationPhase::Active;
          text = opponent.isEmpty() ? tr("Online session") : tr("vs %1").arg(opponent);
        }
        else
          value.phase = PresentationPhase::Hidden;
        break;
      default:
        value.phase = PresentationPhase::Hidden;
        break;
      }
      value.cancel =
          CanPresentCancel(core_state == Core::State::Running, eligible,
                           !m_quickplay_blocking_connection && !m_session.m_quickplay_setup &&
                               guest_cancel_attempt.load() == value.identity.attempt,
                           m_session.m_quickplay_controller->GetGuestCancelAttempt()) ||
          setup_action.Available(action);
      if (m_quickplay_blocking_connection)
        text = tr("Connecting to room…");
      // Once the opponent has joined, setup status names them as well.
      if (value.phase == PresentationPhase::Setup && !opponent.isEmpty())
        text = tr("vs %1").arg(opponent) + QStringLiteral("   ") + text;
      value.leave = leave_action_enabled && value.phase != PresentationPhase::Hidden;
    }
    else if (eligible && core_state == Core::State::Running)
    {
      value.phase = PresentationPhase::LocalCSS;
      text = tr("L + R + D-pad Up — Search");
      if (left || opponent_left)
      {
        value.phase = PresentationPhase::Cancelled;
        text = left ? tr("Left session") : tr("Opponent left the session");
      }
      else if (std::chrono::steady_clock::now() < m_quickplay_cancelled_until)
      {
        value.phase = PresentationPhase::Cancelled;
        text = tr("Cancelled");
      }
    }
  }
  // Bound plain UTF-8 on complete code points; strip control/format characters.
  const auto copy_text = [](const QString& source, auto& target) {
    std::size_t offset = 0;
    for (const auto cp : source.toUcs4())
    {
      const auto category = QChar::category(cp);
      if (category == QChar::Other_Control || category == QChar::Other_Format ||
          category == QChar::Other_Surrogate || category == QChar::Separator_Line ||
          category == QChar::Separator_Paragraph)
        continue;
      const char32_t scalar = cp;
      const auto bytes = QString::fromUcs4(&scalar, 1).toUtf8();
      if (offset + bytes.size() >= target.size())
        break;
      std::copy(bytes.begin(), bytes.end(), target.begin() + offset);
      offset += bytes.size();
    }
  };
  copy_text(text, value.text);
  if (value.cancel)
    copy_text(tr("B — Cancel"), value.cancel_text);
  if (value.leave)
    copy_text(tr("Hold L + R + D-pad Right — Leave"), value.leave_text);
  if (presentation.Publish(value, observed_revision) && core_state == Core::State::Paused &&
      supported)
    RedrawQuickPlayPresentation();
}

void SeamlessQuickPlay::RedrawQuickPlayPresentation(std::function<void(bool)> finished)
{
  if (finished)
    m_quickplay_redraw_callbacks.push_back(std::move(finished));
  if (m_quickplay_redraw_pending)
    return;
  m_quickplay_redraw_pending = true;
  const auto core = ProjectPlusOnline::presentation_core_generation.load();
  const auto input = ProjectPlusOnline::persistent_input_epoch.load();
  const auto revision = ProjectPlusOnline::presentation.Revision();
  const auto identity = QuickPlayIdentity();
  const auto native = m_window.GetRenderWidget()->effectiveWinId();
  const auto widget = QPointer<RenderWidget>(m_window.GetRenderWidget());
  const auto self = QPointer<SeamlessQuickPlay>(this);
  Core::RunOnCPUThread(m_system, [self, widget, native, core, input, revision, identity] {
    const bool ok = ProjectPlusOnline::presentation_core_generation.load() == core &&
                    ProjectPlusOnline::persistent_input_epoch.load() == input &&
                    ProjectPlusOnline::presentation.Revision() == revision && g_presenter &&
                    g_presenter->RedrawRetainedFrame();
    QueueOnObject(
        QApplication::instance(), [self, widget, native, core, input, revision, identity, ok] {
          if (!self)
            return;
          self->m_quickplay_redraw_pending = false;
          auto callbacks = std::move(self->m_quickplay_redraw_callbacks);
          self->m_quickplay_redraw_callbacks.clear();
          const bool current = ok && widget && self->m_window.GetRenderWidget() == widget &&
                               widget->effectiveWinId() == native &&
                               ProjectPlusOnline::presentation_core_generation.load() == core &&
                               ProjectPlusOnline::persistent_input_epoch.load() == input &&
                               ProjectPlusOnline::presentation.Revision() == revision &&
                               self->QuickPlayIdentity() == identity;
          // Coalesce changed presentation while keeping every caller's own request fences.
          if (!current && widget && self->m_window.GetRenderWidget() == widget &&
              widget->effectiveWinId() == native && self->QuickPlayIdentity() == identity &&
              ProjectPlusOnline::presentation.Revision() != revision)
          {
            self->m_quickplay_redraw_callbacks = std::move(callbacks);
            self->RedrawQuickPlayPresentation();
            return;
          }
          for (auto& callback : callbacks)
            callback(current);
        });
  });
}

void SeamlessQuickPlay::ValidateQuickPlayContent(u64 attempt,
                                                 std::function<void(bool, QString)> finished)
{
  if (m_system.IsDualCoreMode() || !QuickPlaySession::IsQuickPlayWindowSystem() ||
      m_window.IsRenderingToMain() || !m_window.GetRenderWidget()->isVisible())
  {
    finished(false, tr("Quick Play requires single-core mode and a separate render window."));
    return;
  }
  const auto previous = Core::GetState(m_system);
  const auto request = ++m_quickplay_validation_request;
  const auto core = ProjectPlusOnline::presentation_core_generation.load();
  const auto input = ProjectPlusOnline::persistent_input_epoch.load();
  const auto room = m_quickplay_room_generation;
  const auto widget = QPointer<RenderWidget>(m_window.GetRenderWidget());
  const auto native = widget->effectiveWinId();
  const ProjectPlusOnline::PrepaintFence fence{{core, input, attempt, room, m_persistent_game},
                                               request,
                                               static_cast<std::uintptr_t>(native)};
  m_quickplay_validation_was_running = previous == Core::State::Running;
  m_quickplay_validation_fence = fence;
  m_quickplay_validating = true;
  const bool trial =
      QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::DeclaredBundleTrialV1;
  if (!trial)
    PublishQuickPlayStatus();
  Core::SetState(m_system, Core::State::Paused);
  auto admit = [this, previous, request, fence, widget, attempt,
                finished = std::move(finished)](bool painted) {
    if (request != m_quickplay_validation_request)
      return;
    const ProjectPlusOnline::PrepaintFence current_fence{
        {ProjectPlusOnline::presentation_core_generation.load(),
         ProjectPlusOnline::persistent_input_epoch.load(), m_session.m_quickplay_owned_attempt,
         m_quickplay_room_generation, m_persistent_game},
        m_quickplay_validation_request,
        static_cast<std::uintptr_t>(m_window.GetRenderWidget()->effectiveWinId())};
    const bool current = fence == current_fence && widget && widget == m_window.GetRenderWidget() &&
                         !m_window.IsExitRequested() && !m_window.IsBootPending() &&
                         Core::GetState(m_system) == Core::State::Paused &&
                         (!attempt || (m_session.m_quickplay_setup &&
                                       m_session.m_quickplay_setup->attempt == attempt &&
                                       m_session.m_quickplay_controller->OwnsAttempt(attempt)));
    m_quickplay_validating = false;
    m_quickplay_validation_fence.reset();
    if (!current)
    {
      // Never resume or admit a different Core/window/input lifetime.
      PublishQuickPlayStatus();
      return;
    }
    QString error;
    auto candidate = std::move(m_persistent_candidate);
    if (!candidate)
      candidate = std::make_unique<PersistentQuickPlaySession>();
    const bool dispatch_ready =
        !m_window.IsStopConfirmShowing() && !QApplication::activeModalWidget();
    const auto admitted_identity = candidate->Identity();
    bool valid = painted && dispatch_ready && candidate->Validate(m_system, &error);
    // The trial does not rescan content; it does refuse a declaration that
    // changed between the initial admission and this matched revalidation.
    if (valid && candidate->IsTrial() && attempt && !admitted_identity.empty() &&
        candidate->Identity() != admitted_identity)
    {
      valid = false;
      error = tr("The Quick Play bundle changed during this search. Cancel and try again.");
    }
    if (!dispatch_ready)
      error = tr("Close the current desktop dialog before starting Quick Play.");
    if (!painted)
      error = tr("Quick Play could not display the paused content check. Use desktop diagnostics.");
    if (valid)
      m_persistent_candidate = std::move(candidate);
    // A successful matched handoff retains its paused boundary; all other
    // outcomes restore exactly the state before validation.
    if (!valid || !attempt)
      Core::SetState(m_system, previous);
    finished(valid, std::move(error));
    PublishQuickPlayStatus();
  };
  // Trial admission has bounded IO only: no verification prepaint, but complete on a later
  // event-loop turn with the same pause/ownership fences, like the strict callback.
  if (trial)
    QTimer::singleShot(0, this, [admit = std::move(admit)] { admit(true); });
  else
    RedrawQuickPlayPresentation(std::move(admit));
}

bool SeamlessQuickPlay::IsPersistentQuickPlayBoundary() const
{
  return m_persistent_quickplay && !m_persistent_retiring &&
         Core::GetState(m_system) == Core::State::Paused;
}

void SeamlessQuickPlay::PreparePersistentQuickPlay(u32 game)
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !m_session.m_quickplay_setup || !IsPersistentQuickPlayBoundary() ||
      !m_session.IsQuickPlayLaunchReady(m_session.m_quickplay_setup->attempt))
  {
    m_session.ReportQuickPlayError(
        tr("The persistent session start lost its owned readiness boundary."));
    return RetirePersistentQuickPlay(true);
  }
  ProjectPlusOnline::presentation.Invalidate();
  m_persistent_game = game;
  ObservePausedQuickPlayCSS();
  PublishQuickPlayPresentation();
  const Core::CPUThreadGuard guard(m_system);
  if (!client->PreparePersistentSession(game, m_persistent_quickplay->Identity(),
                                        m_persistent_quickplay->OriginDigest(),
                                        m_persistent_quickplay->ContractText()))
    QTimer::singleShot(
        0, this,
        [this, game, mismatch = QString::fromStdString(client->PersistentConfigMismatch())] {
          if (m_persistent_game == game)
          {
            m_session.ReportQuickPlayError(
                mismatch.isEmpty() ?
                    tr("The persistent session contract could not be prepared.") :
                    tr("Quick Play refused the join: this setting differs from the host's: %1. "
                       "Change it back, or restart the Offline Launcher.")
                        .arg(mismatch));
            RetirePersistentQuickPlay(true);
          }
        });
}

void SeamlessQuickPlay::HandlePersistentBarrier(u32 game, bool release)
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !m_session.m_quickplay_setup || game != m_persistent_game ||
      !IsPersistentQuickPlayBoundary() || client.get() != m_session.m_quickplay_setup->client ||
      !client->IsConnected())
    return;
  bool ok;
  {
    const Core::CPUThreadGuard guard(m_system);
    ok = release ? client->ReleasePersistentSession(game) : client->AttachPersistentSession(game);
  }
  if (!ok)
  {
    m_session.ReportQuickPlayError(tr("The persistent session attach/release barrier failed."));
    return RetirePersistentQuickPlay(true);
  }
  if (!release)
    return;
  const auto attempt = m_session.m_quickplay_setup->attempt;
  m_session.m_quickplay_setup.reset();
  m_session.m_quickplay_host_attempt = 0;
  m_session.m_quickplay_join_attempt = 0;
  m_netplay_dialog->ReleaseQuickPlayStart();
  m_session.m_quickplay_controller->OnLaunchOwned(attempt);
  Core::SetState(m_system, Core::State::Running);
  INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: active game {} same Core", game);
}

void SeamlessQuickPlay::RetirePersistentQuickPlay(bool restore_css)
{
  if (m_persistent_retiring)
  {
    if (!restore_css)
      m_persistent_restore_css = false;
    return;
  }
  if (!m_persistent_quickplay)
    return;
  const auto delivered_cause = m_netplay_dialog->SessionError();
  const auto cause =
      delivered_cause.isEmpty() ? m_session.m_quickplay_admission_error : delivered_cause;
  InvalidateQuickPlayPresentation();
  m_session.m_quickplay_admission_error = cause;
  m_persistent_retiring = true;
  ProjectPlusOnline::persistent_setup_pause = true;
  m_persistent_restore_css =
      restore_css && !m_window.IsExitRequested() && !m_window.IsBootPending();
  ProjectPlusOnline::persistent_local_input = false;
  ++ProjectPlusOnline::persistent_input_epoch;
  const auto client = Settings::Instance().GetNetPlayClient();
  if (client)
  {
    client->RequestStopGame();
    // Do this BEFORE pause/CPU guard/global NetPlay lock: GetNetPads can hold
    // that lock while waiting for a remote packet on a stalled session.
    client->InvokeStop();
  }
  if (Core::IsRunning(m_system))
  {
    Core::SetState(m_system, Core::State::Paused);
    const Core::CPUThreadGuard guard(m_system);
    if (client)
      client->DetachPersistentSession();
    Config::RemoveLayer(Config::LayerType::Netplay);
    // Restore the local desired devices too, so no NetPlay device request is left for the
    // next local SI poll. A CSS restore then pins them to the checkpoint's devices.
    for (int port = 0; port < 4; ++port)
      m_system.GetSerialInterface().ChangeDevice(Config::Get(Config::GetInfoForSIDevice(port)),
                                                 port);
    Core::UpdateWantDeterminism(m_system);
  }
  else
  {
    // Stop/Close may already have put the Core in Stopping. InvokeStop above
    // releases its final pad wait; disable the global pointer before destroying
    // the client even though its running flag is now clear. Leave its FIFOs
    // intact until CPU teardown and object destruction have finished.
    NetPlay::NetPlay_Disable();
    Config::RemoveLayer(Config::LayerType::Netplay);
  }
  m_persistent_game = 0;
  m_persistent_client = nullptr;
  m_persistent_server = nullptr;
  m_session.m_quickplay_setup.reset();
  m_session.m_quickplay_host_attempt = 0;
  m_session.m_quickplay_join_attempt = 0;
  m_session.m_quickplay_retain_window = false;
  m_netplay_dialog->hide();
  m_netplay_dialog->ResetSession();
  m_session.m_quickplay_retiring_client = client;
  m_session.m_quickplay_retiring_server = Settings::Instance().GetNetPlayServer();
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();
  INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: retiring session; Core retained restore {}",
               m_persistent_restore_css);
  QTimer::singleShot(0, this, &SeamlessQuickPlay::FinishPersistentQuickPlayRetirement);
}

void SeamlessQuickPlay::FinishPersistentQuickPlayRetirement()
{
  if (!m_persistent_retiring)
    return;
  if (!m_session.m_quickplay_retiring_client.expired() ||
      !m_session.m_quickplay_retiring_server.expired())
  {
    QTimer::singleShot(25, this, &SeamlessQuickPlay::FinishPersistentQuickPlayRetirement);
    return;
  }
  // Fence notifications queued by the last network-thread destructor too.
  m_netplay_dialog->ResetSession();
  const bool restore =
      m_persistent_restore_css && Core::IsRunning(m_system) && !m_window.IsExitRequested();
  const auto finish = [self = QPointer<SeamlessQuickPlay>(this), restore](bool ok) {
    if (!self || !self->m_persistent_retiring)
      return;
    const bool resume = restore && ok && self->m_persistent_restore_css &&
                        Core::IsRunning(self->m_system) && !self->m_window.IsExitRequested();
    self->m_persistent_quickplay.reset();
    self->m_persistent_retiring = false;
    self->m_persistent_restore_css = false;
    if (self->m_session.m_quickplay_controller)
      self->m_session.m_quickplay_controller->Cancel();
    if (resume && self->m_quickplay_cancel_consumed)
      ProjectPlusOnline::cancel_release_handoff.Arm(
          ProjectPlusOnline::presentation_core_generation.load(),
          ProjectPlusOnline::persistent_input_epoch.load());
    self->m_quickplay_cancel_consumed = false;
    if (std::exchange(self->m_quickplay_opponent_left, false))
      self->m_quickplay_opponent_left_until =
          std::chrono::steady_clock::now() + std::chrono::seconds(4);
    if (resume)
    {
      {
        const Core::CPUThreadGuard guard(self->m_system);
        QuickPlayRecipe::ReleaseExtraPorts(self->m_system);
      }
      Core::SetState(self->m_system, Core::State::Running);
    }
    self->m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
    if (!ok)
      self->m_session.ReportQuickPlayError(self->m_session.m_quickplay_admission_error +
                                           tr(" The local CSS could not be restored."));
    if (!ok)
      ERROR_LOG_FMT(NETPLAY, "Persistent QuickPlay: local CSS restore failed; keeping Core paused");
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: session retired local CSS {}", resume);
    self->PublishQuickPlayStatus();
  };
  if (!restore)
    return finish(true);
  // Leave returns to this run's hand-free local CSS; the origin (with its port 2 controller,
  // covered until the CSS drops that hand) only if the local snapshot is missing.
  PersistentQuickPlaySession::RestoreLocal(
      m_system, [self = QPointer<SeamlessQuickPlay>(this), finish](bool ok) {
        if (ok || !self || !self->m_persistent_quickplay || !self->m_persistent_retiring)
          return finish(ok);
        self->m_persistent_quickplay->Restore(self->m_system, finish);
      });
}

void SeamlessQuickPlay::TakeLocalQuickPlaySnapshot()
{
  // Only from the idle local CSS: never during a search, handoff, session or retirement.
  if (!Core::IsRunning(m_system) || Core::GetState(m_system) != Core::State::Running ||
      m_session.m_quickplay_setup || m_persistent_quickplay || m_persistent_retiring ||
      m_session.m_quickplay_session || m_quickplay_validating ||
      Settings::Instance().GetNetPlayClient() || Settings::Instance().GetNetPlayServer())
  {
    INFO_LOG_FMT(NETPLAY, "Quick Play recipe: local CSS snapshot skipped (not idle)");
    QuickPlayRecipe::FinishLocalSnapshot();
    return;
  }
  Core::SetState(m_system, Core::State::Paused);
  Core::RunOnCPUThread(m_system, [this] {
    QuickPlayRecipe::SaveLocalSnapshot(m_system);
    QueueOnObject(this, [this] {
      QuickPlayRecipe::FinishLocalSnapshot();
      if (Core::GetState(m_system) == Core::State::Paused && !m_session.m_quickplay_setup &&
          !m_persistent_quickplay && !m_persistent_retiring)
        Core::SetState(m_system, Core::State::Running);
    });
  });
}
