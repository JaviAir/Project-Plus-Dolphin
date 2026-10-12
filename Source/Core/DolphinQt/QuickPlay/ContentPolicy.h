// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <utility>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QString>

#include "Core/SyncIdentifier.h"

namespace QuickPlayContent
{
enum class Policy
{
  Strict,
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
inline constexpr auto CHECKPOINT_SHA =
    "5f39d05f1059b67d9c9796e3709ec0337dfa1740099d97342026105292a25ce9";
// Only this archived bundle is an additional compatible source for this isolated
// trial. The state loader still enforces serializer compatibility. Never rewrite
// the receipt to pretend the trial was certified by the archived build.
inline constexpr auto ARCHIVED_BUILD = "c5df493c211fde31791de47085df09be5bf9144b-persistent1";
inline constexpr qint64 MAX_MANIFEST_BYTES = 64 * 1024;
inline constexpr std::array CONTENT_KEYS{"checkpoint", "offline", "netplay", "iso", "sd",
                                         "nand", "game_settings", "system_settings"};
inline bool IsDigest(const QString& value)
{
  if (value.size() != 64)
    return false;
  for (const QChar ch : value)
  {
    const char16_t c = ch.unicode();
    if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
      return false;
  }
  return true;
}
inline bool ReadManifest(const QString& path, QJsonObject* json)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > MAX_MANIFEST_BYTES)
    return false;
  const auto bytes = file.read(MAX_MANIFEST_BYTES + 1);
  const auto document = QJsonDocument::fromJson(bytes);
  if (bytes.size() > MAX_MANIFEST_BYTES || !document.isObject())
    return false;
  *json = document.object();
  return true;
}
inline bool CompatibleManifest(const QJsonObject& json, Policy policy, const QString& build,
                               const QString& config)
{
  const auto declared_build = json[QStringLiteral("build")].toString();
  if (json[QStringLiteral("schema")].toDouble() != 1 ||
      json[QStringLiteral("checkpoint")].toString() != QString::fromLatin1(CHECKPOINT_SHA) ||
      (declared_build != build &&
       !(policy == Policy::DeclaredBundleTrialV1 && declared_build == QString::fromLatin1(ARCHIVED_BUILD))) ||
      json[QStringLiteral("config")].toString() != config || config.isEmpty())
    return false;
  for (const auto key : CONTENT_KEYS)
    if (!IsDigest(json[QString::fromLatin1(key)].toString()))
      return false;
  return true;
}
inline std::string ContractIdentity(const QJsonObject& json, Policy policy, const QString& build)
{
  auto bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
  // Preserve strict's existing identity, while domain-separating the trial from
  // both legacy strict peers and future trial versions. Hash in-memory data only.
  // Explicit appends: DolphinQt builds with QT_USE_QSTRINGBUILDER.
  if (policy == Policy::DeclaredBundleTrialV1)
  {
    QByteArray prefix("quickplay-declared-bundle-trial-v1\n");
    prefix.append(build.toUtf8()).append('\n');
    bytes.prepend(prefix);
  }
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}
using Paths = QMap<QString, QString>;
using Hasher = std::function<QByteArray(const QString& key, const QString& path)>;
inline QString CheckContent(const QJsonObject& json, const Paths& paths, Policy policy,
                            const Hasher& hash)
{
  for (auto it = paths.begin(); it != paths.end(); ++it)
  {
    const QFileInfo info(it.value());
    const bool tree = it.key() == QStringLiteral("nand") ||
                      it.key() == QStringLiteral("game_settings") ||
                      it.key() == QStringLiteral("system_settings");
    if (!(tree ? info.isDir() && info.isReadable() : info.isFile() && info.isReadable()))
      return it.key();
    if (!tree)
    {
      QFile file(it.value());
      if (!file.open(QIODevice::ReadOnly))
        return it.key();  // Open/close only; no content read in the trial.
    }
    if (policy == Policy::Strict &&
        QString::fromLatin1(hash(it.key(), it.value())) != json[it.key()].toString())
      return it.key();
  }
  return {};
}
template <typename Verify>
bool CheckpointMayLoad(Policy policy, Verify&& verify)
{
  return policy == Policy::DeclaredBundleTrialV1 || std::forward<Verify>(verify)();
}
// A copied GUI-owned declaration, consumed on the network thread without file IO.
struct TrialLauncher
{
  std::string path;
  NetPlay::SyncIdentifier identity;
};
inline TrialLauncher DeclaredLauncher(const QString& path, qint64 size, const QJsonObject& json,
                                      const QString& build)
{
  QByteArray declaration("quickplay-declared-launcher-v1\n");
  declaration.append(build.toUtf8())
      .append('\n')
      .append(json[QStringLiteral("netplay")].toString().toUtf8());
  const QByteArray digest = QCryptographicHash::hash(declaration, QCryptographicHash::Sha1);
  NetPlay::SyncIdentifier identity{static_cast<u64>(size), "ID-Netplay Launcher", 0, 0, false, {}};
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
