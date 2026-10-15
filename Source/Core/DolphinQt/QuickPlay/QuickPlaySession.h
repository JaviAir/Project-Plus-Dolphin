// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QString>

#include "Common/CommonTypes.h"

class GameListModel;
class NetPlayDialog;
class QuickPlayController;
class QuickPlayDialog;
class QuickPlaySettingsDialog;
class QWidget;
class ToolBar;

namespace Core
{
class System;
}

namespace UICommon
{
class GameFile;
}

// What Quick Play needs from the main window, and nothing else. MainWindow implements it.
class QuickPlayWindow
{
public:
  virtual ~QuickPlayWindow() = default;

  virtual bool IsExitRequested() const = 0;
  virtual bool IsBootPending() const = 0;

  virtual void NetPlayQuit() = 0;
  virtual bool NetPlayHostInternal(const UICommon::GameFile& game, u64 quickplay_attempt) = 0;
  virtual bool NetPlayJoinInternal() = 0;
};

// GUI-thread owner of Quick Play in the desktop frontend: the toolbar entry points, the search
// dialog, and the handoff into an ordinary NetPlay room (host or join, then cancel or close).
class QuickPlaySession final : public QObject
{
  Q_OBJECT

public:
  QuickPlaySession(QuickPlayWindow& window, Core::System& system, QWidget* parent,
                   NetPlayDialog* netplay_dialog, const GameListModel& game_list_model,
                   ToolBar* tool_bar);
  ~QuickPlaySession() override;

  // MainWindow hooks.
  void Shutdown();
  // Manual NetPlay host and join are refused while Quick Play owns NetPlay.
  bool OwnsNetPlay() const;
  // After NetPlayQuit destroyed the client and server.
  void OnNetPlayQuit();

private:
  void ShowQuickPlaySettings();
  void StartQuickPlay();
  void PrepareQuickPlayHandoff(u64 attempt);
  bool JoinQuickPlayHost(const QString& host_code);
  QString StartQuickPlayHost(u64 attempt);
  void CancelQuickPlayHost(u64 attempt);

  QuickPlayWindow& m_window;
  Core::System& m_system;
  QWidget* const m_dialog_parent;
  NetPlayDialog* const m_netplay_dialog;
  const GameListModel& m_game_list_model;
  ToolBar* const m_tool_bar;

  // The matched attempt that may create the NetPlay room, until the room is connected.
  u64 m_quickplay_setup_attempt = 0;
  bool m_quickplay_joining = false;
  u64 m_quickplay_host_attempt = 0;
  u64 m_quickplay_join_attempt = 0;
  QuickPlayController* m_quickplay_controller = nullptr;
  QuickPlayDialog* m_quickplay_dialog = nullptr;
  QuickPlaySettingsDialog* m_quickplay_settings_dialog = nullptr;
};
