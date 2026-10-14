// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include <QString>

class GameListModel;
namespace UICommon
{
class GameFile;
}

// Official Project+ v3.2 Netplay content names the launcher "Project+ Netplay Launcher.dol";
// this repository's Data/user copy is "Netplay Launcher.dol". The first existing name wins.
inline constexpr std::array NETPLAY_LAUNCHER_NAMES{"Project+ Netplay Launcher.dol",
                                                   "Netplay Launcher.dol"};

struct QuickPlayLauncherResult
{
  std::shared_ptr<const UICommon::GameFile> game;
  QString error;
};

// Call on the GUI thread. A failed lookup always returns a null game and an actionable error.
// This checks launcher identity, not content compatibility; recheck before any future real host.
QuickPlayLauncherResult FindPPlusNetplayLauncher(const GameListModel& model);
