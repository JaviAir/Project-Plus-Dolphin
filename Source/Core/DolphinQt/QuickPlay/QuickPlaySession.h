// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "Common/CommonTypes.h"
#include "Core/ProjectPlusOnline/Presentation.h"
#include "Core/ProjectPlusOnline/SetupAction.h"
#include "Core/SyncIdentifier.h"

class GameListModel;
class HotkeyScheduler;
class MenuBar;
class NetPlayDialog;
class PersistentQuickPlaySession;
class QuickPlayController;
class QuickPlayDialog;
class QuickPlaySettingsDialog;
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
  void ShowQuickPlaySettings();
  void StartQuickPlay();
  void StartQuickPlayFrom(ProjectPlusOnline::EntrySource entry);
  void BeginQuickPlaySearch();
  void PublishQuickPlayPresentation();
  ProjectPlusOnline::PresentationIdentity QuickPlayIdentity() const;
  bool OwnsInGameQuickPlay() const;
  void ObservePausedQuickPlayCSS();
  void PrepareQuickPlayConnection(u64 attempt, bool host);
  void ReportQuickPlayError(const QString& error);
  void RedrawQuickPlayPresentation(std::function<void(bool)> finished = {});
  void ValidateQuickPlayContent(u64 attempt, std::function<void(bool, QString)> finished);
  void InvalidateQuickPlayPresentation();
  void CancelQuickPlayValidation();
  void PublishQuickPlayStatus();
  void PrepareQuickPlayHandoff(u64 attempt);
  void PreparePersistentQuickPlay(u32 game);
  void HandlePersistentBarrier(u32 game, bool release);
  void RetirePersistentQuickPlay(bool restore_css);
  void FinishPersistentQuickPlayRetirement();
  void TakeLocalQuickPlaySnapshot();
  bool IsPersistentQuickPlayBoundary() const;
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
  std::unique_ptr<PersistentQuickPlaySession> m_persistent_quickplay;
  std::unique_ptr<PersistentQuickPlaySession> m_persistent_candidate;
  bool m_quickplay_cancel_consumed = false;
  std::optional<ProjectPlusOnline::PresentationIdentity> m_quickplay_paused_css;
  bool m_persistent_retiring = false;
  bool m_persistent_restore_css = false;
  u32 m_persistent_game = 0;
  const NetPlay::NetPlayClient* m_persistent_client = nullptr;
  const NetPlay::NetPlayServer* m_persistent_server = nullptr;
  ProjectPlusOnline::EntrySource m_quickplay_entry = ProjectPlusOnline::EntrySource::Desktop;
  u64 m_quickplay_owned_attempt = 0;
  u64 m_quickplay_owner_core = 0;
  bool m_quickplay_validation_was_running = false;
  std::optional<ProjectPlusOnline::PrepaintFence> m_quickplay_validation_fence;
  u64 m_quickplay_room_generation = 1;
  u64 m_quickplay_validation_request = 0;
  bool m_quickplay_validating = false;
  bool m_quickplay_blocking_connection = false;
  bool m_quickplay_redraw_pending = false;
  QString m_quickplay_admission_error;
  std::vector<std::function<void(bool)>> m_quickplay_redraw_callbacks;
  std::chrono::steady_clock::time_point m_quickplay_cancelled_until{};
  // Shows why Quick Play preparation failed for a few seconds after it happens.
  std::optional<std::chrono::steady_clock::time_point> m_quickplay_recipe_failure_until;
  // Set by an accepted local leave chord: Leaving… during retirement, then Left session.
  std::chrono::steady_clock::time_point m_quickplay_left_until{};
  // The opponent ended the shared session: shown while retiring, then on the restored CSS.
  bool m_quickplay_opponent_left = false;
  std::chrono::steady_clock::time_point m_quickplay_opponent_left_until{};
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
};
