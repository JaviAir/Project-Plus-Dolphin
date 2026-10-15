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
#include "Core/SyncIdentifier.h"
#include "DolphinQt/QuickPlay/ContentPolicy.h"
#include "DolphinQt/QuickPlay/QuickPlayController.h"

class GameListModel;
class HotkeyScheduler;
class NetPlayDialog;
class PersistentQuickPlaySession;
class QuickPlaySession;
class QuickPlayWindow;
class QWidget;

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

// Seamless (in-game) Quick Play on top of QuickPlaySession: the same-Core persistent session,
// the content admission, the in-game status overlay, and the in-game search, cancel and leave
// chords. QuickPlaySession calls the seam points below; this class reads and resets the
// session's state directly.
class SeamlessQuickPlay final : public QObject
{
  Q_OBJECT

public:
  SeamlessQuickPlay(QuickPlaySession& session, HotkeyScheduler* hotkey_scheduler);
  ~SeamlessQuickPlay() override;

  // A same-Core session is admitted (set up or running).
  bool IsActive() const { return m_persistent_quickplay != nullptr; }
  bool IsRetiring() const { return m_persistent_retiring; }
  bool IsValidating() const { return m_quickplay_validating; }
  bool IsSetupPaused() const;
  bool IsPersistentQuickPlayBoundary() const;
  // The origin to search with, or empty for an ordinary search.
  std::string OriginDigest() const;
  std::optional<QuickPlayContent::TrialLauncher> TrialLauncher() const;
  // The admitted session's declared room game, if it has one.
  std::optional<NetPlay::SyncIdentifier> DeclaredGame() const;
  // An error when a player of the admitted room reports a different room game: the declared
  // game hashes the build, so they are on another build. Empty otherwise.
  QString RoomGameMismatch() const;

  void PublishQuickPlayStatus();

  // Seam points.
  void Shutdown();
  void OnEmulationStateChanged(Core::State state);
  void OnRequestStop();
  // True: the persistent session took over this stop.
  bool HandleRequestStop();
  void OnForceStop();
  void OnReset();
  bool HandleNetPlayStop();
  bool HandleNetPlayQuit();
  void OnClientCreated();
  void OnError(const QString& error);
  void OnShowDiagnostics();
  // Admission before a search from a running game. True: it calls BeginQuickPlaySearch itself.
  bool StartSearch();
  void OnStateChanged(QuickPlayController::State state, QuickPlayController::State old_state);
  void OnHandoff();
  // True: the persistent handoff took over.
  bool PrepareHandoff(u64 attempt);
  // Blocks setup input and paints the paused status before `connect(presented)` runs. `connect`
  // returns false when the attempt is no longer current.
  void PrepareConnection(std::function<bool(bool)> connect);
  // True: the persistent session handled the cancel.
  bool CancelHost(u64 attempt);

private:
  ProjectPlusOnline::PresentationIdentity QuickPlayIdentity() const;
  bool OwnsInGameQuickPlay() const;
  void ObservePausedQuickPlayCSS();
  void PublishQuickPlayPresentation();
  void RedrawQuickPlayPresentation(std::function<void(bool)> finished = {});
  void ValidateQuickPlayContent(u64 attempt, std::function<void(bool, QString)> finished);
  void InvalidateQuickPlayPresentation();
  void CancelQuickPlayValidation();
  void PreparePersistentQuickPlay(u32 game);
  void HandlePersistentBarrier(u32 game, bool release);
  void RetirePersistentQuickPlay(bool restore_css);
  void FinishPersistentQuickPlayRetirement();
  void TakeLocalQuickPlaySnapshot();

  QuickPlaySession& m_session;
  QuickPlayWindow& m_window;
  Core::System& m_system;
  QWidget* const m_dialog_parent;
  NetPlayDialog* const m_netplay_dialog;
  const GameListModel& m_game_list_model;

  std::unique_ptr<PersistentQuickPlaySession> m_persistent_quickplay;
  std::unique_ptr<PersistentQuickPlaySession> m_persistent_candidate;
  bool m_quickplay_cancel_consumed = false;
  std::optional<ProjectPlusOnline::PresentationIdentity> m_quickplay_paused_css;
  bool m_persistent_retiring = false;
  bool m_persistent_restore_css = false;
  u32 m_persistent_game = 0;
  const NetPlay::NetPlayClient* m_persistent_client = nullptr;
  const NetPlay::NetPlayServer* m_persistent_server = nullptr;
  u64 m_quickplay_owner_core = 0;
  bool m_quickplay_validation_was_running = false;
  std::optional<ProjectPlusOnline::PrepaintFence> m_quickplay_validation_fence;
  u64 m_quickplay_room_generation = 1;
  u64 m_quickplay_validation_request = 0;
  bool m_quickplay_validating = false;
  bool m_quickplay_blocking_connection = false;
  bool m_quickplay_redraw_pending = false;
  std::vector<std::function<void(bool)>> m_quickplay_redraw_callbacks;
  std::chrono::steady_clock::time_point m_quickplay_cancelled_until{};
  // Shows why Quick Play preparation failed for a few seconds after it happens.
  std::optional<std::chrono::steady_clock::time_point> m_quickplay_recipe_failure_until;
  // Set by an accepted local leave chord: Leaving… during retirement, then Left session.
  std::chrono::steady_clock::time_point m_quickplay_left_until{};
  // The opponent ended the shared session: shown while retiring, then on the restored CSS.
  bool m_quickplay_opponent_left = false;
  std::chrono::steady_clock::time_point m_quickplay_opponent_left_until{};
};
