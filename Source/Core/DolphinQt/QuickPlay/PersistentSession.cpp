// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "DolphinQt/QuickPlay/PersistentSession.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <array>
#include <map>
#include <tuple>
#include "DolphinQt/QtUtils/QueueOnObject.h"

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Movie.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/ProjectPlusOnline/LocalInput.h"
#include "Core/ProjectPlusOnline/Presentation.h"
#include "Core/State.h"
#include "Core/System.h"
#include "UICommon/GameFile.h"

namespace
{
QString BundlePath()
{
  return QString::fromStdString(File::GetUserPath(D_USER_IDX)) + QStringLiteral("QuickPlay/");
}
QByteArray HashFile(const QString& path)
{
  // Validation happens before queue admission. Rehash changed files, but retain
  // verified digests for unchanged immutable content across match/leave/rejoin.
  // Include metadata-change time so replacing a file or preserving its mtime
  // cannot reuse a receipt from a different file.
  using Receipt = std::tuple<qint64, QDateTime, QDateTime, QByteArray>;
  static std::map<QString, Receipt> receipts;
  const QFileInfo info(path);
  if (!info.isFile())
    return {};
  const auto modified = info.lastModified();
  const auto changed = info.fileTime(QFile::FileMetadataChangeTime);
  const auto it = receipts.find(path);
  if (it != receipts.end() && std::get<0>(it->second) == info.size() &&
      std::get<1>(it->second) == modified && std::get<2>(it->second) == changed)
    return std::get<3>(it->second);
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
    return {};
  const auto result = hash.result().toHex();
  receipts[path] = {info.size(), modified, changed, result};
  return result;
}
QByteArray HashTree(const QString& root, bool nand = false)
{
  if (!QDir(root).exists())
    return {};
  QStringList paths;
  QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (it.hasNext())
  {
    const auto relative = QDir(root).relativeFilePath(it.next());
    // IOS's host-wall-clock download scheduler rewrites retry times even with
    // the guest paused. fst.bin is the host filesystem index rebuilt by the
    // ordinary state loader when restoring /tmp. Neither is immutable external
    // content. /tmp is serialized and restored by the pinned checkpoint; local
    // cache history must not become an external prerequisite for that restore.
    // Keep all external title data and SYSCONF.
    if (!nand || (relative != QStringLiteral("shared2/wc24/nwc24dl.bin") &&
                  relative != QStringLiteral("fst.bin") &&
                  !relative.startsWith(QStringLiteral("tmp/"))))
      paths.push_back(relative);
  }
  paths.sort();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  for (const auto& path : paths)
  {
    const auto digest = HashFile(QDir(root).filePath(path));
    if (digest.isEmpty())
      return {};
    hash.addData((path + QStringLiteral("\n")).toUtf8());
    hash.addData(digest);
    hash.addData(QByteArray("\n"));
  }
  return hash.result().toHex();
}
}  // namespace

bool PersistentQuickPlaySession::IsConfigured()
{
  return QFile::exists(BundlePath() + QStringLiteral("common-css.json"));
}

bool PersistentQuickPlaySession::Validate(Core::System& system, QString* error)
{
  const auto fail = [&](const QString& message) {
    ERROR_LOG_FMT(NETPLAY, "Persistent QuickPlay: admission rejected: {}", message.toStdString());
    *error = message;
    return false;
  };
  if (!Core::IsRunning(system) || system.IsDualCoreMode() || system.GetMovie().IsMovieActive() ||
      !Config::Get(Config::MAIN_DSP_HLE) || Config::Get(Config::MAIN_ALLOW_SD_WRITES) ||
      Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED) ||
      Config::Get(Config::NETPLAY_NETWORK_MODE) != "fixeddelay" ||
      Config::Get(Config::NETPLAY_SAVEDATA_LOAD) || Config::Get(Config::NETPLAY_SYNC_CODES) ||
      Config::Get(Config::NETPLAY_RECORD_INPUTS) || Config::Get(Config::NETPLAY_SPECTATOR_MODE) ||
      SConfig::GetInstance().GetGameID() != "RSBE01")
    return fail(QStringLiteral("Persistent Quick Play requires the validated single-core, "
                               "GC-only, read-only SD profile and fixed-delay NetPlay."));
  for (const auto slot : ExpansionInterface::SLOTS)
    if (Config::Get(Config::GetInfoForEXIDevice(slot)) != ExpansionInterface::EXIDeviceType::None)
      return fail(QStringLiteral("The common CSS requires empty EXI peripheral slots."));
  // Local port 1 may be a Standard Controller or a GameCube Adapter for Wii U port;
  // either way the guest keeps the checkpoint's standard controllers on ports 1/2.
  std::array<SerialInterface::SIDevices, 4> devices;
  for (int port = 0; port < 4; ++port)
    devices[port] = Config::Get(Config::GetInfoForSIDevice(port));
  if (!ProjectPlusOnline::AdmitsPersistentSIDevices(devices))
    return fail(QStringLiteral("Quick Play needs port 1 set to Standard Controller or GameCube "
                               "Adapter for Wii U, port 2 set to Standard Controller and "
                               "ports 3/4 empty."));
  {
    const Core::CPUThreadGuard guard(system);
    if (IsTrial())
    {
      const auto inserted = system.GetDVDThread().GetInsertedDiscHeader();
      QFile expected(QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO)));
      if (!inserted || !expected.open(QIODevice::ReadOnly) ||
          !QuickPlayContent::DiscHeadersMatch(
              QByteArray(reinterpret_cast<const char*>(inserted->data()), inserted->size()),
              expected.read(0x20)))
        return fail(QStringLiteral("The inserted disc does not match the supported default Wii ISO."));
    }
    else
    {
      const auto inserted = system.GetDVDThread().GetInsertedDiscSyncHash();
      const UICommon::GameFile expected(Config::Get(Config::MAIN_DEFAULT_ISO));
      if (!inserted || !expected.IsValid() || *inserted != expected.GetSyncIdentifier().sync_hash)
        return fail(QStringLiteral("The inserted disc does not match the validated default ISO."));
    }
  }
  QJsonObject json;
  if (!QuickPlayContent::ReadManifest(BundlePath() + QStringLiteral("common-css.json"), &json))
    return fail(QStringLiteral("Install a readable, supported QuickPlay/common-css bundle first."));
  const auto build = QString::fromStdString(Common::GetScmRevGitStr() + "-persistent1");
  if (!QuickPlayContent::CompatibleManifest(
          json, m_policy, build, QString::fromStdString(ConfigLoaders::RunningNetPlayConfigIdentity())))
    return fail(QStringLiteral("The CSS bundle build or effective configuration is incompatible."));
  m_checkpoint = BundlePath() + QStringLiteral("common-css.sav");
  const auto launchers = QString::fromStdString(File::GetUserPath(D_LAUNCHERS_IDX));
  const QuickPlayContent::Paths paths{
      {QStringLiteral("checkpoint"), m_checkpoint},
      {QStringLiteral("offline"), launchers + QStringLiteral("Project+ Offline Launcher.dol")},
      {QStringLiteral("netplay"), launchers + QStringLiteral("Netplay Launcher.dol")},
      {QStringLiteral("iso"), QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO))},
      {QStringLiteral("sd"), QString::fromStdString(Config::Get(Config::MAIN_WII_SD_CARD_IMAGE_PATH))},
      {QStringLiteral("game_settings"), QString::fromStdString(File::GetUserPath(D_GAMESETTINGS_IDX))},
      {QStringLiteral("system_settings"), QString::fromStdString(File::GetSysDirectory()) +
                                             QStringLiteral("GameSettings")},
      {QStringLiteral("nand"), QString::fromStdString(File::GetUserPath(D_WIIROOT_IDX))}};
  const auto failed_key = QuickPlayContent::CheckContent(
      json, paths, m_policy, [](const QString& key, const QString& path) {
        if (key == QStringLiteral("nand"))
          return HashTree(path, true);
        if (key == QStringLiteral("game_settings") || key == QStringLiteral("system_settings"))
          return HashTree(path);
        return HashFile(path);
      });
  if (!failed_key.isEmpty())
    return fail(QStringLiteral("The CSS bundle file/storage check failed: ") + failed_key);
  m_identity = QuickPlayContent::ContractIdentity(json, m_policy, build);
  if (IsTrial())
  {
    const auto path = paths[QStringLiteral("netplay")];
    m_trial_launcher = QuickPlayContent::DeclaredLauncher(path, QFileInfo(path).size(), json, build);
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: declared bundle accepted; content verification skipped; "
                         "policy quickplay-declared-bundle-trial-v1 build {} contract {}",
                 build.toStdString(), m_identity);
  }
  else
  {
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: validated contract {}", m_identity);
  }
  return true;
}

void PersistentQuickPlaySession::Restore(Core::System& system, std::function<void(bool)> finished)
{
  Core::SetState(system, Core::State::Paused);
  const auto path = m_checkpoint.toStdString();
  const bool valid_file =
      QuickPlayContent::CheckpointMayLoad(m_policy, [this] {
        return HashFile(m_checkpoint) == QuickPlayContent::CHECKPOINT_SHA;
      });
  if (IsTrial())
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: restore content verification skipped; ordinary loader retained");
  // Texture/device restoration must execute on the real CPU/GPU thread. A GUI
  // CPUThreadGuard alone does not make the OpenGL context current on that thread.
  Core::RunOnCPUThread(system, [&system, path, valid_file, finished = std::move(finished)] {
    const bool loaded = valid_file && State::LoadForPersistentSession(system, path);
    bool ok = loaded;
    ok = ok && system.GetPPCState().pc == 0x806876d4 &&
         system.GetCoreTiming().GetTicks() == 77875265854;
    auto& si = system.GetSerialInterface();
    for (int port = 0; ok && port < 4; ++port)
      ok = si.GetDeviceType(port) ==
           (port < 2 ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE);
    if (!ok)
      ERROR_LOG_FMT(NETPLAY,
                    "Persistent QuickPlay: restore rejected loaded {} file {} ticks {} PC {:08x} "
                    "devices {},{},{},{}",
                    loaded, valid_file, system.GetCoreTiming().GetTicks(), system.GetPPCState().pc,
                    int(si.GetDeviceType(0)), int(si.GetDeviceType(1)), int(si.GetDeviceType(2)),
                    int(si.GetDeviceType(3)));
    // The live devices came from the checkpoint. Pin the desired types to them
    // so the next SI poll cannot swap in a locally configured Wii U adapter
    // device; that port remains an input source of the standard controller.
    for (int port = 0; ok && port < 4; ++port)
      si.ChangeDevice(si.GetDeviceType(port), port);
    if (ok)
      INFO_LOG_FMT(NETPLAY,
                   "Persistent QuickPlay: clean CSS restored on CPU/GPU thread; Core retained "
                   "ticks {} PC {:08x}",
                   system.GetCoreTiming().GetTicks(), system.GetPPCState().pc);
    const auto core = ProjectPlusOnline::presentation_core_generation.load();
    const auto input = ProjectPlusOnline::persistent_input_epoch.load();
    QueueOnObject(QApplication::instance(), [finished, ok, core, input] {
      if (ProjectPlusOnline::presentation_core_generation.load() != core ||
          ProjectPlusOnline::persistent_input_epoch.load() != input)
        return;
      finished(ok);
    });
  });
}
