// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

#include "Core/SyncIdentifier.h"

namespace QuickPlayContent
{
enum class Policy
{
  // Also hashes the inserted disc against the configured default ISO.
  Strict,
  // No content scans: the disc is identified by its header only (the default).
  DeclaredBundleTrialV1,
};
constexpr Policy SelectedPolicy()
{
#ifdef QUICKPLAY_NO_BULK_SCAN_TRIAL
  return Policy::DeclaredBundleTrialV1;
#else
  return Policy::Strict;
#endif
}
// Peers ready together only with equal build, recipe and effective configuration. Each peer
// generates its own origin; its digest is compared separately in the ready handshake.
// In-memory data only. Explicit appends: DolphinQt builds with QT_USE_QSTRINGBUILDER.
// The hashed text: policy, build and recipe lines, then the "name=value" configuration lines.
// Peers also exchange it so a refusal can name what differs.
inline QByteArray ContractText(Policy policy, const QString& build, const QString& recipe,
                               const QString& config)
{
  QByteArray bytes(policy == Policy::DeclaredBundleTrialV1 ? "quickplay-recipe-contract-v1 no-scan\n" :
                                                             "quickplay-recipe-contract-v1 strict\n");
  bytes.append(build.toUtf8())
      .append('\n')
      .append(recipe.toUtf8())
      .append('\n')
      .append(config.toUtf8());
  return bytes;
}
inline std::string ContractIdentity(Policy policy, const QString& build, const QString& recipe,
                                    const QString& config)
{
  return QCryptographicHash::hash(ContractText(policy, build, recipe, config),
                                  QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}
// Official Project+ v3.2 Netplay content names the launcher "Project+ Netplay Launcher.dol";
// this repository's Data/user copy is "Netplay Launcher.dol". The first existing name wins.
inline constexpr std::array NETPLAY_LAUNCHER_NAMES{"Project+ Netplay Launcher.dol",
                                                   "Netplay Launcher.dol"};
// A copied GUI-owned declaration, consumed on the network thread without file IO.
struct TrialLauncher
{
  std::string path;
  NetPlay::SyncIdentifier identity;
};
inline TrialLauncher DeclaredLauncher(const QString& path, qint64 size, const std::string& game_id,
                                      const QString& build, const QString& recipe)
{
  QByteArray declaration("quickplay-declared-launcher-v2\n");
  declaration.append(build.toUtf8()).append('\n').append(recipe.toUtf8());
  const QByteArray digest = QCryptographicHash::hash(declaration, QCryptographicHash::Sha1);
  NetPlay::SyncIdentifier identity{static_cast<u64>(size), game_id, 0, 0, false, {}};
  std::copy(digest.begin(), digest.end(), identity.sync_hash.begin());
  return {path.toStdString(), std::move(identity)};
}
inline bool DiscHeadersMatch(const QByteArray& inserted, const QByteArray& expected)
{
  // Identification only: raw Wii disc header, game ID, disc number and revision.
  // No apploader, DOL, FST, banner or arbitrary-size partition reads.
  return inserted.size() == 0x20 && inserted == expected &&
         expected.startsWith("RSBE01") &&
         expected.mid(0x18, 4) == QByteArray::fromHex("5d1c9ea3");
}
}  // namespace QuickPlayContent
