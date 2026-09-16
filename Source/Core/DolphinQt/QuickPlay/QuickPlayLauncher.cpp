// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayLauncher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QModelIndex>

#include "Common/FileUtil.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "UICommon/GameFile.h"

QuickPlayLauncherResult FindPPlusNetplayLauncher(const GameListModel& model)
{
  const auto failure = [] {
    return QuickPlayLauncherResult{
        nullptr, QCoreApplication::translate(
                     "QuickPlayLauncher",
                     "Project+ Netplay Launcher could not be found or verified. Refresh the game "
                     "list or repair your Project+ installation.")};
  };
  const QString filename = QStringLiteral("Netplay Launcher.dol");
  const QString launcher_path = QString::fromStdString(File::GetUserPath(D_LAUNCHERS_IDX));
  if (launcher_path.isEmpty())
    return failure();

  const QString directory = QDir(launcher_path).canonicalPath();
  if (directory.isEmpty())
    return failure();

  const QString expected_path = QDir(directory).filePath(filename);
  std::shared_ptr<const UICommon::GameFile> launcher;
  int candidates = 0;
  for (int row = 0; row < model.rowCount(QModelIndex()); ++row)
  {
    const auto game = model.GetGameFile(row);
    if (!game)
      continue;

    const QFileInfo file(QString::fromStdString(game->GetFilePath()));
    if (file.fileName() != filename || file.dir().canonicalPath() != directory)
      continue;

    // Count entries at the expected location before validating their metadata. Even conflicting
    // stale entries must fail closed instead of choosing whichever valid entry appears first.
    if (++candidates > 1)
    {
      return {nullptr,
              QCoreApplication::translate(
                  "QuickPlayLauncher",
                  "Multiple Project+ Netplay Launcher entries were found. Remove duplicate "
                  "game-list entries and refresh the game list, or repair your Project+ "
                  "installation.")};
    }

    if (game->GetFileName() == "Offline Launcher.dol" || game->GetGameID() == "ID-Offline Launcher")
      continue;

    // Canonical equality rejects a launcher symlink pointing outside the directory or at Offline.
    // Filesystem checks also reject cached entries whose file has since been deleted.
    if (!game->IsValid() || game->GetPlatform() != DiscIO::Platform::ELFOrDOL ||
        game->GetFileName() != "Netplay Launcher.dol" ||
        game->GetGameID() != "ID-Netplay Launcher" || !file.isFile() || !file.isReadable() ||
        file.canonicalFilePath() != expected_path)
      continue;

    launcher = game;
  }

  if (!launcher)
    return failure();

  return {launcher, {}};
}
