// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlaySession.h"

#include <string>
#include <utility>

#include <QTimer>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/NetPlay/NetPlayDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QuickPlay/QuickPlayController.h"
#include "DolphinQt/QuickPlay/QuickPlayDialog.h"
#include "DolphinQt/QuickPlay/QuickPlayLauncher.h"
#include "DolphinQt/QuickPlay/QuickPlaySettingsDialog.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/ToolBar.h"
#include "UICommon/GameFile.h"

QuickPlaySession::QuickPlaySession(QuickPlayWindow& window, Core::System& system, QWidget* parent,
                                   NetPlayDialog* netplay_dialog,
                                   const GameListModel& game_list_model, ToolBar* tool_bar)
    : QObject(parent), m_window(window), m_system(system), m_dialog_parent(parent),
      m_netplay_dialog(netplay_dialog), m_game_list_model(game_list_model), m_tool_bar(tool_bar)
{
  connect(tool_bar, &ToolBar::QuickPlayPressed, this, &QuickPlaySession::StartQuickPlay);
  connect(tool_bar, &ToolBar::QuickPlaySettingsPressed, this,
          &QuickPlaySession::ShowQuickPlaySettings);
}

QuickPlaySession::~QuickPlaySession() = default;

void QuickPlaySession::Shutdown()
{
  // Destroy orchestration while its MainWindow callbacks and NetPlay dialog are still alive.
  delete m_quickplay_dialog;
  m_quickplay_dialog = nullptr;
  delete m_quickplay_controller;
  m_quickplay_controller = nullptr;
}

bool QuickPlaySession::OwnsNetPlay() const
{
  return m_quickplay_setup_attempt || m_quickplay_host_attempt || m_quickplay_joining ||
         (m_quickplay_controller &&
          m_quickplay_controller->GetState() != QuickPlayController::State::Idle &&
          m_quickplay_controller->GetState() != QuickPlayController::State::Error);
}

void QuickPlaySession::OnNetPlayQuit()
{
  const auto attempt = std::exchange(m_quickplay_setup_attempt, 0);
  m_quickplay_join_attempt = 0;
  m_quickplay_host_attempt = 0;
  if (!m_quickplay_controller)
    return;
  if (attempt)
    m_quickplay_controller->OnHostClosed(attempt);
  else if (m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
    m_quickplay_controller->Cancel();
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

void QuickPlaySession::StartQuickPlay()
{
  if (m_window.IsExitRequested() || m_window.IsBootPending())
    return;
  if (m_netplay_dialog->isVisible())
  {
    // An open NetPlay room takes precedence: bring it forward instead of searching.
    m_netplay_dialog->raise();
    m_netplay_dialog->activateWindow();
    return;
  }
  if (Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient())
    return;
  if (m_quickplay_settings_dialog && m_quickplay_settings_dialog->isVisible())
    return;
  if (m_quickplay_controller &&
      m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
  {
    // An existing attempt is revealed without starting another one.
    m_quickplay_dialog->show();
    m_quickplay_dialog->raise();
    m_quickplay_dialog->activateWindow();
    return;
  }
  if (!Core::IsUninitialized(m_system))
  {
    ModalMessageBox::critical(m_dialog_parent, tr("Quick Play"),
                              tr("Close the running game before starting Quick Play."));
    return;
  }

  m_tool_bar->SetQuickPlayActive(true);
  if (!m_quickplay_controller)
  {
    m_quickplay_controller = new QuickPlayController(
        [this](u64 attempt) { return StartQuickPlayHost(attempt); },
        [this](u64 attempt) { CancelQuickPlayHost(attempt); },
        [this](const QString& code) {
          return JoinQuickPlayHost(code) ?
                     QString{} :
                     tr("Could not join the Quick Play host. Cancel and try again.");
        },
        this);
    connect(m_quickplay_controller, &QuickPlayController::PrepareHandoff, this,
            &QuickPlaySession::PrepareQuickPlayHandoff);
    connect(m_netplay_dialog, &NetPlayDialog::HostTraversalChanged, m_quickplay_controller,
            &QuickPlayController::OnHostTraversalChanged);
    connect(m_netplay_dialog, &NetPlayDialog::QuickPlayOpponentConnected, m_quickplay_controller,
            &QuickPlayController::OnHostOpponentConnected);
    connect(m_quickplay_controller, &QuickPlayController::StateChanged, this,
            [this](QuickPlayController::State state) {
              if (state == QuickPlayController::State::Error)
              {
                INFO_LOG_FMT(NETPLAY, "QuickPlay: error shown: {}",
                             m_quickplay_controller->GetErrorMessage().toStdString());
              }
              if (state == QuickPlayController::State::NetPlayConnected &&
                  m_quickplay_setup_attempt)
              {
                // The ordinary NetPlay room now belongs to the user.
                m_quickplay_host_attempt = 0;
                m_quickplay_join_attempt = 0;
                m_quickplay_controller->OnLaunchOwned(std::exchange(m_quickplay_setup_attempt, 0));
              }
              m_tool_bar->SetQuickPlayActive(state != QuickPlayController::State::Idle &&
                                             state != QuickPlayController::State::NetPlayOwned);
            });
    m_quickplay_dialog = new QuickPlayDialog(*m_quickplay_controller, m_dialog_parent);
  }

  m_quickplay_controller->Start();
  m_quickplay_dialog->show();
  m_quickplay_dialog->raise();
  m_quickplay_dialog->activateWindow();
}

void QuickPlaySession::PrepareQuickPlayHandoff(u64 attempt)
{
  if (!m_quickplay_controller->OwnsAttempt(attempt))
    return;
  if (m_window.IsExitRequested() || m_window.IsBootPending() ||
      Settings::Instance().IsBatchModeEnabled() || Settings::Instance().GetNetPlayClient() ||
      Settings::Instance().GetNetPlayServer() || m_netplay_dialog->isVisible())
  {
    m_quickplay_controller->OnHandoffReady(
        attempt, tr("Close the current NetPlay session before Quick Play connects."));
    return;
  }
  if (!Core::IsUninitialized(m_system))
  {
    m_quickplay_controller->OnHandoffReady(
        attempt, tr("Close the running game before Quick Play connects."));
    return;
  }
  m_quickplay_setup_attempt = attempt;
  // Continue outside the controller's own state change.
  QTimer::singleShot(0, this, [this, attempt] {
    if (!m_window.IsExitRequested())
      m_quickplay_controller->OnHandoffReady(attempt);
  });
}

bool QuickPlaySession::JoinQuickPlayHost(const QString& host_code)
{
  if (!m_quickplay_setup_attempt || !m_quickplay_controller ||
      !m_quickplay_controller->OwnsAttempt(m_quickplay_setup_attempt) || m_quickplay_joining ||
      m_quickplay_host_attempt || !QuickPlayMatchmaker::IsValidHostCode(host_code) ||
      !Core::IsUninitialized(m_system) || Settings::Instance().GetNetPlayServer() ||
      Settings::Instance().GetNetPlayClient() || m_netplay_dialog->isVisible())
    return false;

  if (!FindPPlusNetplayLauncher(m_game_list_model).game)
    return false;
  m_quickplay_joining = true;
  m_quickplay_join_attempt = m_quickplay_setup_attempt;
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
  if (m_quickplay_setup_attempt != attempt || !m_quickplay_controller ||
      !m_quickplay_controller->OwnsAttempt(attempt) || !Core::IsUninitialized(m_system) ||
      Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->isVisible())
    return tr("Close the current game or NetPlay session before hosting Quick Play.");
  const auto launcher = FindPPlusNetplayLauncher(m_game_list_model);
  if (!launcher.game)
    return launcher.error;
  m_quickplay_host_attempt = attempt;
  if (!m_window.NetPlayHostInternal(*launcher.game, attempt))
    return tr("Could not create the Quick Play NetPlay host. Cancel and try again.");
  return {};
}

void QuickPlaySession::CancelQuickPlayHost(u64 attempt)
{
  if (!attempt)
    return;
  const bool owns_session =
      m_quickplay_host_attempt == attempt || m_quickplay_join_attempt == attempt;
  if (m_quickplay_setup_attempt == attempt)
    m_quickplay_setup_attempt = 0;
  if (!owns_session)
    return;
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_netplay_dialog->hide();
  m_window.NetPlayQuit();
}
