// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlaySession.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <QApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/HW/SI/SI.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/ProjectPlusOnline/LobbyPresentation.h"
#include "Core/ProjectPlusOnline/LocalCSS.h"
#include "Core/ProjectPlusOnline/LocalInput.h"
#include "Core/ProjectPlusOnline/Status.h"
#include "Core/QuickPlayRecipe.h"
#include "Core/System.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/HotkeyScheduler.h"
#include "DolphinQt/MenuBar.h"
#include "DolphinQt/NetPlay/NetPlayDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QuickPlay/ContentPolicy.h"
#include "DolphinQt/QuickPlay/PersistentSession.h"
#include "DolphinQt/QuickPlay/QuickPlayClient.h"
#include "DolphinQt/QuickPlay/QuickPlayController.h"
#include "DolphinQt/QuickPlay/QuickPlayDialog.h"
#include "DolphinQt/QuickPlay/QuickPlayLauncher.h"
#include "DolphinQt/QuickPlay/QuickPlaySettingsDialog.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/ToolBar.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/Present.h"

// Seamless Quick Play supports X11 (including XWayland) and Windows.
static bool IsQuickPlayWindowSystem()
{
  const QString platform_name = QGuiApplication::platformName();
  return platform_name == QStringLiteral("xcb") || platform_name == QStringLiteral("windows");
}

// The trial declaration names the game list's own launcher entry, so the
// network-thread lookup compares the exact path string it will select.
static std::optional<QuickPlayContent::TrialLauncher>
QuickPlayTrialLauncher(const PersistentQuickPlaySession* session, const GameListModel& model)
{
  if (!session || !session->TrialLauncher())
    return std::nullopt;
  auto trial = *session->TrialLauncher();
  if (const auto launcher = FindPPlusNetplayLauncher(model).game)
    trial.path = launcher->GetFilePath();
  return trial;
}

QuickPlaySession::QuickPlaySession(QuickPlayWindow& window, Core::System& system, QWidget* parent,
                                   NetPlayDialog* netplay_dialog,
                                   const GameListModel& game_list_model, ToolBar* tool_bar,
                                   MenuBar* menu_bar, HotkeyScheduler* hotkey_scheduler)
    : QObject(parent), m_window(window), m_system(system), m_dialog_parent(parent),
      m_netplay_dialog(netplay_dialog), m_game_list_model(game_list_model), m_tool_bar(tool_bar)
{
  connect(menu_bar, &MenuBar::ShowQuickPlayDiagnostics, this,
          &QuickPlaySession::ShowQuickPlayDiagnostics);
  connect(tool_bar, &ToolBar::QuickPlayPressed, this, &QuickPlaySession::StartQuickPlay);
  connect(tool_bar, &ToolBar::QuickPlaySettingsPressed, this,
          &QuickPlaySession::ShowQuickPlaySettings);
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
            !m_quickplay_setup || m_quickplay_setup->attempt != request.context.identity.attempt ||
            m_quickplay_setup->window != m_window.GetRenderWidget() ||
            m_quickplay_setup->native_window !=
                static_cast<u64>(m_window.GetRenderWidget()->effectiveWinId()) ||
            m_quickplay_setup->client != Settings::Instance().GetNetPlayClient().get() ||
            m_quickplay_setup->server != Settings::Instance().GetNetPlayServer().get() ||
            QApplication::activeModalWidget() || m_window.IsStopConfirmShowing() ||
            !Host::GetInstance()->GetRenderFocus() ||
            ProjectPlusOnline::host_ui_input_blocked.load() || !device_connected ||
            !m_quickplay_controller ||
            m_quickplay_controller->GetGuestCancelAttempt() != request.context.identity.attempt)
          return;
        m_quickplay_cancel_consumed = true;
        ProjectPlusOnline::setup_action.Invalidate();
        m_quickplay_controller->Cancel();
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
            m_quickplay_recovery != QuickPlayRecovery::None || !Core::IsRunning(m_system) ||
            QApplication::activeModalWidget() || m_window.IsStopConfirmShowing() ||
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
          [this] { StartQuickPlayFrom(ProjectPlusOnline::EntrySource::InGame); });
  connect(
      Host::GetInstance(), &Host::RequestQuickPlayCancel, this,
      [this] {
        // Synchronous GUI delivery from the guarded Core host job. No event-loop
        // boundary between its identity check and the canonical desktop Cancel.
        const auto attempt = ProjectPlusOnline::guest_cancel_attempt.load();
        if (attempt && m_quickplay_controller &&
            m_quickplay_controller->GetGuestCancelAttempt() == attempt)
        {
          m_quickplay_controller->Cancel();
          PublishQuickPlayStatus();
        }
      },
      Qt::DirectConnection);

  connect(m_netplay_dialog, &NetPlayDialog::InGameError, this, [this](const QString& error) {
    ReportQuickPlayError(error);
    const auto identity = QuickPlayIdentity();
    if (m_quickplay_blocking_connection)
      return;  // Synchronous caller returns the retained cause after construction.
    QTimer::singleShot(0, this, [this, identity, error] {
      if (identity != QuickPlayIdentity() || !OwnsInGameQuickPlay())
        return;
      if (m_quickplay_controller && m_quickplay_controller->OwnsAttempt(identity.attempt))
        m_quickplay_controller->OnLaunchFailed(identity.attempt, error);
      else if (m_persistent_quickplay)
        RetirePersistentQuickPlay(true);
    });
  });
  connect(m_netplay_dialog, &NetPlayDialog::QuickPlayStartAborted, this, [this](quint64 attempt) {
    // Defer destruction until the normal NetPlay callback has returned.
    QTimer::singleShot(0, this, [this, attempt] {
      if (m_quickplay_controller)
        m_quickplay_controller->OnLaunchFailed(attempt, tr("NetPlay game start was aborted."));
    });
  });
  connect(m_netplay_dialog, &NetPlayDialog::PersistentStart, this,
          &QuickPlaySession::PreparePersistentQuickPlay);
  connect(m_netplay_dialog, &NetPlayDialog::PersistentBarrier, this,
          &QuickPlaySession::HandlePersistentBarrier);
  connect(m_netplay_dialog, &NetPlayDialog::Stop, this, &QuickPlaySession::OnNetPlayStop);

  // Offline Launcher boots from this frontend prepare the in-game Quick Play origin.
  QuickPlayRecipe::SetFrontendSupported(true);
  QuickPlayRecipe::SetLocalSnapshotHandler([this] { TakeLocalQuickPlaySnapshot(); });

  auto* status_timer = new QTimer(this);
  connect(status_timer, &QTimer::timeout, this, &QuickPlaySession::PublishQuickPlayStatus);
  status_timer->start(100);
  PublishQuickPlayStatus();
}

QuickPlaySession::~QuickPlaySession() = default;

void QuickPlaySession::Shutdown()
{
  InvalidateQuickPlayPresentation();
  if (m_persistent_quickplay)
    RetirePersistentQuickPlay(false);
  // Destroy orchestration while its MainWindow callbacks and NetPlay dialog are still alive.
  delete m_quickplay_dialog;
  m_quickplay_dialog = nullptr;
  delete m_quickplay_controller;
  m_quickplay_controller = nullptr;
}

void QuickPlaySession::OnEmulationStateChanged(Core::State state)
{
  if (state == Core::State::Stopping && m_persistent_quickplay)
    RetirePersistentQuickPlay(false);
  if (state == Core::State::Stopping && m_quickplay_session)
    EndQuickPlaySession(true);
  if (state == Core::State::Running && m_quickplay_recovery == QuickPlayRecovery::Booting)
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_controller->Cancel();
    INFO_LOG_FMT(NETPLAY, "QuickPlay: local recovery running");
  }
}

bool QuickPlaySession::IsSetupPaused() const
{
  return m_quickplay_validating || m_persistent_retiring ||
         (m_persistent_quickplay && m_quickplay_setup);
}

bool QuickPlaySession::HandleStopComplete()
{
  if (m_quickplay_recovery == QuickPlayRecovery::WaitingForStop)
  {
    m_quickplay_recovery = QuickPlayRecovery::Queued;
    // Let the old Core's observers drain before destroying the old lobby or booting.
    QTimer::singleShot(0, this, &QuickPlaySession::FinishQuickPlayRecovery);
    return true;
  }
  if (m_quickplay_recovery == QuickPlayRecovery::Queued ||
      m_quickplay_recovery == QuickPlayRecovery::Releasing)
    return true;
  if (m_quickplay_recovery == QuickPlayRecovery::Booting)
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_controller->Cancel();
  }
  if (m_quickplay_retain_window && !m_window.IsExitRequested() &&
      !Settings::Instance().IsBatchModeEnabled() && !m_window.IsBootPending())
  {
    const auto attempt = std::exchange(m_quickplay_stop_attempt, 0);
    if (attempt)
    {
      // Drain every observer of the old Uninitialized notification before creating a
      // lobby: NetPlayDialog also handles that notification and must see no new session.
      QTimer::singleShot(0, this, [this, attempt] {
        if (m_quickplay_retain_window && !m_window.IsExitRequested() && !m_window.IsBootPending() &&
            Core::IsUninitialized(m_system) && m_quickplay_controller->OwnsAttempt(attempt))
          m_quickplay_controller->OnCoreStopped(attempt);
      });
    }
    return true;
  }
  return false;
}

void QuickPlaySession::OnRequestStop()
{
  // Revoke the deferred admission before a confirmation dialog can run its
  // nested event loop. Restore only the pause owned by this pending request.
  if (m_quickplay_validating)
  {
    CancelQuickPlayValidation();
    InvalidateQuickPlayPresentation();
  }
  ProjectPlusOnline::presentation.Invalidate();
  // A close during the deferred stop continuation must cancel the reboot, while
  // leaving its sole teardown owner alive to release the old session.
  if (m_quickplay_recovery != QuickPlayRecovery::None)
    m_quickplay_recovery_restart = false;
  if (m_quickplay_retain_window || m_quickplay_stop_attempt)
  {
    if (m_quickplay_controller &&
        m_quickplay_controller->GetState() != QuickPlayController::State::NetPlayOwned)
      m_quickplay_controller->Cancel();
    m_quickplay_stop_attempt = 0;
    m_quickplay_retain_window = false;
  }
}

bool QuickPlaySession::HandleRequestStop()
{
  if (m_persistent_quickplay || m_persistent_retiring)
  {
    // Explicit application Stop/Close retires the session and stops this Core;
    // only a room/session leave performs clean local CSS recovery.
    m_window.ForceStop();
    return true;
  }
  if (m_quickplay_session || m_quickplay_recovery != QuickPlayRecovery::None)
  {
    // Guest power events are asynchronous to synchronized pad input. End this
    // session using the normal NetPlay stop notification instead of guest shutdown.
    EndQuickPlaySession(false);
    m_window.ForceStop();
    return true;
  }
  return false;
}

void QuickPlaySession::OnForceStop()
{
  InvalidateQuickPlayPresentation();
  if (m_persistent_quickplay || m_persistent_retiring)
    RetirePersistentQuickPlay(false);
}

void QuickPlaySession::OnReset()
{
  CancelQuickPlayValidation();
  InvalidateQuickPlayPresentation();
}

bool QuickPlaySession::OwnsBoot() const
{
  return m_quickplay_setup || m_quickplay_recovery == QuickPlayRecovery::WaitingForStop ||
         m_quickplay_recovery == QuickPlayRecovery::Queued ||
         m_quickplay_recovery == QuickPlayRecovery::Releasing;
}

void QuickPlaySession::OnBoot()
{
  // The retained-window lease ends when an explicit later boot consumes the window.
  m_quickplay_retain_window = false;
}

bool QuickPlaySession::OnNetPlayBoot()
{
  if (!m_quickplay_setup)
    return true;
  const auto attempt = m_quickplay_setup->attempt;
  if (!NetPlay::IsNetPlayRunning() || !IsQuickPlayLaunchReady(attempt))
  {
    QTimer::singleShot(0, this, [this, attempt] {
      m_quickplay_controller->OnLaunchFailed(attempt, tr("Quick Play launch was invalidated."));
    });
    return false;
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay: NetPlay owns boot, attempt {}", attempt);
  const auto* retained_window = m_window.GetRenderWidget();
  const auto native_window = m_quickplay_setup->native_window;
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_quickplay_setup.reset();
  m_netplay_dialog->ReleaseQuickPlayStart();
  m_quickplay_controller->OnLaunchOwned(attempt);
  // Ownership observers may close the window before the fresh boot is entered.
  if (!m_quickplay_retain_window || m_window.IsExitRequested() ||
      m_window.GetRenderWidget() != retained_window || !m_window.GetRenderWidget()->isVisible() ||
      static_cast<u64>(m_window.GetRenderWidget()->effectiveWinId()) != native_window)
  {
    if (const auto client = Settings::Instance().GetNetPlayClient())
      client->RequestStopGame();
    return false;
  }
  m_quickplay_session = true;
  return true;
}

void QuickPlaySession::OnNetPlayStop()
{
  if (m_persistent_quickplay || m_persistent_retiring)
  {
    // A peer stop with no captured failure: the opponent left (local leave and
    // Stop retire first, so their own stop echo arrives while retiring).
    if (m_persistent_quickplay && !m_persistent_retiring &&
        m_netplay_dialog->SessionError().isEmpty())
      m_quickplay_opponent_left = true;
    RetirePersistentQuickPlay(true);
    return;
  }
  EndQuickPlaySession(true);
  m_window.ForceStop();
  if (m_quickplay_recovery == QuickPlayRecovery::WaitingForStop && Core::IsUninitialized(m_system))
    m_window.OnStopComplete();
}

bool QuickPlaySession::OwnsNetPlay() const
{
  return m_quickplay_setup || m_quickplay_host_attempt || m_quickplay_joining ||
         m_persistent_quickplay || m_persistent_retiring || m_quickplay_session ||
         m_quickplay_validating || m_quickplay_recovery != QuickPlayRecovery::None ||
         (m_quickplay_controller &&
          m_quickplay_controller->GetState() != QuickPlayController::State::Idle &&
          m_quickplay_controller->GetState() != QuickPlayController::State::Error);
}

bool QuickPlaySession::CanJoinWhileRunning() const
{
  return IsPersistentQuickPlayBoundary() && (m_quickplay_joining || m_quickplay_host_attempt);
}

bool QuickPlaySession::CanHostWhileRunning(u64 quickplay_attempt) const
{
  return quickplay_attempt && m_quickplay_setup &&
         m_quickplay_setup->attempt == quickplay_attempt && IsPersistentQuickPlayBoundary();
}

bool QuickPlaySession::IsRetiring() const
{
  return m_persistent_retiring || m_quickplay_recovery != QuickPlayRecovery::None;
}

bool QuickPlaySession::MayJoinOwnServer(const NetPlay::NetPlayServer& server) const
{
  return !m_quickplay_setup || (m_quickplay_controller->OwnsAttempt(m_quickplay_setup->attempt) &&
                                server.GetQuickPlayAttempt() == m_quickplay_setup->attempt);
}

bool QuickPlaySession::IsPersistent() const
{
  return m_persistent_quickplay != nullptr;
}

std::optional<QuickPlaySession::QuickPlaySetup> QuickPlaySession::JoinSetup() const
{
  return (m_quickplay_joining || m_quickplay_host_attempt) ? m_quickplay_setup : std::nullopt;
}

void QuickPlaySession::PrepareJoin(bool local_join)
{
  if (!local_join)
    m_netplay_dialog->BeginSession(
        m_quickplay_setup ? m_quickplay_entry : ProjectPlusOnline::EntrySource::Desktop,
        m_persistent_quickplay != nullptr, m_quickplay_setup ? m_quickplay_setup->attempt : 0,
        QuickPlayTrialLauncher(m_persistent_quickplay.get(), m_game_list_model));
  if (m_quickplay_setup && m_quickplay_retain_window)
  {
    const auto attempt = m_quickplay_setup->attempt;
    m_netplay_dialog->ArmQuickPlayStart(
        attempt, [this, attempt] { return IsQuickPlayLaunchReady(attempt); });
  }
}

void QuickPlaySession::OnClientCreated()
{
  if (m_quickplay_setup)
  {
    m_quickplay_setup->client = Settings::Instance().GetNetPlayClient().get();
    m_quickplay_setup->server = Settings::Instance().GetNetPlayServer().get();
    if (m_persistent_quickplay)
    {
      m_persistent_client = m_quickplay_setup->client;
      m_persistent_server = m_quickplay_setup->server;
    }
  }
}

void QuickPlaySession::PrepareHost(u64 quickplay_attempt)
{
  // Establish presentation before the server or local client can report synchronous errors.
  m_netplay_dialog->BeginSession(
      quickplay_attempt ? m_quickplay_entry : ProjectPlusOnline::EntrySource::Desktop,
      m_persistent_quickplay != nullptr, quickplay_attempt,
      QuickPlayTrialLauncher(m_persistent_quickplay.get(), m_game_list_model));
}

NetPlay::SyncIdentifier QuickPlaySession::RoomGame(const UICommon::GameFile& game) const
{
  return m_persistent_quickplay && m_persistent_quickplay->TrialLauncher() ?
             m_persistent_quickplay->TrialLauncher()->identity :
             game.GetSyncIdentifier();
}

std::optional<u64> QuickPlaySession::PrepareNetPlayQuit()
{
  if (m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame)
  {
    const auto cause = m_netplay_dialog->SessionError();
    if (!cause.isEmpty())
      ReportQuickPlayError(cause);
  }
  if (m_persistent_quickplay || m_persistent_retiring)
  {
    RetirePersistentQuickPlay(true);
    return std::nullopt;
  }
  InvalidateQuickPlayPresentation();
  if (m_quickplay_session || m_quickplay_recovery != QuickPlayRecovery::None)
  {
    EndQuickPlaySession(false);
    m_window.ForceStop();
    if (Core::IsUninitialized(m_system))
      m_window.OnStopComplete();
    return std::nullopt;
  }
  const auto attempt = m_quickplay_setup ? m_quickplay_setup->attempt : 0;
  ReleaseQuickPlayWindow();
  m_quickplay_setup.reset();
  m_quickplay_join_attempt = 0;
  m_quickplay_host_attempt = 0;
  return attempt;
}

void QuickPlaySession::FinishNetPlayQuit(u64 attempt)
{
  m_netplay_dialog->ResetSession();
  if (m_quickplay_controller)
  {
    if (attempt)
      m_quickplay_controller->OnHostClosed(attempt);
    else if (m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
      m_quickplay_controller->Cancel();
  }
}

void QuickPlaySession::ShowQuickPlaySettings()
{
  if (m_quickplay_controller &&
      m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
    return;

  // Settings access must not construct a controller or start a matchmaking attempt.
  m_tool_bar->SetQuickPlaySettingsOpen(true);
  if (!m_quickplay_settings_dialog)
  {
    m_quickplay_settings_dialog = new QuickPlaySettingsDialog(m_dialog_parent);
    connect(m_quickplay_settings_dialog, &QDialog::finished, this,
            [this] { m_tool_bar->SetQuickPlaySettingsOpen(false); });
  }
  m_quickplay_settings_dialog->Open();
}

void QuickPlaySession::PublishQuickPlayStatus()
{
  ProjectPlusOnline::persistent_setup_pause = m_quickplay_validating || m_persistent_retiring ||
                                              (m_persistent_quickplay && m_quickplay_setup);
  Common::ScopeGuard publish_presentation([this] { PublishQuickPlayPresentation(); });
  using ProjectPlusOnline::GuestStatus;
  using State = QuickPlayController::State;
  ProjectPlusOnline::persistent_local_input =
      PersistentQuickPlaySession::IsConfigured() && !m_persistent_quickplay &&
      !m_persistent_retiring && !NetPlay::IsNetPlayRunning() && !m_window.IsExitRequested() &&
      !m_quickplay_validating;
  const auto publish = [this](GuestStatus status) {
    const auto attempt =
        m_quickplay_controller && m_quickplay_recovery == QuickPlayRecovery::None &&
                !m_quickplay_validating && !m_quickplay_blocking_connection &&
                !m_persistent_retiring && ProjectPlusOnline::IsCancellable(status) ?
            m_quickplay_controller->GetGuestCancelAttempt() :
            0;
    if (ProjectPlusOnline::guest_cancel_attempt.load() != attempt)
      ProjectPlusOnline::guest_cancel_attempt = 0;
    ProjectPlusOnline::guest_cancel_attempt = attempt;
  };
  if (m_quickplay_recovery != QuickPlayRecovery::None)
    return publish(GuestStatus::Unavailable);
  const auto client = Settings::Instance().GetNetPlayClient();
  if (client)
  {
    const auto lobby = client->GetLobbyState();
    if (lobby.connected && lobby.players.size() >= 2)
      return publish(GuestStatus::Connected);
  }
  if (client || Settings::Instance().GetNetPlayServer() || m_quickplay_session)
    return publish(GuestStatus::Connecting);
  if (m_quickplay_stop_attempt && m_quickplay_setup &&
      m_quickplay_controller->OwnsAttempt(m_quickplay_stop_attempt))
  {
    if (m_quickplay_presentation == QuickPlayPresentation::Matched)
      return publish(GuestStatus::Matched);
    if (m_quickplay_presentation == QuickPlayPresentation::Connecting)
      return publish(GuestStatus::Connecting);
  }
  const auto state = m_quickplay_controller ? m_quickplay_controller->GetState() : State::Idle;
  switch (state)
  {
  case State::Idle:
    if (Core::GetState(m_system) == Core::State::Stopping)
      return publish(GuestStatus::Unavailable);
    return publish(m_quickplay_setup ? GuestStatus::Connecting : GuestStatus::Idle);
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

void QuickPlaySession::InvalidateQuickPlayPresentation()
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
  m_quickplay_admission_error.clear();
  m_quickplay_cancelled_until = {};
}

void QuickPlaySession::CancelQuickPlayValidation()
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

ProjectPlusOnline::PresentationIdentity QuickPlaySession::QuickPlayIdentity() const
{
  return {ProjectPlusOnline::presentation_core_generation.load(),
          ProjectPlusOnline::persistent_input_epoch.load(), m_quickplay_owned_attempt,
          m_quickplay_room_generation, m_persistent_game};
}

bool QuickPlaySession::OwnsInGameQuickPlay() const
{
  return m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame &&
         m_quickplay_owner_core == ProjectPlusOnline::presentation_core_generation.load() &&
         m_persistent_quickplay && !m_persistent_retiring && m_quickplay_owned_attempt &&
         m_persistent_client == Settings::Instance().GetNetPlayClient().get() &&
         m_persistent_server == Settings::Instance().GetNetPlayServer().get();
}

void QuickPlaySession::ReportQuickPlayError(const QString& error)
{
  INFO_LOG_FMT(NETPLAY, "QuickPlay: error shown: {}", error.toStdString());
  m_quickplay_admission_error = error;
  m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
  m_netplay_dialog->SetDiagnosticError(error);
  PublishQuickPlayStatus();
  const bool renderer_route = Core::IsRunning(m_system) && !m_system.IsDualCoreMode() &&
                              PersistentQuickPlaySession::IsConfigured() &&
                              IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
                              m_window.GetRenderWidget()->isVisible();
  if (m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame && !error.isEmpty() &&
      !renderer_route && !m_window.IsExitRequested() && !m_window.IsBootPending() &&
      Core::GetState(m_system) != Core::State::Stopping)
    m_netplay_dialog->ShowDiagnostics();
}

void QuickPlaySession::ShowQuickPlayDiagnostics()
{
  ProjectPlusOnline::setup_action.Invalidate();
  if (!m_quickplay_admission_error.isEmpty())
    m_netplay_dialog->SetDiagnosticError(m_quickplay_admission_error);
  m_netplay_dialog->ShowDiagnostics();
}

void QuickPlaySession::ObservePausedQuickPlayCSS()
{
  m_quickplay_paused_css.reset();
  ProjectPlusOnline::setup_action.Invalidate();
  const auto identity = QuickPlayIdentity();
  const auto session = QPointer<QuickPlaySession>(this);
  Core::RunOnCPUThread(m_system, [session, identity, &system = m_system] {
    if (identity.core != ProjectPlusOnline::presentation_core_generation.load() ||
        identity.input != ProjectPlusOnline::persistent_input_epoch.load() ||
        Core::GetState(system) != Core::State::Paused)
      return;
    const bool css =
        ProjectPlusOnline::ObserveLocalCSS(system) && !system.GetMovie().IsMovieActive();
    QueueOnObject(QApplication::instance(), [session, identity, css] {
      if (!session || identity != session->QuickPlayIdentity() ||
          !session->IsPersistentQuickPlayBoundary())
        return;
      if (css)
        session->m_quickplay_paused_css = identity;
      session->PublishQuickPlayStatus();
    });
  });
}

void QuickPlaySession::PrepareQuickPlayConnection(u64 attempt, bool host)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt))
    return;
  m_quickplay_blocking_connection = true;
  ProjectPlusOnline::setup_action.Invalidate();
  PublishQuickPlayStatus();
  const auto fence = ProjectPlusOnline::PrepaintFence{
      QuickPlayIdentity(), ++m_quickplay_validation_request,
      static_cast<std::uintptr_t>(m_window.GetRenderWidget()->effectiveWinId())};
  const auto connect_room = [this, attempt, host, fence](bool painted) {
    const auto current = ProjectPlusOnline::PrepaintFence{
        QuickPlayIdentity(), m_quickplay_validation_request,
        static_cast<std::uintptr_t>(m_window.GetRenderWidget()->effectiveWinId())};
    if (fence != current || !m_quickplay_controller->OwnsAttempt(attempt) || !m_quickplay_setup ||
        m_quickplay_setup->attempt != attempt)
      return;
    const bool dispatchable = !m_window.IsExitRequested() && !m_window.IsBootPending() &&
                              !m_window.IsStopConfirmShowing() &&
                              !QApplication::activeModalWidget();
    const bool supported =
        !m_persistent_quickplay || (IsPersistentQuickPlayBoundary() && !m_system.IsDualCoreMode() &&
                                    IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
                                    m_window.GetRenderWidget()->isVisible());
    const QString error = painted && dispatchable && supported ?
                              QString{} :
                              tr("Quick Play could not display its paused connection status. Open "
                                 "Tools > Quick Play Diagnostics.");
    m_quickplay_controller->ContinueConnection(attempt, host, error);
    m_quickplay_blocking_connection = false;
    ProjectPlusOnline::setup_action.Invalidate();  // A fresh B release is required after blocking.
    PublishQuickPlayStatus();
  };
  if (m_persistent_quickplay)
    RedrawQuickPlayPresentation(connect_room);
  else
    connect_room(true);  // Ordinary Desktop setup retains its synchronous behavior.
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

void QuickPlaySession::PublishQuickPlayPresentation()
{
  using namespace ProjectPlusOnline;
  using State = QuickPlayController::State;
  PresentationSnapshot value;
  const auto observed_revision = presentation.Revision();
  const auto core_state = Core::GetState(m_system);
  const auto core = presentation_core_generation.load();
  const auto input = persistent_input_epoch.load();
  value.identity = QuickPlayIdentity();
  value.entry = m_quickplay_entry;
  const auto client = Settings::Instance().GetNetPlayClient();
  const auto lobby = client ? std::optional{client->GetLobbyState()} : std::nullopt;
  const bool disconnected = lobby && !lobby->connected;
  const bool supported = Core::IsRunning(m_system) && PersistentQuickPlaySession::IsConfigured();
  const bool eligible = local_css.Read().IsFresh(core, input, std::chrono::steady_clock::now()) &&
                        IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
                        m_window.GetRenderWidget()->isVisible() && !client &&
                        !Settings::Instance().GetNetPlayServer();
  const auto state = m_quickplay_controller ? m_quickplay_controller->GetState() : State::Idle;
  const bool paused_action =
      OwnsInGameQuickPlay() && IsPersistentQuickPlayBoundary() &&
      m_quickplay_paused_css == value.identity && m_quickplay_setup &&
      m_quickplay_setup->attempt == value.identity.attempt &&
      m_quickplay_setup->window == m_window.GetRenderWidget() &&
      m_quickplay_setup->native_window ==
          static_cast<u64>(m_window.GetRenderWidget()->effectiveWinId()) &&
      m_quickplay_controller && m_quickplay_controller->OwnsAttempt(value.identity.attempt) &&
      m_quickplay_controller->GetGuestCancelAttempt() == value.identity.attempt &&
      guest_cancel_attempt.load() == value.identity.attempt &&
      (!lobby || (lobby->connected && lobby->current_game == value.identity.game)) && supported &&
      IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
      m_window.GetRenderWidget()->isVisible() && !m_window.IsExitRequested() &&
      !m_window.IsBootPending() && !m_window.IsStopConfirmShowing() &&
      !QApplication::activeModalWidget() && !m_quickplay_validating &&
      !m_quickplay_blocking_connection && m_quickplay_recovery == QuickPlayRecovery::None;
  const auto action = setup_action.Publish(value.identity, paused_action);
  // Leave is available for the whole owned in-game session once the opponent has
  // joined: paused setup, shared CSS, stage select, gameplay and results.
  const bool leave_action_enabled =
      OwnsInGameQuickPlay() && lobby && lobby->connected && lobby->players.size() >= 2 &&
      supported && IsQuickPlayWindowSystem() && !m_window.IsRenderingToMain() &&
      m_window.GetRenderWidget()->isVisible() && !m_window.IsExitRequested() &&
      !m_window.IsBootPending() && !m_window.IsStopConfirmShowing() &&
      !QApplication::activeModalWidget() && !m_quickplay_validating &&
      !m_quickplay_blocking_connection && m_quickplay_recovery == QuickPlayRecovery::None &&
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
           m_quickplay_recovery == QuickPlayRecovery::None)
  {
    if (m_quickplay_validating)
    {
      value.phase = PresentationPhase::Checking;
      text = QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::Strict ?
                 tr("Checking game files…") :
                 tr("Preparing session…");
    }
    else if (!m_quickplay_admission_error.isEmpty() && value.entry == EntrySource::InGame &&
             m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Error;
      text = m_quickplay_admission_error + tr("  Tools > Quick Play Diagnostics");
    }
    else if (state == State::Error && value.entry == EntrySource::InGame &&
             m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Error;
      text = m_quickplay_controller->GetErrorMessage();
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
        const auto& regions = m_quickplay_controller->GetRegions();
        const auto backend = m_quickplay_controller->GetBackendLabel();
        // Kept short: the overlay has two lines, and the cancel hint follows the text.
        const auto stage = m_quickplay_controller->GetSearchStage();
        const QString where = regions.size() == 1 ?
                                  QuickPlayClient::GetRegionLabel(regions.front()) :
                                  tr("%n regions", "", regions.size());
        if (!m_quickplay_controller->GetSearchNotice().isEmpty())
          text = m_quickplay_controller->GetSearchNotice();
        else if (regions.isEmpty())
          text = tr("Searching (%1)…").arg(backend);
        else if (stage == QuickPlayMatchmaker::SearchStage::JoinOnly)
          text = tr("Looking for a host in %1 (%2)… %3 s")
                     .arg(where, backend)
                     .arg(m_quickplay_controller->GetSearchStageSeconds());
        else if (stage == QuickPlayMatchmaker::SearchStage::JoinListed)
          text = tr("Searching in %1 (%2), now listed…").arg(where, backend);
        else
          text = tr("Searching in %1 (%2)…").arg(where, backend);
        break;
      }
      case State::MatchedHost:
      case State::MatchedClient:
      case State::PreparingHandoff:
      case State::WaitingForCoreStop:
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
      value.cancel = CanPresentCancel(core_state == Core::State::Running, eligible,
                                      !m_quickplay_blocking_connection && !m_quickplay_setup &&
                                          guest_cancel_attempt.load() == value.identity.attempt,
                                      m_quickplay_controller->GetGuestCancelAttempt()) ||
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

void QuickPlaySession::RedrawQuickPlayPresentation(std::function<void(bool)> finished)
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
  const auto session = QPointer<QuickPlaySession>(this);
  Core::RunOnCPUThread(m_system, [session, widget, native, core, input, revision, identity] {
    const bool ok = ProjectPlusOnline::presentation_core_generation.load() == core &&
                    ProjectPlusOnline::persistent_input_epoch.load() == input &&
                    ProjectPlusOnline::presentation.Revision() == revision && g_presenter &&
                    g_presenter->RedrawRetainedFrame();
    QueueOnObject(
        QApplication::instance(), [session, widget, native, core, input, revision, identity, ok] {
          if (!session)
            return;
          session->m_quickplay_redraw_pending = false;
          auto callbacks = std::move(session->m_quickplay_redraw_callbacks);
          session->m_quickplay_redraw_callbacks.clear();
          const bool current = ok && widget && session->m_window.GetRenderWidget() == widget &&
                               widget->effectiveWinId() == native &&
                               ProjectPlusOnline::presentation_core_generation.load() == core &&
                               ProjectPlusOnline::persistent_input_epoch.load() == input &&
                               ProjectPlusOnline::presentation.Revision() == revision &&
                               session->QuickPlayIdentity() == identity;
          // Coalesce changed presentation while keeping every caller's own request fences.
          if (!current && widget && session->m_window.GetRenderWidget() == widget &&
              widget->effectiveWinId() == native && session->QuickPlayIdentity() == identity &&
              ProjectPlusOnline::presentation.Revision() != revision)
          {
            session->m_quickplay_redraw_callbacks = std::move(callbacks);
            session->RedrawQuickPlayPresentation();
            return;
          }
          for (auto& callback : callbacks)
            callback(current);
        });
  });
}

void QuickPlaySession::ValidateQuickPlayContent(u64 attempt,
                                                std::function<void(bool, QString)> finished)
{
  if (m_system.IsDualCoreMode() || !IsQuickPlayWindowSystem() || m_window.IsRenderingToMain() ||
      !m_window.GetRenderWidget()->isVisible())
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
         ProjectPlusOnline::persistent_input_epoch.load(), m_quickplay_owned_attempt,
         m_quickplay_room_generation, m_persistent_game},
        m_quickplay_validation_request,
        static_cast<std::uintptr_t>(m_window.GetRenderWidget()->effectiveWinId())};
    const bool current = fence == current_fence && widget && widget == m_window.GetRenderWidget() &&
                         !m_window.IsExitRequested() && !m_window.IsBootPending() &&
                         Core::GetState(m_system) == Core::State::Paused &&
                         (!attempt || (m_quickplay_setup && m_quickplay_setup->attempt == attempt &&
                                       m_quickplay_controller->OwnsAttempt(attempt)));
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

void QuickPlaySession::StartQuickPlay()
{
  StartQuickPlayFrom(ProjectPlusOnline::EntrySource::Desktop);
}

void QuickPlaySession::StartQuickPlayFrom(ProjectPlusOnline::EntrySource entry)
{
  if (m_quickplay_validating || m_quickplay_recovery != QuickPlayRecovery::None ||
      m_persistent_retiring || m_window.IsExitRequested() || m_window.IsBootPending())
    return;
  if (entry == ProjectPlusOnline::EntrySource::Desktop &&
      (Settings::Instance().GetNetPlayClient() || m_netplay_dialog->HasRoom()))
  {
    ShowQuickPlayDiagnostics();
    return;
  }
  if (Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->HasRoom())
    return;
  if (m_quickplay_settings_dialog && m_quickplay_settings_dialog->isVisible())
    return;
  if (Core::GetState(m_system) == Core::State::Stopping || m_quickplay_stop_attempt)
    return;
  if (entry == ProjectPlusOnline::EntrySource::InGame && m_quickplay_controller &&
      m_quickplay_controller->GetState() == QuickPlayController::State::Error)
    m_quickplay_controller->Cancel();
  if (m_quickplay_controller &&
      m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
  {
    // Explicit desktop diagnostics may reveal an existing attempt without
    // replacing its provenance or starting another coordinator request.
    if (entry == ProjectPlusOnline::EntrySource::Desktop)
    {
      m_quickplay_dialog->show();
      m_quickplay_dialog->raise();
      m_quickplay_dialog->activateWindow();
    }
    return;
  }
  m_quickplay_entry = entry;
  m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
  m_quickplay_admission_error.clear();
  m_netplay_dialog->SetDiagnosticError({});
  m_quickplay_cancelled_until = {};
  m_quickplay_left_until = {};
  m_quickplay_opponent_left_until = {};
  ++m_quickplay_room_generation;
  if (Core::IsRunning(m_system) &&
      (PersistentQuickPlaySession::IsConfigured() ||
       QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::DeclaredBundleTrialV1))
  {
    ValidateQuickPlayContent(0, [this](bool valid, QString error) {
      if (!valid)
      {
        ReportQuickPlayError(error);
        PublishQuickPlayStatus();
        if (m_quickplay_entry == ProjectPlusOnline::EntrySource::Desktop)
          ModalMessageBox::critical(m_dialog_parent, tr("Quick Play"), m_quickplay_admission_error);
        return;
      }
      BeginQuickPlaySearch();
    });
    return;
  }
  BeginQuickPlaySearch();
}

void QuickPlaySession::BeginQuickPlaySearch()
{
  m_tool_bar->SetQuickPlayActive(true);
  if (!m_quickplay_controller)
  {
    m_quickplay_controller = new QuickPlayController(
        [this](u64 attempt) { return StartQuickPlayHost(attempt); },
        [this](u64 attempt) { CancelQuickPlayHost(attempt); },
        [this](const QString& code) {
          if (JoinQuickPlayHost(code))
            return QString{};
          return m_quickplay_admission_error.isEmpty() ?
                     tr("Could not join the Quick Play host. Cancel and try again.") :
                     m_quickplay_admission_error;
        },
        [] {
          // Only a ready recipe means a seamless search: the origin outlives a stopped game.
          return QuickPlayRecipe::IsReady() ? QuickPlayRecipe::GetOrigin().digest : std::string();
        },
        this);
    connect(m_quickplay_controller, &QuickPlayController::PrepareConnection, this,
            &QuickPlaySession::PrepareQuickPlayConnection);
    connect(m_quickplay_controller, &QuickPlayController::PrepareHandoff, this,
            &QuickPlaySession::PrepareQuickPlayHandoff);
    connect(m_netplay_dialog, &NetPlayDialog::HostTraversalChanged, this,
            [this](quint64 attempt, const QString& code, bool failed) {
              if (failed && !m_quickplay_admission_error.isEmpty())
                m_quickplay_controller->OnLaunchFailed(attempt, m_quickplay_admission_error);
              else
                m_quickplay_controller->OnHostTraversalChanged(attempt, code, failed);
            });
    connect(m_netplay_dialog, &NetPlayDialog::QuickPlayOpponentConnected, m_quickplay_controller,
            &QuickPlayController::OnHostOpponentConnected);
    connect(m_quickplay_controller, &QuickPlayController::StateChanged, this,
            [this, previous =
                       QuickPlayController::State::Idle](QuickPlayController::State state) mutable {
              const auto old_state = std::exchange(previous, state);
              if (state == QuickPlayController::State::Error)
              {
                ReportQuickPlayError(m_quickplay_controller->GetErrorMessage());
              }
              if (state == QuickPlayController::State::Searching)
              {
                m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
                m_quickplay_owned_attempt = m_quickplay_controller->GetGuestCancelAttempt();
              }
              if (state == QuickPlayController::State::Idle)
              {
                m_quickplay_owned_attempt = 0;
                m_quickplay_cancelled_until =
                    old_state != QuickPlayController::State::NetPlayOwned &&
                            old_state != QuickPlayController::State::Error ?
                        std::chrono::steady_clock::now() + std::chrono::seconds(2) :
                        std::chrono::steady_clock::time_point{};
                if (m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame &&
                    m_quickplay_dialog)
                  m_quickplay_dialog->hide();
              }
              PublishQuickPlayStatus();
              if (state == QuickPlayController::State::NetPlayConnected && m_quickplay_setup)
              {
                const auto attempt = m_quickplay_setup->attempt;
                if (m_quickplay_retain_window)
                {
                  QTimer::singleShot(0, this, [this, attempt] { CheckQuickPlayLaunch(attempt); });
                }
                else
                {
                  // Desktop Quick Play still hands a normal lobby to the user.
                  m_quickplay_host_attempt = 0;
                  m_quickplay_join_attempt = 0;
                  m_quickplay_setup.reset();
                  m_quickplay_controller->OnLaunchOwned(attempt);
                }
              }
              m_tool_bar->SetQuickPlayActive(state != QuickPlayController::State::Idle &&
                                             state != QuickPlayController::State::NetPlayOwned);
            });
    m_quickplay_dialog = new QuickPlayDialog(*m_quickplay_controller, m_dialog_parent);
  }

  // Start remains canonical; entry provenance controls only presentation.
  m_quickplay_controller->Start();
  PublishQuickPlayStatus();
  if (m_quickplay_entry == ProjectPlusOnline::EntrySource::Desktop)
  {
    m_quickplay_dialog->show();
    m_quickplay_dialog->raise();
    m_quickplay_dialog->activateWindow();
  }
  else
  {
    m_quickplay_dialog->hide();
  }
}

void QuickPlaySession::PrepareQuickPlayHandoff(u64 attempt)
{
  if (!m_quickplay_controller->OwnsAttempt(attempt))
    return;
  const auto state = Core::GetState(m_system);
  if (m_window.IsExitRequested() || m_window.IsBootPending() ||
      Settings::Instance().IsBatchModeEnabled() || Settings::Instance().GetNetPlayClient() ||
      Settings::Instance().GetNetPlayServer() || m_netplay_dialog->HasRoom() ||
      (state != Core::State::Uninitialized && state != Core::State::Running &&
       state != Core::State::Paused))
  {
    m_quickplay_controller->OnCoreStopped(attempt, tr("The local game cannot hand off right now."));
    return;
  }
  if (state != Core::State::Uninitialized &&
      (!IsQuickPlayWindowSystem() || m_window.IsRenderingToMain() ||
       !m_window.GetRenderWidget()->isVisible()))
  {
    m_quickplay_controller->OnCoreStopped(
        attempt, tr("In-game Quick Play requires a separate render window."));
    return;
  }
  ProjectPlusOnline::presentation.Invalidate();
  ProjectPlusOnline::setup_action.Invalidate();
  ProjectPlusOnline::leave_action.Invalidate();
  m_quickplay_paused_css.reset();
  ++m_quickplay_room_generation;
  m_quickplay_setup = QuickPlaySetup{attempt,
                                     Config::Get(Config::NETPLAY_TRAVERSAL_SERVER),
                                     Config::Get(Config::NETPLAY_TRAVERSAL_PORT),
                                     Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT),
                                     Config::Get(Config::NETPLAY_LISTEN_PORT),
                                     Config::Get(Config::NETPLAY_NICKNAME),
                                     Config::Get(Config::NETPLAY_NETWORK_MODE),
                                     Config::Get(Config::NETPLAY_MINIMUM_BUFFER_SIZE),
                                     Config::Get(Config::NETPLAY_USE_UPNP)};
  if (state == Core::State::Uninitialized)
  {
    QTimer::singleShot(0, this, [this, attempt] {
      if (Core::IsUninitialized(m_system) && !m_window.IsExitRequested())
        m_quickplay_controller->OnCoreStopped(attempt);
    });
    return;
  }
  if (m_persistent_candidate || PersistentQuickPlaySession::IsConfigured() ||
      QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::DeclaredBundleTrialV1)
  {
    ValidateQuickPlayContent(attempt, [this, attempt](bool valid, QString error) {
      if (!m_quickplay_controller->OwnsAttempt(attempt))
        return;
      if (!valid)
      {
        m_quickplay_controller->OnHandoffReady(attempt, error);
        return;
      }
      m_persistent_quickplay = std::move(m_persistent_candidate);
      m_quickplay_retain_window = true;
      m_quickplay_setup->window = m_window.GetRenderWidget();
      m_quickplay_setup->native_window = static_cast<u64>(m_window.GetRenderWidget()->winId());
      ProjectPlusOnline::persistent_local_input = false;
      ++ProjectPlusOnline::persistent_input_epoch;
      const auto* session_owner = m_persistent_quickplay.get();
      const auto request = m_quickplay_validation_request;
      const auto room = m_quickplay_room_generation;
      m_persistent_quickplay->Restore(m_system, [session = QPointer<QuickPlaySession>(this),
                                                 attempt, session_owner, request, room](bool ok) {
        if (!session || session->m_persistent_quickplay.get() != session_owner ||
            session->m_persistent_retiring || request != session->m_quickplay_validation_request ||
            room != session->m_quickplay_room_generation ||
            !session->m_quickplay_controller->OwnsAttempt(attempt))
          return;
        session->m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
        INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: paused handoff attempt {} restored {}",
                     attempt, ok);
        if (ok)
          session->ObservePausedQuickPlayCSS();
        session->m_quickplay_controller->OnHandoffReady(
            attempt, ok ? QString{} : tr("The common CSS could not be restored."));
      });
    });
    return;
  }
  m_quickplay_stop_attempt = attempt;
  m_quickplay_retain_window = true;
  m_quickplay_setup->window = m_window.GetRenderWidget();
  m_quickplay_setup->native_window = static_cast<u64>(m_window.GetRenderWidget()->winId());
  // Bounded local presentation stages owned by the assigned handoff. The guest
  // renders snapshots only; no ACK, guest timer or renderer can hold up shutdown.
  m_quickplay_presentation = QuickPlayPresentation::Matched;
  PublishQuickPlayStatus();
  QTimer::singleShot(500, this, [this, attempt] { AdvanceQuickPlayPresentation(attempt); });
}

void QuickPlaySession::AdvanceQuickPlayPresentation(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_stop_attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt) ||
      m_quickplay_controller->GetState() != QuickPlayController::State::WaitingForHandoff)
    return;
  const auto state = Core::GetState(m_system);
  if (m_window.IsExitRequested() || m_window.IsBootPending() ||
      (state != Core::State::Running && state != Core::State::Paused))
    return;
  if (m_quickplay_presentation == QuickPlayPresentation::Matched)
  {
    m_quickplay_presentation = QuickPlayPresentation::Connecting;
    PublishQuickPlayStatus();
    QTimer::singleShot(500, this, [this, attempt] { AdvanceQuickPlayPresentation(attempt); });
    return;
  }
  if (m_quickplay_presentation != QuickPlayPresentation::Connecting)
    return;
  m_quickplay_presentation = QuickPlayPresentation::None;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: stopping local Core, attempt {}", attempt);
  Core::Stop(m_system);
}

bool QuickPlaySession::IsQuickPlayLaunchReady(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt) ||
      m_quickplay_controller->GetState() != QuickPlayController::State::NetPlayConnected ||
      m_window.IsExitRequested() || m_window.IsBootPending() ||
      (!Core::IsUninitialized(m_system) && !IsPersistentQuickPlayBoundary()) ||
      m_window.IsRenderingToMain() || Config::Get(Config::MAIN_RENDER_TO_MAIN) ||
      !m_quickplay_retain_window || m_quickplay_setup->window != m_window.GetRenderWidget() ||
      !m_window.GetRenderWidget()->isVisible() ||
      m_quickplay_setup->native_window !=
          static_cast<u64>(m_window.GetRenderWidget()->effectiveWinId()))
    return false;
  const auto server = Settings::Instance().GetNetPlayServer();
  if (m_quickplay_setup->client != Settings::Instance().GetNetPlayClient().get() ||
      m_quickplay_setup->server != server.get())
    return false;
  if (m_quickplay_host_attempt == attempt)
  {
    if (!server || server->GetQuickPlayAttempt() != attempt)
      return false;
  }
  else if (m_quickplay_join_attempt != attempt || server)
  {
    return false;
  }
  return m_netplay_dialog->IsQuickPlayReady();
}

void QuickPlaySession::CheckQuickPlayLaunch(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt))
    return;
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !client->IsConnected())
  {
    m_quickplay_controller->OnLaunchFailed(attempt, tr("The Quick Play peer disconnected."));
    return;
  }
  if (!m_quickplay_setup->start_requested && IsQuickPlayLaunchReady(attempt) &&
      !QApplication::activeModalWidget() && m_quickplay_host_attempt == attempt)
  {
    m_quickplay_setup->start_requested = true;
    INFO_LOG_FMT(NETPLAY, "QuickPlay: readiness satisfied; normal host Start Game, attempt {}",
                 attempt);
    if (!m_netplay_dialog->StartQuickPlayGame(attempt))
    {
      m_quickplay_controller->OnLaunchFailed(attempt, m_quickplay_admission_error.isEmpty() ?
                                                          tr("NetPlay could not start the game.") :
                                                          m_quickplay_admission_error);
      return;
    }
  }
  QTimer::singleShot(100, this, [this, attempt] { CheckQuickPlayLaunch(attempt); });
}

bool QuickPlaySession::IsPersistentQuickPlayBoundary() const
{
  return m_persistent_quickplay && !m_persistent_retiring &&
         Core::GetState(m_system) == Core::State::Paused;
}

void QuickPlaySession::PreparePersistentQuickPlay(u32 game)
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !m_quickplay_setup || !IsPersistentQuickPlayBoundary() ||
      !IsQuickPlayLaunchReady(m_quickplay_setup->attempt))
  {
    ReportQuickPlayError(tr("The persistent session start lost its owned readiness boundary."));
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
            ReportQuickPlayError(
                mismatch.isEmpty() ?
                    tr("The persistent session contract could not be prepared.") :
                    tr("Quick Play refused the join: this setting differs from the host's: %1. "
                       "Change it back, or restart the Offline Launcher.")
                        .arg(mismatch));
            RetirePersistentQuickPlay(true);
          }
        });
}

void QuickPlaySession::HandlePersistentBarrier(u32 game, bool release)
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !m_quickplay_setup || game != m_persistent_game ||
      !IsPersistentQuickPlayBoundary() || client.get() != m_quickplay_setup->client ||
      !client->IsConnected())
    return;
  bool ok;
  {
    const Core::CPUThreadGuard guard(m_system);
    ok = release ? client->ReleasePersistentSession(game) : client->AttachPersistentSession(game);
  }
  if (!ok)
  {
    ReportQuickPlayError(tr("The persistent session attach/release barrier failed."));
    return RetirePersistentQuickPlay(true);
  }
  if (!release)
    return;
  const auto attempt = m_quickplay_setup->attempt;
  m_quickplay_setup.reset();
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_netplay_dialog->ReleaseQuickPlayStart();
  m_quickplay_controller->OnLaunchOwned(attempt);
  Core::SetState(m_system, Core::State::Running);
  INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: active game {} same Core", game);
}

void QuickPlaySession::RetirePersistentQuickPlay(bool restore_css)
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
  const auto cause = delivered_cause.isEmpty() ? m_quickplay_admission_error : delivered_cause;
  InvalidateQuickPlayPresentation();
  m_quickplay_admission_error = cause;
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
  m_quickplay_setup.reset();
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_quickplay_retain_window = false;
  m_netplay_dialog->hide();
  m_netplay_dialog->ResetSession();
  m_quickplay_retiring_client = client;
  m_quickplay_retiring_server = Settings::Instance().GetNetPlayServer();
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();
  INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: retiring session; Core retained restore {}",
               m_persistent_restore_css);
  QTimer::singleShot(0, this, &QuickPlaySession::FinishPersistentQuickPlayRetirement);
}

void QuickPlaySession::FinishPersistentQuickPlayRetirement()
{
  if (!m_persistent_retiring)
    return;
  if (!m_quickplay_retiring_client.expired() || !m_quickplay_retiring_server.expired())
  {
    QTimer::singleShot(25, this, &QuickPlaySession::FinishPersistentQuickPlayRetirement);
    return;
  }
  // Fence notifications queued by the last network-thread destructor too.
  m_netplay_dialog->ResetSession();
  const bool restore =
      m_persistent_restore_css && Core::IsRunning(m_system) && !m_window.IsExitRequested();
  const auto finish = [session = QPointer<QuickPlaySession>(this), restore](bool ok) {
    if (!session || !session->m_persistent_retiring)
      return;
    const bool resume = restore && ok && session->m_persistent_restore_css &&
                        Core::IsRunning(session->m_system) && !session->m_window.IsExitRequested();
    session->m_persistent_quickplay.reset();
    session->m_persistent_retiring = false;
    session->m_persistent_restore_css = false;
    if (session->m_quickplay_controller)
      session->m_quickplay_controller->Cancel();
    if (resume && session->m_quickplay_cancel_consumed)
      ProjectPlusOnline::cancel_release_handoff.Arm(
          ProjectPlusOnline::presentation_core_generation.load(),
          ProjectPlusOnline::persistent_input_epoch.load());
    session->m_quickplay_cancel_consumed = false;
    if (std::exchange(session->m_quickplay_opponent_left, false))
      session->m_quickplay_opponent_left_until =
          std::chrono::steady_clock::now() + std::chrono::seconds(4);
    if (resume)
    {
      {
        const Core::CPUThreadGuard guard(session->m_system);
        QuickPlayRecipe::ReleaseExtraPorts(session->m_system);
      }
      Core::SetState(session->m_system, Core::State::Running);
    }
    session->m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
    if (!ok)
      session->ReportQuickPlayError(session->m_quickplay_admission_error +
                                    tr(" The local CSS could not be restored."));
    if (!ok)
      ERROR_LOG_FMT(NETPLAY, "Persistent QuickPlay: local CSS restore failed; keeping Core paused");
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: session retired local CSS {}", resume);
    session->PublishQuickPlayStatus();
  };
  if (!restore)
    return finish(true);
  // Leave returns to this run's hand-free local CSS; the origin (with its port 2 controller,
  // covered until the CSS drops that hand) only if the local snapshot is missing.
  PersistentQuickPlaySession::RestoreLocal(
      m_system, [session = QPointer<QuickPlaySession>(this), finish](bool ok) {
        if (ok || !session || !session->m_persistent_quickplay || !session->m_persistent_retiring)
          return finish(ok);
        session->m_persistent_quickplay->Restore(session->m_system, finish);
      });
}

void QuickPlaySession::TakeLocalQuickPlaySnapshot()
{
  // Only from the idle local CSS: never during a search, handoff, session or retirement.
  if (!Core::IsRunning(m_system) || Core::GetState(m_system) != Core::State::Running ||
      m_quickplay_setup || m_persistent_quickplay || m_persistent_retiring || m_quickplay_session ||
      m_quickplay_validating || Settings::Instance().GetNetPlayClient() ||
      Settings::Instance().GetNetPlayServer())
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
      if (Core::GetState(m_system) == Core::State::Paused && !m_quickplay_setup &&
          !m_persistent_quickplay && !m_persistent_retiring)
        Core::SetState(m_system, Core::State::Running);
    });
  });
}

void QuickPlaySession::ReleaseQuickPlayWindow()
{
  m_quickplay_presentation = QuickPlayPresentation::None;
  m_quickplay_stop_attempt = 0;
  if (std::exchange(m_quickplay_retain_window, false) && Core::IsUninitialized(m_system))
  {
    m_window.HideRenderWidget();
    m_window.SetFullScreenResolution(false);
  }
  // During Stopping the normal OnStopComplete performs the same cleanup.
}

void QuickPlaySession::EndQuickPlaySession(bool restart)
{
  if (m_quickplay_recovery != QuickPlayRecovery::None)
  {
    if (!restart)
      m_quickplay_recovery_restart = false;
    return;
  }
  if (!std::exchange(m_quickplay_session, false))
    return;

  m_quickplay_recovery = QuickPlayRecovery::WaitingForStop;
  m_quickplay_recovery_restart = restart && !m_window.IsExitRequested() &&
                                 !m_window.IsBootPending() && m_window.GetRenderWidget() &&
                                 m_window.GetRenderWidget()->isVisible();
  m_quickplay_retain_window = m_quickplay_recovery_restart;
  if (!restart)
  {
    if (const auto client = Settings::Instance().GetNetPlayClient())
      client->RequestStopGame();
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay: ending session, local restart {}",
               m_quickplay_recovery_restart);
}

void QuickPlaySession::FinishQuickPlayRecovery()
{
  if ((m_quickplay_recovery != QuickPlayRecovery::Queued &&
       m_quickplay_recovery != QuickPlayRecovery::Releasing) ||
      !Core::IsUninitialized(m_system))
    return;

  if (m_quickplay_recovery == QuickPlayRecovery::Queued)
  {
    // Core and boot configuration cleanup has finished. Release presentation
    // ownership too; the golf overlay can hold a client reference.
    m_quickplay_recovery = QuickPlayRecovery::Releasing;
    m_quickplay_retiring_client = Settings::Instance().GetNetPlayClient();
    m_quickplay_retiring_server = Settings::Instance().GetNetPlayServer();
    m_netplay_dialog->hide();
    m_netplay_dialog->ResetSession();
    Settings::Instance().ResetNetPlayClient();
    Settings::Instance().ResetNetPlayServer();
  }
  // A nested modal callback can still hold a session reference. Never boot a new
  // Core until those references unwind and both network-thread destructors finish.
  if (!m_quickplay_retiring_client.expired() || !m_quickplay_retiring_server.expired() ||
      QApplication::activeModalWidget())
  {
    QTimer::singleShot(25, this, &QuickPlaySession::FinishQuickPlayRecovery);
    return;
  }
  // Invalidate even notifications produced while the last reference was retiring.
  m_netplay_dialog->ResetSession();
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;

  if (!m_quickplay_recovery_restart || m_window.IsExitRequested() || m_window.IsBootPending() ||
      !m_quickplay_retain_window || !m_window.GetRenderWidget()->isVisible())
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_retain_window = false;
    m_quickplay_controller->Cancel();
    m_window.OnStopComplete();
    return;
  }

  const auto launcher = FindPPlusNetplayLauncher(m_game_list_model);
  if (!launcher.game)
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_controller->Cancel();
    ReleaseQuickPlayWindow();
    ModalMessageBox::critical(m_dialog_parent, tr("Quick Play"), launcher.error);
    return;
  }

  m_quickplay_recovery = QuickPlayRecovery::Booting;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: booting local recovery");
  m_window.StartGame(launcher.game->GetFilePath());
  if (Core::IsUninitialized(m_system) && m_quickplay_recovery == QuickPlayRecovery::Booting)
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_controller->Cancel();
  }
}

bool QuickPlaySession::JoinQuickPlayHost(const QString& host_code)
{
  if (!m_quickplay_setup || !m_quickplay_controller ||
      !m_quickplay_controller->OwnsAttempt(m_quickplay_setup->attempt) ||
      m_quickplay_setup->attempt != m_quickplay_owned_attempt || m_quickplay_joining ||
      m_quickplay_host_attempt || !QuickPlayClient::IsValidHostCode(host_code) ||
      (!Core::IsUninitialized(m_system) && !IsPersistentQuickPlayBoundary()) ||
      Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->HasRoom())
    return false;

  if (!FindPPlusNetplayLauncher(m_game_list_model).game)
    return false;
  m_quickplay_joining = true;
  m_quickplay_join_attempt = m_quickplay_setup ? m_quickplay_setup->attempt : 0;
  const auto previous_method = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const auto previous_code = Config::Get(Config::NETPLAY_HOST_CODE);
  Common::ScopeGuard restore([&] {
    Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, previous_method);
    Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, previous_code);
    m_quickplay_joining = false;
  });
  // Same effective settings as a traversal browser session. The connect port is unused for
  // traversal, and the configured traversal server/port stay in effect. Restore manual values.
  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, std::string("traversal"));
  Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, host_code.toStdString());
  return m_window.NetPlayJoinInternal();
}

QString QuickPlaySession::StartQuickPlayHost(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt || !m_quickplay_controller ||
      !m_quickplay_controller->OwnsAttempt(attempt) || m_quickplay_owned_attempt != attempt ||
      (!Core::IsUninitialized(m_system) && !IsPersistentQuickPlayBoundary()) ||
      Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->HasRoom())
    return tr("Close the current game or NetPlay session before hosting Quick Play.");
  const auto launcher = FindPPlusNetplayLauncher(m_game_list_model);
  if (!launcher.game)
    return launcher.error;
  m_quickplay_host_attempt = attempt;
  if (!m_window.NetPlayHostInternal(*launcher.game, attempt))
    return m_quickplay_admission_error.isEmpty() ?
               tr("Could not create the Quick Play NetPlay host. Cancel and try again.") :
               m_quickplay_admission_error;
  return {};
}

void QuickPlaySession::CancelQuickPlayHost(u64 attempt)
{
  if (!attempt)
    return;
  if (m_quickplay_validating && m_quickplay_setup && m_quickplay_setup->attempt == attempt)
    CancelQuickPlayValidation();
  if (m_persistent_quickplay && m_quickplay_setup && m_quickplay_setup->attempt == attempt)
  {
    RetirePersistentQuickPlay(true);
    return;
  }
  const bool owns_session =
      m_quickplay_host_attempt == attempt || m_quickplay_join_attempt == attempt;
  if (m_quickplay_setup && m_quickplay_setup->attempt == attempt)
  {
    m_quickplay_setup.reset();
    ReleaseQuickPlayWindow();
  }
  if (!owns_session)
    return;
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_netplay_dialog->hide();
  m_window.NetPlayQuit();
}
