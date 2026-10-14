// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "DolphinQt/QuickPlay/PersistentSession.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <array>
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
#include "Core/QuickPlayRecipe.h"
#include "Core/State.h"
#include "Core/System.h"
#include "UICommon/GameFile.h"

namespace
{
QString NotReadyMessage()
{
  switch (QuickPlayRecipe::GetPhase())
  {
  case QuickPlayRecipe::Phase::Preparing:
    return QStringLiteral("Quick Play is still preparing. Wait for the character select screen.");
  case QuickPlayRecipe::Phase::Failed:
    return QStringLiteral("Quick Play could not prepare a shared start: ") +
           QString::fromStdString(QuickPlayRecipe::FailureReason());
  default:
    return QStringLiteral("In-game Quick Play needs the Project+ Offline Launcher started with "
                          "seamless Quick Play enabled (Quick Play settings).");
  }
}
}  // namespace

bool PersistentQuickPlaySession::IsConfigured()
{
  return QuickPlayRecipe::IsReady();
}

bool PersistentQuickPlaySession::Validate(Core::System& system, QString* error)
{
  const auto fail = [&](const QString& message) {
    ERROR_LOG_FMT(NETPLAY, "Persistent QuickPlay: admission rejected: {}", message.toStdString());
    *error = message;
    return false;
  };
  if (!QuickPlayRecipe::IsReady())
    return fail(NotReadyMessage());
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
  // either way the guest keeps the origin's standard controllers on ports 1/2.
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
  m_origin = QuickPlayRecipe::GetOrigin();
  if (QFileInfo(QString::fromStdString(m_origin.path)).size() != qint64(m_origin.size))
    return fail(QStringLiteral("The generated CSS origin file is missing or was changed. "
                               "Restart the Offline Launcher."));
  const auto build = QString::fromStdString(Common::GetScmRevGitStr());
  const auto recipe = QString::fromLatin1(QuickPlayRecipe::RECIPE_ID);
  const auto config = QString::fromStdString(ConfigLoaders::RunningNetPlayConfigIdentity());
  m_identity = QuickPlayContent::ContractIdentity(m_policy, build, recipe, config);
  m_contract_text =
      QuickPlayContent::ContractText(m_policy, build, recipe, config).toStdString();
  if (IsTrial())
  {
    const QDir launchers(QString::fromStdString(File::GetUserPath(D_LAUNCHERS_IDX)));
    QFileInfo launcher;
    for (const auto name : QuickPlayContent::NETPLAY_LAUNCHER_NAMES)
    {
      launcher = QFileInfo(launchers.filePath(QString::fromLatin1(name)));
      if (launcher.isFile())
        break;
    }
    if (!launcher.isFile() || !launcher.isReadable())
      return fail(QStringLiteral("The Project+ Netplay Launcher is missing from the Launcher "
                                 "folder."));
    // A DOL's game ID is "ID-" plus its file name without extension, as in the game list.
    m_trial_launcher = QuickPlayContent::DeclaredLauncher(
        launcher.filePath(), launcher.size(), "ID-" + launcher.completeBaseName().toStdString(),
        build, recipe);
  }
  INFO_LOG_FMT(NETPLAY,
               "Persistent QuickPlay: admitted {} policy, build {} recipe {} contract {} origin {}",
               IsTrial() ? "no-scan" : "strict", build.toStdString(), recipe.toStdString(),
               m_identity, m_origin.digest);
  // Compare these lines between peers when their contract identities differ.
  for (const auto& line : config.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: config {}", line.toStdString());
  return true;
}

void PersistentQuickPlaySession::RestoreLocal(Core::System& system,
                                              std::function<void(bool)> finished)
{
  Core::SetState(system, Core::State::Paused);
  Core::RunOnCPUThread(system, [&system, finished = std::move(finished)] {
    const bool ok = QuickPlayRecipe::HasLocalSnapshot() &&
                    State::LoadForPersistentSession(system, QuickPlayRecipe::LocalSnapshotPath());
    // As in Restore: keep port 1's standard controller even when a Wii U adapter port drives
    // it. Ports 2-4 are the configured devices already; ReleaseExtraPorts reapplies them.
    if (ok)
    {
      auto& si = system.GetSerialInterface();
      si.ChangeDevice(si.GetDeviceType(0), 0);
    }
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: local CSS snapshot restored {}; ticks {} PC {:08x}",
                 ok, system.GetCoreTiming().GetTicks(), system.GetPPCState().pc);
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

void PersistentQuickPlaySession::Restore(Core::System& system, std::function<void(bool)> finished)
{
  Core::SetState(system, Core::State::Paused);
  // Texture/device restoration must execute on the real CPU/GPU thread. A GUI
  // CPUThreadGuard alone does not make the graphics context current on that thread.
  Core::RunOnCPUThread(system, [&system, origin = m_origin, finished = std::move(finished)] {
    const bool file = File::GetSize(origin.path) == origin.size;
    const bool loaded = file && State::LoadForPersistentSession(system, origin.path);
    // The loaded state must be the origin this Core generated and announced to its peer.
    std::string detail;
    const auto digest = loaded ? QuickPlayRecipe::ComputeDigest(system, &detail) : std::string{};
    bool ok = loaded && system.GetPPCState().pc == origin.pc &&
              system.GetCoreTiming().GetTicks() == origin.ticks && digest == origin.digest;
    auto& si = system.GetSerialInterface();
    for (int port = 0; ok && port < 4; ++port)
      ok = si.GetDeviceType(port) ==
           (port < 2 ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE);
    if (!ok)
      ERROR_LOG_FMT(NETPLAY,
                    "Persistent QuickPlay: restore rejected loaded {} file {} ticks {} PC {:08x} "
                    "digest {} expected {} ({}) devices {},{},{},{}",
                    loaded, file, system.GetCoreTiming().GetTicks(), system.GetPPCState().pc,
                    digest, origin.digest, detail, int(si.GetDeviceType(0)),
                    int(si.GetDeviceType(1)), int(si.GetDeviceType(2)), int(si.GetDeviceType(3)));
    // Pin the desired types to the origin's live devices, so the next SI poll cannot swap in a
    // configured Wii U adapter; that port stays an input source of the standard controller.
    for (int port = 0; ok && port < 4; ++port)
      si.ChangeDevice(si.GetDeviceType(port), port);
    if (ok)
      INFO_LOG_FMT(NETPLAY,
                   "Persistent QuickPlay: own CSS origin {} restored on CPU/GPU thread; Core "
                   "retained ticks {} PC {:08x}",
                   digest, system.GetCoreTiming().GetTicks(), system.GetPPCState().pc);
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
