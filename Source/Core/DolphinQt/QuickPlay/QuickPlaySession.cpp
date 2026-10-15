// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlaySession.h"

#include <utility>

#include <QApplication>
#include <QGuiApplication>
#include <QTimer>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/ProjectPlusOnline/LobbyPresentation.h"
#include "Core/System.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/MenuBar.h"
#include "DolphinQt/NetPlay/NetPlayDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QuickPlay/QuickPlayController.h"
#include "DolphinQt/QuickPlay/QuickPlayDialog.h"
#include "DolphinQt/QuickPlay/QuickPlayLauncher.h"
#include "DolphinQt/QuickPlay/QuickPlaySettingsDialog.h"
#include "DolphinQt/QuickPlay/SeamlessQuickPlay.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/ToolBar.h"
#include "UICommon/GameFile.h"

// Seamless Quick Play supports X11 (including XWayland) and Windows.
bool QuickPlaySession::IsQuickPlayWindowSystem()
{
  const QString platform_name = QGuiApplication::platformName();
  return platform_name == QStringLiteral("xcb") || platform_name == QStringLiteral("windows");
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
  connect(m_netplay_dialog, &NetPlayDialog::QuickPlayStartAborted, this, [this](quint64 attempt) {
    // Defer destruction until the normal NetPlay callback has returned.
    QTimer::singleShot(0, this, [this, attempt] {
      if (m_quickplay_controller)
        m_quickplay_controller->OnLaunchFailed(attempt, tr("NetPlay game start was aborted."));
    });
  });
  connect(m_netplay_dialog, &NetPlayDialog::Stop, this, &QuickPlaySession::OnNetPlayStop);

  m_seamless = std::make_unique<SeamlessQuickPlay>(*this, hotkey_scheduler);
}

QuickPlaySession::~QuickPlaySession() = default;

void QuickPlaySession::Shutdown()
{
  m_seamless->Shutdown();
  // Destroy orchestration while its MainWindow callbacks and NetPlay dialog are still alive.
  delete m_quickplay_dialog;
  m_quickplay_dialog = nullptr;
  delete m_quickplay_controller;
  m_quickplay_controller = nullptr;
}

void QuickPlaySession::OnEmulationStateChanged(Core::State state)
{
  m_seamless->OnEmulationStateChanged(state);
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
  return m_seamless->IsSetupPaused();
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
  m_seamless->OnRequestStop();
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
  if (m_seamless->HandleRequestStop())
    return true;
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
  m_seamless->OnForceStop();
}

void QuickPlaySession::OnReset()
{
  m_seamless->OnReset();
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
  if (m_seamless->HandleNetPlayStop())
    return;
  EndQuickPlaySession(true);
  m_window.ForceStop();
  if (m_quickplay_recovery == QuickPlayRecovery::WaitingForStop && Core::IsUninitialized(m_system))
    m_window.OnStopComplete();
}

bool QuickPlaySession::OwnsNetPlay() const
{
  return m_quickplay_setup || m_quickplay_host_attempt || m_quickplay_joining ||
         m_seamless->IsActive() || m_seamless->IsRetiring() || m_quickplay_session ||
         m_seamless->IsValidating() || m_quickplay_recovery != QuickPlayRecovery::None ||
         (m_quickplay_controller &&
          m_quickplay_controller->GetState() != QuickPlayController::State::Idle &&
          m_quickplay_controller->GetState() != QuickPlayController::State::Error);
}

bool QuickPlaySession::CanJoinWhileRunning() const
{
  return m_seamless->IsPersistentQuickPlayBoundary() &&
         (m_quickplay_joining || m_quickplay_host_attempt);
}

bool QuickPlaySession::CanHostWhileRunning(u64 quickplay_attempt) const
{
  return quickplay_attempt && m_quickplay_setup &&
         m_quickplay_setup->attempt == quickplay_attempt &&
         m_seamless->IsPersistentQuickPlayBoundary();
}

bool QuickPlaySession::IsRetiring() const
{
  return m_seamless->IsRetiring() || m_quickplay_recovery != QuickPlayRecovery::None;
}

bool QuickPlaySession::MayJoinOwnServer(const NetPlay::NetPlayServer& server) const
{
  return !m_quickplay_setup || (m_quickplay_controller->OwnsAttempt(m_quickplay_setup->attempt) &&
                                server.GetQuickPlayAttempt() == m_quickplay_setup->attempt);
}

bool QuickPlaySession::IsPersistent() const
{
  return m_seamless->IsActive();
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
        m_seamless->IsActive(), m_quickplay_setup ? m_quickplay_setup->attempt : 0,
        m_seamless->TrialLauncher());
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
    m_seamless->OnClientCreated();
  }
}

void QuickPlaySession::PrepareHost(u64 quickplay_attempt)
{
  // Establish presentation before the server or local client can report synchronous errors.
  m_netplay_dialog->BeginSession(
      quickplay_attempt ? m_quickplay_entry : ProjectPlusOnline::EntrySource::Desktop,
      m_seamless->IsActive(), quickplay_attempt, m_seamless->TrialLauncher());
}

NetPlay::SyncIdentifier QuickPlaySession::RoomGame(const UICommon::GameFile& game) const
{
  if (const auto declared = m_seamless->DeclaredGame())
    return *declared;
  return game.GetSyncIdentifier();
}

std::optional<u64> QuickPlaySession::PrepareNetPlayQuit()
{
  if (m_seamless->HandleNetPlayQuit())
    return std::nullopt;
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

void QuickPlaySession::ReportQuickPlayError(const QString& error)
{
  INFO_LOG_FMT(NETPLAY, "QuickPlay: error shown: {}", error.toStdString());
  m_quickplay_admission_error = error;
  m_netplay_dialog->SetDiagnosticError(error);
  m_seamless->OnError(error);
}

void QuickPlaySession::ShowQuickPlayDiagnostics()
{
  m_seamless->OnShowDiagnostics();
  if (!m_quickplay_admission_error.isEmpty())
    m_netplay_dialog->SetDiagnosticError(m_quickplay_admission_error);
  m_netplay_dialog->ShowDiagnostics();
}

void QuickPlaySession::PrepareQuickPlayConnection(u64 attempt, bool host)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt))
    return;
  m_seamless->PrepareConnection([this, attempt, host](bool presented) {
    if (!m_quickplay_controller->OwnsAttempt(attempt) || !m_quickplay_setup ||
        m_quickplay_setup->attempt != attempt)
      return false;
    const bool dispatchable = !m_window.IsExitRequested() && !m_window.IsBootPending() &&
                              !m_window.IsStopConfirmShowing() &&
                              !QApplication::activeModalWidget();
    const QString error = presented && dispatchable ?
                              QString{} :
                              tr("Quick Play could not display its paused connection status. Open "
                                 "Tools > Quick Play Diagnostics.");
    m_quickplay_controller->ContinueConnection(attempt, host, error);
    return true;
  });
}

void QuickPlaySession::StartQuickPlay()
{
  StartQuickPlayFrom(ProjectPlusOnline::EntrySource::Desktop);
}

void QuickPlaySession::StartQuickPlayFrom(ProjectPlusOnline::EntrySource entry)
{
  if (m_seamless->IsValidating() || m_quickplay_recovery != QuickPlayRecovery::None ||
      m_seamless->IsRetiring() || m_window.IsExitRequested() || m_window.IsBootPending())
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
  m_quickplay_admission_error.clear();
  m_netplay_dialog->SetDiagnosticError({});
  if (m_seamless->StartSearch())
    return;
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
        [this] { return m_seamless->OriginDigest(); }, this);
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
                m_quickplay_owned_attempt = m_quickplay_controller->GetGuestCancelAttempt();
              if (state == QuickPlayController::State::Idle)
              {
                m_quickplay_owned_attempt = 0;
                if (m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame &&
                    m_quickplay_dialog)
                  m_quickplay_dialog->hide();
              }
              m_seamless->OnStateChanged(state, old_state);
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
  m_seamless->PublishQuickPlayStatus();
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
  m_seamless->OnHandoff();
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
  if (m_seamless->PrepareHandoff(attempt))
    return;
  m_quickplay_stop_attempt = attempt;
  m_quickplay_retain_window = true;
  m_quickplay_setup->window = m_window.GetRenderWidget();
  m_quickplay_setup->native_window = static_cast<u64>(m_window.GetRenderWidget()->winId());
  // Bounded local presentation stages owned by the assigned handoff. The guest
  // renders snapshots only; no ACK, guest timer or renderer can hold up shutdown.
  m_quickplay_presentation = QuickPlayPresentation::Matched;
  m_seamless->PublishQuickPlayStatus();
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
    m_seamless->PublishQuickPlayStatus();
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
      (!Core::IsUninitialized(m_system) && !m_seamless->IsPersistentQuickPlayBoundary()) ||
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
  if (const auto mismatch = m_seamless->RoomGameMismatch(); !mismatch.isEmpty())
  {
    m_quickplay_controller->OnLaunchFailed(attempt, mismatch);
    return;
  }
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
      m_quickplay_host_attempt || !QuickPlayMatchmaker::IsValidHostCode(host_code) ||
      (!Core::IsUninitialized(m_system) && !m_seamless->IsPersistentQuickPlayBoundary()) ||
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
      (!Core::IsUninitialized(m_system) && !m_seamless->IsPersistentQuickPlayBoundary()) ||
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
  if (m_seamless->CancelHost(attempt))
    return;
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
