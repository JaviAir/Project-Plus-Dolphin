// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace ProjectPlusOnline
{
enum class GuestStatus : std::uint16_t
{
  Idle,
  Searching,
  Matched,
  Connecting,
  Connected,
  Error,
  Unavailable
};
// Host-only chord adapter for a configured persistent CSS profile. Epochs invalidate
// queued requests on restore/retirement; no state is serialized into guest RAM.
inline std::atomic<bool> persistent_local_input{false};
inline std::atomic<std::uint64_t> persistent_input_epoch{1};
// Host-only attempt identity. Never serialized or exposed to the guest. The GUI
// clears this before a non-cancellable state and sets a new identity only after it.
inline std::atomic<std::uint64_t> guest_cancel_attempt{0};
constexpr bool IsCancellable(GuestStatus status)
{
  return status == GuestStatus::Searching || status == GuestStatus::Matched ||
         status == GuestStatus::Connecting;
}
}  // namespace ProjectPlusOnline
