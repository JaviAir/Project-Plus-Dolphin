// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <QString>

class GameListModel;
namespace UICommon
{
class GameFile;
}

struct QuickPlayLauncherResult
{
  std::shared_ptr<const UICommon::GameFile> game;
  QString error;
};

// Call on the GUI thread. A failed lookup always returns a null game and an actionable error.
// This checks launcher identity, not content compatibility; recheck before any future real host.
QuickPlayLauncherResult FindPPlusNetplayLauncher(const GameListModel& model);
