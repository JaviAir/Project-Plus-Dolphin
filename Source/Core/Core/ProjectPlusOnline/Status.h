// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <limits>

#include "Core/ProjectPlusOnline/Protocol.h"

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
// Host-only attempt identity. Never serialized or exposed to the guest. The GUI
// clears this before publishing a non-cancellable state and publishes a new
// identity only after its status. Readers bracket status with identity loads.
inline std::atomic<std::uint64_t> guest_cancel_attempt{0};
constexpr bool IsCancellable(GuestStatus status)
{
  return status == GuestStatus::Searching || status == GuestStatus::Matched ||
         status == GuestStatus::Connecting;
}

// An endpoint can cancel only an attempt observed in a consumed status reply.
// HELLO drops this observation; sequence high-water marks still cannot rewind.
class CancelObservation
{
public:
  void Observe(std::uint64_t before, Snapshot status, std::uint64_t after)
  {
    m_attempt = before == after && IsCancellable(status.status) && status.reason == Reason::None ?
                    before :
                    0;
  }
  void Reset() { m_attempt = 0; }
  std::uint64_t Target(std::uint64_t current) const { return m_attempt == current ? m_attempt : 0; }

private:
  std::uint64_t m_attempt = 0;
};
constexpr std::size_t STATUS_PACKET_SIZE = 24;
using StatusPacket = std::array<std::uint8_t, STATUS_PACKET_SIZE>;
inline StatusPacket StatusResponse(const Packet& header, Snapshot snapshot)
{
  StatusPacket response{};
  std::copy(header.begin(), header.end(), response.begin());
  if (header[6] != 0 || header[7] != static_cast<std::uint8_t>(Result::Accepted))
    return response;
  response[13] = 1;  // GuestStatus payload schema, independent of envelope version.
  response[15] = static_cast<std::uint8_t>(snapshot.status);
  for (unsigned i = 0; i != 4; ++i)
    response[16 + i] = static_cast<std::uint8_t>(snapshot.epoch >> (24 - 8 * i));
  response[21] = static_cast<std::uint8_t>(snapshot.reason);
  return response;
}
}  // namespace ProjectPlusOnline
