// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/QuickPlayRecipe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/Wiimote.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/ProjectPlusOnline/LocalInput.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/VideoConfig.h"

namespace QuickPlayRecipe
{
namespace
{
// Guest binding for Project+ v3.2 (RSBE01 rev 1 plus the official SD's codes). Module
// addresses come from the OS module list.
constexpr u32 MODULE_HEAD = 0x800030c8;
constexpr u32 SCENE_PTR = 0x805b8ba4;
constexpr u32 SCENE_MANAGER_PTR = 0x805a0060;  // r13-0x43c0, returned by 8002d018
constexpr u32 CSS_MODULE_ID = 10;
constexpr u32 CSS_TEXT_SECTION = 1;
constexpr u32 CSS_UPDATE_OFFSET = 0x4e10;
constexpr u32 CSS_UPDATE_WORD = 0x9421ffe0;
constexpr u32 SI_POLL_SHADOW = 0x80474e9c;  // SDK Si.poll (SIEnablePolling / SISetXY)
constexpr u32 SI_TYPE = 0x80474eb0;         // SDK Type[4] (SIGetType cache)
constexpr u32 SI_TYPE_GC_STANDARD = 0x09000000;
constexpr u32 SI_ERROR_NO_RESPONSE = 0x08;
// The origin is the third consecutive CSS update entry where css-init-v1 holds.
constexpr int OCCURRENCE = 3;
// The verified origin is at 5.7 s emulated; give up well after it.
constexpr u64 PREPARE_LIMIT_SECONDS = 20;
// After ports 2-4 are released, keep the image covered until the CSS drops the port 2 hand.
// A configured, connected second controller keeps its hand; show the CSS after ~1.5 s anyway.
constexpr u64 CONCEAL_LIMIT_FIELDS = 90;
// The presented frame can trail guest memory; keep covering a few fields after the hand is gone.
constexpr u64 CONCEAL_SETTLE_FIELDS = 3;
constexpr char WIRELESS_MAC[] = "00:17:ab:0a:5a:79";
constexpr char FALLBACK_SERIAL[] = "123456789";
constexpr u32 MEM2_PHYSICAL = 0x10000000;

struct RecipeState
{
  std::atomic<bool> frontend{false};
  std::atomic<bool> active{false};
  std::atomic<Phase> phase{Phase::Off};
  // Read by the video thread: cover the game image (see ConcealsGame).
  std::atomic<bool> conceal{false};
  std::atomic<bool> snapshot_queued{false};
  std::atomic<bool> local_ready{false};
  std::function<void()> local_snapshot_handler;  // set once by the frontend
  std::mutex lock;  // origin, failure, gpu_writes
  Origin origin;
  std::string failure;
  // Physical [begin, end) ranges written by EFB/XFB copies while preparing.
  std::vector<std::pair<u32, u32>> gpu_writes;
  std::chrono::steady_clock::time_point boot_time;

  // CPU thread only.
  u64 fields = 0;
  bool css_bound = false;
  u32 entry = 0;
  u64 entries = 0;
  u32 qual_task = 0;
  int qual_count = 0;
  u64 io_total = 0;
  u64 io_total_at_entry = 0;
  bool nonneutral_seen = false;
  u32 css_task = 0;
  u64 conceal_fields = 0;
  u64 released_fields = 0;
  bool local_pending = false;
};

RecipeState s;

bool Valid(u32 p)
{
  return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000);
}

double Since(std::chrono::steady_clock::time_point t)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

std::string SceneName(Memory::MemoryManager& mem)
{
  const u32 scene = mem.Read_U32(SCENE_PTR);
  if (!Valid(scene))
    return {};
  const u32 name = mem.Read_U32(scene);
  if (!Valid(name))
    return {};
  std::string out;
  for (u32 i = 0; i < 24; ++i)
  {
    const char c = static_cast<char>(mem.Read_U8(name + i));
    if (c == 0)
      break;
    out.push_back(c);
  }
  return out;
}

void ClearHook(Core::System& system)
{
  if (!s.css_bound)
    return;
  system.GetPowerPC().GetBreakPoints().ClearCaptureHook();
  s.css_bound = false;
}

void NotifyHost(bool resume)
{
  Core::QueueHostJob([resume](Core::System& system) {
    // Preparation runs muted at unlimited speed; give the player their own settings back.
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_EMULATION_SPEED);
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_AUDIO_MUTED);
    AudioCommon::UpdateSoundStream(system);
    if (resume)
      Core::SetState(system, Core::State::Running);
  });
}

// CPU thread.
void Fail(Core::System& system, const std::string& reason, bool resume)
{
  if (s.phase != Phase::Preparing)
    return;
  ClearHook(system);
  {
    std::lock_guard guard(s.lock);
    s.failure = reason;
  }
  s.phase = Phase::Failed;
  s.conceal = false;
  ERROR_LOG_FMT(NETPLAY,
                "Quick Play recipe: preparation failed after {:.2f} s ({} CSS updates, ticks {}): {}",
                Since(s.boot_time), s.entries, system.GetCoreTiming().GetTicks(), reason);
  NotifyHost(resume);
}

bool OnCaptureHook(Core::System& system);

void BindCSS(Core::System& system)
{
  auto& mem = system.GetMemory();
  u32 module = mem.Read_U32(MODULE_HEAD);
  u32 css_text = 0;
  for (int guard = 0; Valid(module) && guard < 64; ++guard)
  {
    if (mem.Read_U32(module) == CSS_MODULE_ID)
    {
      const u32 sections = mem.Read_U32(module + 0x10);
      if (Valid(sections) && mem.Read_U32(module + 0x0c) > CSS_TEXT_SECTION)
        css_text = mem.Read_U32(sections + CSS_TEXT_SECTION * 8) & ~1u;
    }
    module = mem.Read_U32(module + 4);
  }
  if (!css_text && s.css_bound)
  {
    ClearHook(system);
    return;
  }
  if (!css_text || s.css_bound)
    return;
  const u32 entry = css_text + CSS_UPDATE_OFFSET;
  if (mem.Read_U32(entry) != CSS_UPDATE_WORD)
  {
    Fail(system,
         fmt::format("the character select code is not the supported Project+ v3.2 build "
                     "(entry {:08x} = {:08x})",
                     entry, mem.Read_U32(entry)),
         false);
    return;
  }
  s.entry = entry;
  s.css_bound = true;
  system.GetPowerPC().GetBreakPoints().SetCaptureHook(
      entry, [&system] { return OnCaptureHook(system); });
}

bool PendingIOEvents(Core::System& system, std::string* which)
{
  const std::string summary = system.GetCoreTiming().GetScheduledEventsSummary();
  bool any = false;
  std::istringstream lines(summary);
  std::string line;
  while (std::getline(lines, line))
  {
    const auto colon = line.find(" : ");
    if (colon == std::string::npos)
      continue;
    const std::string name = line.substr(0, colon);
    const u64 userdata = std::strtoull(line.substr(line.rfind(' ') + 1).c_str(), nullptr, 16);
    // DVDInterface reply type is the high 32 bits of the userdata; DTK (3) is Dolphin's idle
    // disc-audio streaming timer, rescheduled indefinitely, not a guest-issued drive command.
    const bool dtk_timer = name == "FinishExecutingCommand" && (userdata >> 32) == 3;
    if ((name == "IPCEvent" || name == "FinishReadDVDThread" || name == "FinishDICommand" ||
         name == "FinishExecutingCommand") &&
        !dtk_timer)
    {
      any = true;
      *which += name + ";";
    }
  }
  return any;
}

// Predicate css-init-v1: the published CSS task is settled, both standard controllers are
// detected with their areas active, ports 3/4 are absent, no input other than neutral was
// consumed and no guest I/O is in flight or newly issued.
bool EvaluatePredicate(Core::System& system, u32 task, std::string* why)
{
  auto& mem = system.GetMemory();
  const auto fail = [why](std::string reason) {
    *why = std::move(reason);
    return false;
  };
  const u32 manager = mem.Read_U32(SCENE_MANAGER_PTR);
  if (!Valid(manager))
    return fail("manager");
  const u32 scene = mem.Read_U32(manager + 4);
  if (scene != mem.Read_U32(SCENE_PTR) || !Valid(scene) || mem.Read_U32(scene + 0x400) != task)
    return fail("not-published-task");
  if (mem.Read_U32(manager + 0x284) != 0 || mem.Read_U32(manager + 0x288) != 1)
    return fail("transition");
  if (mem.Read_U32(task + 0x40) != 0 || mem.Read_U32(task + 0x5f8) != 0 ||
      mem.Read_U32(task + 0x630) != 0)
    return fail("task-state");
  if (static_cast<s32>(mem.Read_U32(task + 0x64c)) >= 0)
    return fail("entry-countdown");
  if (mem.Read_U32(task + 0x648) != 4)
    return fail("area-count");
  const u32 poll = mem.Read_U32(SI_POLL_SHADOW);
  for (u32 ch = 0; ch < 4; ++ch)
  {
    const u32 type = mem.Read_U32(SI_TYPE + 4 * ch);
    const bool connected = ch < 2;  // Standard controllers on SI0/SI1, none on SI2/SI3.
    if (connected && (type != SI_TYPE_GC_STANDARD || !(poll & (0x80u >> ch))))
      return fail(fmt::format("si-detect ch{}", ch));
    if (!connected && (type != SI_ERROR_NO_RESPONSE || (poll & (0x80u >> ch))))
      return fail(fmt::format("si-absent ch{}", ch));
    const u32 area = mem.Read_U32(task + 0x44 + 4 * ch);
    if (!Valid(area))
      return fail(fmt::format("area{}", ch));
    const u32 cursor = mem.Read_U32(area + 0x1a8);
    if (!Valid(cursor) || mem.Read_U32(cursor + 0xa4) != 0)
      return fail(fmt::format("cursor{}", ch));
    const u32 port = mem.Read_U32(area + 0x1dc), state = mem.Read_U32(area + 0x1e0);
    if (connected && (port != ch || state != 0))
      return fail(fmt::format("active-area{}", ch));
    if (!connected && (port != 0xffffffff || state != 4))
      return fail(fmt::format("inactive-area{}", ch));
  }
  if (s.nonneutral_seen)
    return fail("non-neutral-input-consumed");
  std::string pending;
  if (PendingIOEvents(system, &pending))
    return fail("io-pending " + pending);
  if (s.io_total != s.io_total_at_entry)
    return fail("io-issued-this-interval");
  *why = "ok";
  return true;
}

std::string GeneratedDirectory()
{
  return File::GetUserPath(D_USER_IDX) + "QuickPlay" DIR_SEP "generated" DIR_SEP;
}

// CPU thread, stopped at the origin before the CSS update entry executes.
void Capture(Core::System& system)
{
  if (s.phase != Phase::Preparing)
    return;
  const auto start = std::chrono::steady_clock::now();
  Origin origin;
  origin.pc = system.GetPPCState().pc;
  origin.ticks = system.GetCoreTiming().GetTicks();
  std::string detail;
  origin.digest = ComputeDigest(system, &detail);
  const double digest_seconds = Since(start);
  const std::string directory = GeneratedDirectory();
  origin.path = directory + "origin.sav";
  if (!File::CreateFullPath(directory) || !State::SaveForPersistentSession(system, origin.path))
  {
    Fail(system, "the generated CSS origin could not be saved to " + origin.path, true);
    return;
  }
  origin.size = File::GetSize(origin.path);
  const std::string manifest = fmt::format(
      "{{\n  \"schema\": 1,\n  \"recipe\": \"{}\",\n  \"build\": \"{}\",\n  \"pc\": \"{:08x}\",\n"
      "  \"ticks\": {},\n  \"digest\": \"{}\",\n  \"size\": {}\n}}\n",
      RECIPE_ID, Common::GetScmRevGitStr(), origin.pc, origin.ticks, origin.digest, origin.size);
  // The manifest describes the file for diagnostics; this build never loads it as a cache.
  File::WriteStringToFile(directory + "origin.json", manifest);
  ClearHook(system);
  {
    std::lock_guard guard(s.lock);
    s.origin = origin;
  }
  s.css_task = s.qual_task;
  s.local_pending = true;
  s.phase = Phase::Ready;
  ReleaseExtraPorts(system);
  NOTICE_LOG_FMT(NETPLAY,
                 "Quick Play recipe {}: origin digest {} ({}) at CSS update {}; prepared in "
                 "{:.2f} s wall, {:.3f} s emulated; digest {:.3f} s, save {:.3f} s, {} bytes",
                 RECIPE_ID, origin.digest, detail, s.entries, Since(s.boot_time),
                 double(origin.ticks) / system.GetSystemTimers().GetTicksPerSecond(),
                 digest_seconds, Since(start) - digest_seconds, origin.size);
  NotifyHost(true);
}

bool OnCaptureHook(Core::System& system)
{
  if (s.phase != Phase::Preparing)
    return false;
  auto& mem = system.GetMemory();
  const u32 task = system.GetPPCState().gpr[3];
  ++s.entries;
  std::string why = "scene";
  const bool ok =
      SceneName(mem) == "scSelctCharacter" && EvaluatePredicate(system, task, &why);
  s.io_total_at_entry = s.io_total;
  if (ok && task == s.qual_task && s.qual_count > 0)
    ++s.qual_count;
  else
    s.qual_count = ok ? 1 : 0;
  s.qual_task = ok ? task : 0;
  DEBUG_LOG_FMT(NETPLAY, "Quick Play recipe: CSS update {} qualifies {} ({}) count {} ticks {}",
                s.entries, ok, why, s.qual_count, system.GetCoreTiming().GetTicks());
  if (s.qual_count < OCCURRENCE)
    return false;
  // Break before the entry executes; capture runs as a CPU job once the JIT has exited.
  system.GetCPU().AddCPUThreadJob([&system] { Capture(system); });
  return true;
}

void ForceSettings()
{
  using Config::SetCurrent;
  // Guest-visible or compared by the NetPlay contract: identical on every peer.
  SetCurrent(Config::MAIN_CPU_THREAD, false);
  SetCurrent(Config::MAIN_CPU_CORE, PowerPC::DefaultCPUCore());
  SetCurrent(Config::MAIN_DSP_HLE, true);
  SetCurrent(Config::MAIN_SYNC_ON_SKIP_IDLE, true);
  SetCurrent(Config::MAIN_SYNC_GPU, false);
  SetCurrent(Config::MAIN_MMU, false);
  SetCurrent(Config::MAIN_FASTMEM, true);
  SetCurrent(Config::MAIN_FPRF, false);
  SetCurrent(Config::MAIN_ACCURATE_NANS, false);
  SetCurrent(Config::MAIN_ACCURATE_FMADDS, true);
  SetCurrent(Config::MAIN_DISABLE_ICACHE, false);
  SetCurrent(Config::MAIN_FLOAT_EXCEPTIONS, false);
  SetCurrent(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS, false);
  SetCurrent(Config::MAIN_JIT_FOLLOW_BRANCH, true);
  SetCurrent(Config::MAIN_ACCURATE_CPU_CACHE, false);
  SetCurrent(Config::MAIN_OVERCLOCK_ENABLE, false);
  SetCurrent(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
  SetCurrent(Config::MAIN_RAM_OVERRIDE_ENABLE, false);
  SetCurrent(Config::MAIN_FAST_DISC_SPEED, false);
  SetCurrent(Config::MAIN_SLOT_A, ExpansionInterface::EXIDeviceType::None);
  SetCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::None);
  SetCurrent(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::None);
  // Port 1 keeps a Standard Controller or a GameCube Adapter for Wii U setting; the guest
  // sees a standard controller either way (SI init uses ContractSIDevice).
  const auto port1 = Config::Get(Config::GetInfoForSIDevice(0));
  if (!ProjectPlusOnline::SupportsLocalCommands(port1))
    SetCurrent(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);
  SetCurrent(Config::GetInfoForSIDevice(1), SerialInterface::SIDEVICE_GC_CONTROLLER);
  SetCurrent(Config::GetInfoForSIDevice(2), SerialInterface::SIDEVICE_NONE);
  SetCurrent(Config::GetInfoForSIDevice(3), SerialInterface::SIDEVICE_NONE);
  for (int i = 0; i < MAX_BBMOTES; ++i)
    SetCurrent(Config::GetInfoForWiimoteSource(i), WiimoteSource::None);
  SetCurrent(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, false);
  SetCurrent(Config::MAIN_USB_PASSTHROUGH_DEVICES, std::string{});
  SetCurrent(Config::MAIN_WII_KEYBOARD, false);
  SetCurrent(Config::MAIN_EMULATE_SKYLANDER_PORTAL, false);
  SetCurrent(Config::MAIN_EMULATE_INFINITY_BASE, false);
  SetCurrent(Config::MAIN_EMULATE_WII_SPEAK, false);
  SetCurrent(Config::MAIN_WII_SD_CARD, true);
  SetCurrent(Config::MAIN_ALLOW_SD_WRITES, false);
  SetCurrent(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC, false);
  SetCurrent(Config::MAIN_ENABLE_CHEATS, true);
  SetCurrent(Config::MAIN_GC_LANGUAGE, 0);
  SetCurrent(Config::MAIN_OVERRIDE_REGION_SETTINGS, false);
  SetCurrent(Config::MAIN_CUSTOM_RTC_ENABLE, true);
  SetCurrent(Config::MAIN_CUSTOM_RTC_VALUE, EPOCH);
  SetCurrent(Config::MAIN_ENABLE_SAVESTATES, true);
  SetCurrent(Config::MAIN_WIRELESS_MAC, std::string(WIRELESS_MAC));

  // Resolution, AA, filtering and the other settings ordinary NetPlay only syncs with Strict
  // Settings Sync stay the player's own: they only change pixels, and the digest leaves the
  // memory EFB/XFB copies write out.
  SetCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
  SetCurrent(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, false);
  SetCurrent(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, 128);
  SetCurrent(Config::GFX_BACKEND_MULTITHREADING, false);
  SetCurrent(Config::GFX_HIRES_TEXTURES, false);
  SetCurrent(Config::GFX_MODS_ENABLE, false);
  SetCurrent(Config::GFX_HACK_VI_SKIP, false);
  SetCurrent(Config::GFX_HACK_EFB_ACCESS_ENABLE, false);
  SetCurrent(Config::GFX_HACK_BBOX_ENABLE, false);
  SetCurrent(Config::GFX_HACK_FORCE_PROGRESSIVE, true);
  SetCurrent(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, false);
  SetCurrent(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, false);
  SetCurrent(Config::GFX_HACK_DISABLE_COPY_TO_VRAM, false);
  SetCurrent(Config::GFX_HACK_IMMEDIATE_XFB, true);
  SetCurrent(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, false);
  SetCurrent(Config::GFX_HACK_DEFER_EFB_COPIES, true);
  SetCurrent(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE, 64);
  SetCurrent(Config::GFX_HACK_EFB_DEFER_INVALIDATION, false);
  SetCurrent(Config::GFX_HACK_SKIP_DUPLICATE_XFBS, false);
  SetCurrent(Config::GFX_PERF_QUERIES_ENABLE, false);
  // Rendering inputs that stay at their defaults.
  SetCurrent(Config::GFX_HACK_FAST_TEXTURE_SAMPLING,
             Config::GFX_HACK_FAST_TEXTURE_SAMPLING.GetDefaultValue());
  SetCurrent(Config::GFX_HACK_MISSING_COLOR_VALUE,
             Config::GFX_HACK_MISSING_COLOR_VALUE.GetDefaultValue());

  // Persistent sessions need these NetPlay options off (fresh profiles enable save sync).
  SetCurrent(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  SetCurrent(Config::NETPLAY_SAVEDATA_LOAD, false);
  SetCurrent(Config::NETPLAY_SYNC_CODES, false);
  SetCurrent(Config::NETPLAY_RECORD_INPUTS, false);
  SetCurrent(Config::NETPLAY_SPECTATOR_MODE, false);

  // Preparation only (host-side): run muted at unlimited speed until the origin.
  SetCurrent(Config::MAIN_EMULATION_SPEED, 0.0f);
  SetCurrent(Config::MAIN_AUDIO_MUTED, true);
}
}  // namespace

// CPU thread (or under a CPU thread guard): the CSS shows a port 2 hand.
bool SecondPortHandShown(Core::System& system)
{
  auto& mem = system.GetMemory();
  const u32 area = Valid(s.css_task) ? mem.Read_U32(s.css_task + 0x44 + 4) : 0;
  return Valid(area) &&
         !(mem.Read_U32(area + 0x1dc) == 0xffffffff && mem.Read_U32(area + 0x1e0) == 4);
}

void ReleaseExtraPorts(Core::System& system)
{
  auto& si = system.GetSerialInterface();
  bool changed = false;
  for (int port = 1; port < 4; ++port)
  {
    const auto device = Config::GetBase(Config::GetInfoForSIDevice(port));
    changed |= si.GetDeviceType(port) != device;
    si.ChangeDevice(device, port);
  }
  s.conceal_fields = 0;
  s.released_fields = 0;
  // Cover only a hand that is on screen and about to go: the origin's port 2 controller. The
  // local snapshot has no port 2 hand (its port may still be switching to the configured device).
  s.conceal = changed && SecondPortHandShown(system);
}

void SetLocalSnapshotHandler(std::function<void()> handler)
{
  s.local_snapshot_handler = std::move(handler);
}

std::string LocalSnapshotPath()
{
  return GeneratedDirectory() + "local.sav";
}

bool SaveLocalSnapshot(Core::System& system)
{
  const auto start = std::chrono::steady_clock::now();
  s.local_ready = s.phase == Phase::Ready && State::SaveForPersistentSession(system, LocalSnapshotPath());
  INFO_LOG_FMT(NETPLAY, "Quick Play recipe: local CSS snapshot saved {} in {:.3f} s",
               s.local_ready.load(), Since(start));
  return s.local_ready;
}

void FinishLocalSnapshot()
{
  s.snapshot_queued = false;
  s.conceal = false;
}

bool HasLocalSnapshot()
{
  return s.local_ready && File::Exists(LocalSnapshotPath());
}

bool ConcealsGame()
{
  return s.conceal;
}

void SetFrontendSupported(bool supported)
{
  s.frontend = supported;
}

bool ActivateForBoot(const std::string& game_id)
{
  Deactivate();
  if (!s.frontend || !Config::Get(Config::NETPLAY_QUICKPLAY_SEAMLESS) ||
      (game_id != "ID-Project+ Offline Launcher" && game_id != "ID-Offline Launcher"))
    return false;
  ForceSettings();
  s.fields = 0;
  s.css_bound = false;
  s.entry = 0;
  s.entries = 0;
  s.qual_task = 0;
  s.qual_count = 0;
  s.io_total = 0;
  s.io_total_at_entry = 0;
  s.nonneutral_seen = false;
  s.css_task = 0;
  s.conceal_fields = 0;
  s.local_pending = false;
  s.local_ready = false;
  s.snapshot_queued = false;
  File::Delete(LocalSnapshotPath());
  {
    std::lock_guard guard(s.lock);
    s.origin = {};
    s.failure.clear();
    s.gpu_writes.clear();
  }
  s.boot_time = std::chrono::steady_clock::now();
  s.phase = Phase::Preparing;
  s.conceal = true;
  s.active = true;
  NOTICE_LOG_FMT(NETPLAY, "Quick Play recipe {}: preparing from {} (epoch {:#x}, build {})",
                 RECIPE_ID, game_id, EPOCH, Common::GetScmRevGitStr());
  return true;
}

void Deactivate()
{
  // A stop during preparation would otherwise leave the hook armed for the next boot.
  Core::System::GetInstance().GetPowerPC().GetBreakPoints().ClearCaptureHook();
  s.css_bound = false;
  s.conceal = false;
  s.local_ready = false;
  s.active = false;
  s.phase = Phase::Off;
}

bool IsActive()
{
  return s.active;
}

Phase GetPhase()
{
  return s.phase;
}

bool IsReady()
{
  return s.phase == Phase::Ready;
}

std::string FailureReason()
{
  std::lock_guard guard(s.lock);
  return s.failure;
}

Origin GetOrigin()
{
  std::lock_guard guard(s.lock);
  return s.origin;
}

std::string ComputeDigest(Core::System& system, std::string* detail)
{
  std::vector<std::pair<u32, u32>> gpu;
  {
    std::lock_guard guard(s.lock);
    gpu = s.gpu_writes;
  }
  std::ranges::sort(gpu);
  std::vector<std::pair<u32, u32>> merged;
  for (const auto& range : gpu)
  {
    if (!merged.empty() && range.first <= merged.back().second)
      merged.back().second = std::max(merged.back().second, range.second);
    else
      merged.push_back(range);
  }
  u64 gpu_bytes = 0;
  for (const auto& range : merged)
    gpu_bytes += range.second - range.first;
  const u64 gpu_ranges = merged.empty() ? 0 : XXH3_64bits(merged.data(), merged.size() * sizeof(merged[0]));
  // Hash a memory region without the copied ranges that fall inside it.
  const auto hash = [&merged](const u8* data, u32 base, u32 size) {
    XXH3_state_t* region = XXH3_createState();
    XXH3_64bits_reset(region);
    u32 at = base;
    for (const auto& [begin, end] : merged)
    {
      const u32 b = std::clamp(begin, base, base + size), e = std::clamp(end, base, base + size);
      if (b >= e)
        continue;
      XXH3_64bits_update(region, data + (at - base), b - at);
      at = e;
    }
    XXH3_64bits_update(region, data + (at - base), base + size - at);
    const u64 digest = XXH3_64bits_digest(region);
    XXH3_freeState(region);
    return digest;
  };
  auto& mem = system.GetMemory();
  const u64 mem1 = hash(mem.GetRAM(), 0, mem.GetRamSizeReal());
  const u64 mem2 = mem.GetEXRAM() ? hash(mem.GetEXRAM(), MEM2_PHYSICAL, mem.GetExRamSizeReal()) : 0;
  const auto& ppc = system.GetPPCState();
  XXH3_state_t* state = XXH3_createState();
  XXH3_64bits_reset(state);
  XXH3_64bits_update(state, &ppc.gpr, sizeof(ppc.gpr));
  XXH3_64bits_update(state, &ppc.ps, sizeof(ppc.ps));
  XXH3_64bits_update(state, &ppc.spr, sizeof(ppc.spr));
  XXH3_64bits_update(state, &ppc.sr, sizeof(ppc.sr));
  const u32 cr = ppc.cr.Get(), msr = ppc.msr.Hex, fpscr = ppc.fpscr.Hex;
  XXH3_64bits_update(state, &cr, sizeof(cr));
  XXH3_64bits_update(state, &msr, sizeof(msr));
  XXH3_64bits_update(state, &fpscr, sizeof(fpscr));
  XXH3_64bits_update(state, &ppc.pc, sizeof(ppc.pc));
  const u64 regs = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  const std::array<u64, 6> parts{mem1, mem2, regs, ppc.pc, system.GetCoreTiming().GetTicks(),
                                 gpu_ranges};
  if (detail)
  {
    *detail = fmt::format("mem1 {:016x} mem2 {:016x} regs {:016x} pc {:08x} ticks {} gpu copies "
                          "{:016x} ({} ranges, {} bytes not digested)",
                          mem1, mem2, regs, ppc.pc, parts[4], gpu_ranges, merged.size(),
                          gpu_bytes);
  }
  return fmt::format("{:016x}", XXH3_64bits(parts.data(), sizeof(parts)));
}

bool EmulatedTime(Core::System& system, u64* seconds)
{
  if (!s.active)
    return false;
  *seconds = EPOCH + system.GetCoreTiming().GetTicks() / system.GetSystemTimers().GetTicksPerSecond();
  return true;
}

bool HostMilliseconds(Core::System& system, u64* milliseconds)
{
  if (!s.active)
    return false;
  const u64 ticks = system.GetCoreTiming().GetTicks();
  *milliseconds = u64{EPOCH} * 1000 + ticks * 1000 / system.GetSystemTimers().GetTicksPerSecond();
  return true;
}

bool MemcardFormatTime(u64* seconds)
{
  if (!s.active)
    return false;
  *seconds = 0;
  return true;
}

bool FallbackSerial(std::string* serial)
{
  if (!s.active)
    return false;
  *serial = FALLBACK_SERIAL;
  return true;
}

bool SuppressCopyBack()
{
  return s.active;
}

bool NeutralPad(int port, GCPadStatus* pad)
{
  if (!s.active || s.phase != Phase::Preparing)
    return false;
  *pad = {};
  pad->isConnected = port < 2;
  pad->stickX = GCPadStatus::MAIN_STICK_CENTER_X;
  pad->stickY = GCPadStatus::MAIN_STICK_CENTER_Y;
  pad->substickX = GCPadStatus::C_STICK_CENTER_X;
  pad->substickY = GCPadStatus::C_STICK_CENTER_Y;
  return true;
}

void NoteSIRead(int channel, bool high, u32 value)
{
  if (s.phase == Phase::Preparing && channel < 2 &&
      value != (high ? 0x00808080u : 0x80800000u))
    s.nonneutral_seen = true;
}

void NoteGPUWrite(u32 address, u32 bytes_per_row, u32 rows, u32 stride)
{
  if (s.phase != Phase::Preparing || !rows || !bytes_per_row)
    return;
  address &= 0x1fffffff;
  std::lock_guard guard(s.lock);
  if (stride == bytes_per_row)
  {
    s.gpu_writes.emplace_back(address, address + rows * stride);
    return;
  }
  for (u32 row = 0; row < rows; ++row)
    s.gpu_writes.emplace_back(address + row * stride, address + row * stride + bytes_per_row);
}

void NoteGuestIO()
{
  if (s.phase == Phase::Preparing)
    ++s.io_total;
}

// CPU thread: hand the due local snapshot to the frontend (host thread). The cover, if any,
// stays up until the frontend has saved it.
void QueueLocalSnapshot()
{
  s.local_pending = false;
  if (!s.local_snapshot_handler)
  {
    s.conceal = false;
    return;
  }
  s.snapshot_queued = true;
  Core::QueueHostJob([](Core::System&) { s.local_snapshot_handler(); });
}

void OnFieldBoundary(Core::System& system)
{
  if (s.snapshot_queued)
    return;
  if (!s.conceal && s.local_pending && s.phase == Phase::Ready)
  {
    QueueLocalSnapshot();
    return;
  }
  if (s.conceal && s.phase == Phase::Ready)
  {
    // Port 2's CSS area goes inactive (as css-init-v1 checks for ports 3/4) once the
    // released port stops answering.
    const bool released = !SecondPortHandShown(system);
    s.released_fields = released ? s.released_fields + 1 : 0;
    if (s.released_fields > CONCEAL_SETTLE_FIELDS || ++s.conceal_fields > CONCEAL_LIMIT_FIELDS)
    {
      INFO_LOG_FMT(NETPLAY, "Quick Play recipe: CSS shown after {} fields (port 2 released {})",
                   s.conceal_fields, released);
      if (s.local_pending)
        QueueLocalSnapshot();
      else
        s.conceal = false;
    }
    return;
  }
  if (s.phase != Phase::Preparing)
    return;
  ++s.fields;
  const u64 ticks = system.GetCoreTiming().GetTicks();
  if (ticks > PREPARE_LIMIT_SECONDS * system.GetSystemTimers().GetTicksPerSecond())
  {
    Fail(system,
         fmt::format("the character select screen did not reach the shared start within {} s "
                     "of emulated time",
                     PREPARE_LIMIT_SECONDS),
         false);
    return;
  }
  if (s.fields % 4 == 0 || !s.css_bound)
    BindCSS(system);
}
}  // namespace QuickPlayRecipe
