// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "Common/CommonTypes.h"

struct GCPadStatus;

namespace Core
{
class System;
}

// Quick Play recipe mode. When DolphinQt boots the Project+ Offline Launcher, every peer
// boots under the same clock, private temporary NAND, forced settings and neutral input,
// stops at the first verified CSS origin, saves it to a private file and hands control to
// the player on the same Core. Peers then restore their own origin on join instead of a
// shared checkpoint. Portable-sync research recipe portable-css-ab/2 (STEP4-REPORT.md).
namespace QuickPlayRecipe
{
inline constexpr char RECIPE_ID[] = "portable-css-ab/2";
// Recipe clock origin, 2026-01-01T00:00:00Z. It must stay at least 71 s after 2000-01-01:
// guest system time is the time base plus the -2^32 bias that Wii HLE presets, and a
// negative system time stalls SI controller detection.
inline constexpr u32 EPOCH = 0x6955b900;

enum class Phase
{
  Off,
  Preparing,
  Ready,
  Failed,
};

struct Origin
{
  u32 pc = 0;
  u64 ticks = 0;
  // XXH3 over the MEM1/MEM2/CPU register digests, PC and ticks; equal on peers that can join.
  std::string digest;
  std::string path;
  u64 size = 0;
};

// DolphinQt only: later boots of the Offline Launcher may use recipe mode.
void SetFrontendSupported(bool supported);
// BootCore, after game metadata: activates for the Offline Launcher and forces the recipe's
// settings into the CurrentRun layer. Returns whether recipe mode is active for this boot.
bool ActivateForBoot(const std::string& game_id);
// After the Wii root has shut down.
void Deactivate();

bool IsActive();
Phase GetPhase();
bool IsReady();
// Valid once GetPhase() is Failed.
std::string FailureReason();
// Valid once GetPhase() is Ready.
Origin GetOrigin();

// CPU thread, Core paused. MEM1, MEM2 and CPU register digest; `detail` names the parts.
std::string ComputeDigest(Core::System& system, std::string* detail = nullptr);

// Emulation hooks. Each is a no-op that returns false unless recipe mode is active.
bool EmulatedTime(Core::System& system, u64* seconds);
bool HostMilliseconds(Core::System& system, u64* milliseconds);
bool MemcardFormatTime(u64* seconds);
bool FallbackSerial(std::string* serial);
bool SuppressCopyBack();
// Until the origin is captured, guest ports 1/2 read a connected neutral controller and no
// physical input is sampled.
bool NeutralPad(int port, GCPadStatus* pad);
void NoteSIRead(int channel, bool high, u32 value);
void NoteGuestIO();
void OnFieldBoundary(Core::System& system);
}  // namespace QuickPlayRecipe
