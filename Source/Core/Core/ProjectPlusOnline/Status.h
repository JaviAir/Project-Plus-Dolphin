// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

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
enum class Reason : std::uint16_t
{
  None,
  AttemptFailed,
  Lifecycle
};
struct Snapshot
{
  GuestStatus status;
  std::uint32_t epoch;
  Reason reason;
};

// Single GUI-thread publisher, CPU-thread reader. One atomic load gives a coherent
// snapshot. No Qt, network objects, identifiers or pointers cross this boundary.
class StatusMailbox
{
public:
  void Publish(GuestStatus status, Reason reason = Reason::None)
  {
    const auto old = Read();
    if (old.status == status && old.reason == reason)
      return;
    if (old.epoch == std::numeric_limits<std::uint32_t>::max())
    {
      m_value.store(Pack(GuestStatus::Unavailable, old.epoch, Reason::Lifecycle));
      return;
    }
    m_value.store(Pack(status, old.epoch + 1, reason));
  }
  Snapshot Read() const
  {
    const auto value = m_value.load();
    return {static_cast<GuestStatus>(value & 0xffff), static_cast<std::uint32_t>(value >> 32),
            static_cast<Reason>((value >> 16) & 0xffff)};
  }

private:
  static constexpr std::uint64_t Pack(GuestStatus status, std::uint32_t epoch, Reason reason)
  {
    return (std::uint64_t{epoch} << 32) | (static_cast<std::uint64_t>(reason) << 16) |
           static_cast<std::uint16_t>(status);
  }
  std::atomic<std::uint64_t> m_value{Pack(GuestStatus::Unavailable, 1, Reason::Lifecycle)};
};
inline StatusMailbox guest_status;
// Host-only chord adapter for a configured persistent CSS profile. Epochs invalidate
// queued requests on restore/retirement; no state is serialized into guest RAM.
inline std::atomic<bool> persistent_local_input{false};
inline std::atomic<std::uint64_t> persistent_input_epoch{1};
// Host-only attempt identity. Never serialized or exposed to the guest. The GUI
// clears this before publishing a non-cancellable state and publishes a new
// identity only after its status. Readers bracket status with identity loads.
inline std::atomic<std::uint64_t> guest_cancel_attempt{0};
constexpr bool IsCancellable(GuestStatus status)
{
  return status == GuestStatus::Searching || status == GuestStatus::Matched ||
         status == GuestStatus::Connecting;
}
}  // namespace ProjectPlusOnline
