// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <QObject>
#include <QString>

#include "Common/CommonTypes.h"
#include "Core/ProjectPlusOnline/LobbyPresentation.h"
#include "Core/SyncIdentifier.h"

class GameListModel;
class HotkeyScheduler;
class MenuBar;
class NetPlayDialog;
class QuickPlayController;
class QuickPlayDialog;
class QuickPlaySettingsDialog;
class SeamlessQuickPlay;
class QWidget;
class RenderWidget;
class ToolBar;

namespace Core
{
enum class State;
class System;
}  // namespace Core

namespace NetPlay
{
class NetPlayClient;
class NetPlayServer;
}  // namespace NetPlay

namespace UICommon
{
class GameFile;
}

// What Quick Play needs from the main window, and nothing else. MainWindow implements it.
// Queries and actions are synchronous: Quick Play relies on their exact order.
class QuickPlayWindow
{
public:
  virtual ~QuickPlayWindow() = default;

  virtual bool IsExitRequested() const = 0;
  virtual bool IsBootPending() const = 0;
  virtual bool IsStopConfirmShowing() const = 0;
  virtual bool IsRenderingToMain() const = 0;
  // Can change between calls: the main window recreates its render widget.
  virtual RenderWidget* GetRenderWidget() const = 0;

  virtual void ForceStop() = 0;
  virtual void OnStopComplete() = 0;
  virtual void NetPlayQuit() = 0;
  virtual void HideRenderWidget() = 0;
  virtual void SetFullScreenResolution(bool fullscreen) = 0;
  virtual void StartGame(const std::string& path) = 0;
  virtual bool NetPlayHostInternal(const UICommon::GameFile& game, u64 quickplay_attempt) = 0;
  virtual bool NetPlayJoinInternal() = 0;
};

// GUI-thread owner of Quick Play in the desktop frontend: search, handoff, launch checks,
// host/join, cancel, recovery and session end. MainWindow forwards its own lifecycle events
// (stop, NetPlay quit, boot, emulation state) through the hooks below.
class QuickPlaySession final : public QObject
{
  Q_OBJECT

public:
  // Host-owned setup values, captured before BootManager restores per-game config.
  struct QuickPlaySetup
  {
    u64 attempt;
    std::string traversal_host;
    u16 traversal_port;
    u16 traversal_port_alt;
    u16 listen_port;
    std::string nickname;
    std::string network_mode;
    u32 minimum_buffer;
    bool use_upnp;
    RenderWidget* window = nullptr;
    u64 native_window = 0;
    bool start_requested = false;
    const NetPlay::NetPlayClient* client = nullptr;
    const NetPlay::NetPlayServer* server = nullptr;
  };

  QuickPlaySession(QuickPlayWindow& window, Core::System& system, QWidget* parent,
                   NetPlayDialog* netplay_dialog, const GameListModel& game_list_model,
                   ToolBar* tool_bar, MenuBar* menu_bar, HotkeyScheduler* hotkey_scheduler);
  ~QuickPlaySession() override;

  void ShowQuickPlayDiagnostics();
  // Seamless Quick Play supports X11 (including XWayland) and Windows.
  static bool IsQuickPlayWindowSystem();

  // MainWindow lifecycle hooks.
  void Shutdown();
  void OnEmulationStateChanged(Core::State state);
  // Play and frame advance are refused while a seamless setup pause is owned.
  bool IsSetupPaused() const;
  // OnStopComplete with the Core uninitialized. True: Quick Play owns this stop.
  bool HandleStopComplete();
  void OnRequestStop();
  // True: Quick Play stopped the Core itself.
  bool HandleRequestStop();
  void OnForceStop();
  void OnReset();
  // Only the guarded NetPlay BootGame callback may consume an owned handoff.
  bool OwnsBoot() const;
  void OnBoot();
  // The NetPlay BootGame callback, before booting. False: this boot must not start.
  bool OnNetPlayBoot();
  // Manual NetPlay host and join are refused while Quick Play owns NetPlay.
  bool OwnsNetPlay() const;

  // NetPlay room setup hooks.
  bool CanJoinWhileRunning() const;
  bool CanHostWhileRunning(u64 quickplay_attempt) const;
  bool IsRetiring() const;
  bool MayJoinOwnServer(const NetPlay::NetPlayServer& server) const;
  bool IsPersistent() const;
  std::optional<QuickPlaySetup> JoinSetup() const;
  const std::optional<QuickPlaySetup>& GetSetup() const { return m_quickplay_setup; }
  void PrepareJoin(bool local_join);
  void OnClientCreated();
  void PrepareHost(u64 quickplay_attempt);
  NetPlay::SyncIdentifier RoomGame(const UICommon::GameFile& game) const;
  // NetPlayQuit before the client and server are destroyed. nullopt: Quick Play handled it.
  std::optional<u64> PrepareNetPlayQuit();
  void FinishNetPlayQuit(u64 attempt);

private:
  friend class SeamlessQuickPlay;

  void ShowQuickPlaySettings();
  void StartQuickPlay();
  void StartQuickPlayFrom(ProjectPlusOnline::EntrySource entry);
  void BeginQuickPlaySearch();
  void PrepareQuickPlayConnection(u64 attempt, bool host);
  void ReportQuickPlayError(const QString& error);
  void PrepareQuickPlayHandoff(u64 attempt);
  void AdvanceQuickPlayPresentation(u64 attempt);
  void ReleaseQuickPlayWindow();
  void EndQuickPlaySession(bool restart);
  void FinishQuickPlayRecovery();
  void OnNetPlayStop();
  bool JoinQuickPlayHost(const QString& host_code);
  QString StartQuickPlayHost(u64 attempt);
  void CancelQuickPlayHost(u64 attempt);
  bool IsQuickPlayLaunchReady(u64 attempt);
  void CheckQuickPlayLaunch(u64 attempt);

  QuickPlayWindow& m_window;
  Core::System& m_system;
  QWidget* const m_dialog_parent;
  NetPlayDialog* const m_netplay_dialog;
  const GameListModel& m_game_list_model;
  ToolBar* const m_tool_bar;

  bool m_quickplay_joining = false;
  u64 m_quickplay_host_attempt = 0;
  std::optional<QuickPlaySetup> m_quickplay_setup;
  ProjectPlusOnline::EntrySource m_quickplay_entry = ProjectPlusOnline::EntrySource::Desktop;
  u64 m_quickplay_owned_attempt = 0;
  QString m_quickplay_admission_error;
  enum class QuickPlayPresentation
  {
    None,
    Matched,
    Connecting
  };
  QuickPlayPresentation m_quickplay_presentation = QuickPlayPresentation::None;
  u64 m_quickplay_stop_attempt = 0;
  u64 m_quickplay_join_attempt = 0;
  bool m_quickplay_retain_window = false;
  // Only an in-game Quick Play launch owns automatic local frontend recovery.
  bool m_quickplay_session = false;
  enum class QuickPlayRecovery
  {
    None,
    WaitingForStop,
    Queued,
    Releasing,
    Booting
  };
  QuickPlayRecovery m_quickplay_recovery = QuickPlayRecovery::None;
  bool m_quickplay_recovery_restart = false;
  std::weak_ptr<NetPlay::NetPlayClient> m_quickplay_retiring_client;
  std::weak_ptr<NetPlay::NetPlayServer> m_quickplay_retiring_server;
  QuickPlayController* m_quickplay_controller = nullptr;
  QuickPlayDialog* m_quickplay_dialog = nullptr;
  QuickPlaySettingsDialog* m_quickplay_settings_dialog = nullptr;
  // Last: destroyed first, while the session state it reads is still alive.
  std::unique_ptr<SeamlessQuickPlay> m_seamless;
};
