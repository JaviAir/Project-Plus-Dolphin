// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayLauncher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QModelIndex>

#include <memory>
#include <string>

#include "Common/FileUtil.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/QuickPlay/ContentPolicy.h"
#include "UICommon/GameFile.h"

namespace
{
QuickPlayLauncherResult FindNamedLauncher(const GameListModel& model, const QString& directory,
                                          const QString& filename)
{
  const QString expected_path = QDir(directory).filePath(filename);
  // A DOL's game ID is "ID-" plus its file name without extension.
  const std::string game_id = "ID-" + QFileInfo(filename).completeBaseName().toStdString();
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

    if (game->GetGameID().find("Offline Launcher") != std::string::npos)
      continue;

    // Canonical equality rejects a launcher symlink pointing outside the directory or at Offline.
    // Filesystem checks also reject cached entries whose file has since been deleted.
    if (!game->IsValid() || game->GetPlatform() != DiscIO::Platform::ELFOrDOL ||
        game->GetFileName() != filename.toStdString() || game->GetGameID() != game_id ||
        !file.isFile() || !file.isReadable() || file.canonicalFilePath() != expected_path)
      continue;

    launcher = game;
  }
  return {launcher, {}};
}
}  // namespace

QuickPlayLauncherResult FindPPlusNetplayLauncher(const GameListModel& model)
{
  const auto failure = [] {
    return QuickPlayLauncherResult{
        nullptr, QCoreApplication::translate(
                     "QuickPlayLauncher",
                     "Project+ Netplay Launcher could not be found or verified. Add the Launcher "
                     "folder to the game list paths, refresh the game list or repair your "
                     "Project+ installation.")};
  };
  const QString launcher_path = QString::fromStdString(File::GetUserPath(D_LAUNCHERS_IDX));
  if (launcher_path.isEmpty())
    return failure();

  const QString directory = QDir(launcher_path).canonicalPath();
  if (directory.isEmpty())
    return failure();

  // Official content first, then this build's Data/user name; the first file present wins,
  // matching the declaration in PersistentQuickPlaySession::Validate.
  for (const auto name : QuickPlayContent::NETPLAY_LAUNCHER_NAMES)
  {
    const QString filename = QString::fromLatin1(name);
    if (!QFileInfo(QDir(directory).filePath(filename)).isFile())
      continue;
    auto result = FindNamedLauncher(model, directory, filename);
    if (!result.game && result.error.isEmpty())
      return failure();
    return result;
  }
  return failure();
}
