// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ProjectPlusOnline
{
// Wire format, not a native struct: all integers are big endian.
constexpr std::uint32_t MAGIC = 0x50504f4e;  // PPON, also the prototype EXI identity.
constexpr std::uint16_t VERSION = 1;
constexpr std::size_t PACKET_SIZE = 12;
using Packet = std::array<std::uint8_t, PACKET_SIZE>;
enum class Command : std::uint16_t
{
  Hello = 1,
  StartQuickPlay = 2,
  GetStatus = 3,
  CancelQuickPlay = 4,
};
enum class Result : std::uint16_t
{
  Accepted = 0,
  BadPacket = 1,
  BadVersion = 2,
  UnknownCommand = 3,
  StaleSequence = 4,
  HelloRequired = 5,
  Busy = 6,
};
constexpr std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
  return (std::uint32_t{bytes[offset]} << 24) | (std::uint32_t{bytes[offset + 1]} << 16) |
         (std::uint32_t{bytes[offset + 2]} << 8) | bytes[offset + 3];
}
constexpr Packet Response(Result result, std::uint32_t sequence)
{
  return {0x50,
          0x50,
          0x4f,
          0x4e,
          0,
          1,
          0,
          static_cast<std::uint8_t>(result),
          static_cast<std::uint8_t>(sequence >> 24),
          static_cast<std::uint8_t>(sequence >> 16),
          static_cast<std::uint8_t>(sequence >> 8),
          static_cast<std::uint8_t>(sequence)};
}

// Emulation-thread owned. Never serialized: sequence high-water mark cannot rewind.
// No wrapping within a device lifetime. A new boot is needed after UINT32_MAX.
class Session
{
public:
  Packet Accept(std::span<const std::uint8_t> bytes, bool busy, bool& start_quickplay)
  {
    start_quickplay = false;
    if (bytes.size() != PACKET_SIZE || Read32(bytes, 0) != MAGIC)
      return Response(Result::BadPacket, 0);
    const auto sequence = Read32(bytes, 8);
    if (bytes[4] != 0 || bytes[5] != VERSION)
      return Response(Result::BadVersion, sequence);
    if (bytes[6] != 0 || (bytes[7] != static_cast<std::uint8_t>(Command::Hello) &&
                          bytes[7] != static_cast<std::uint8_t>(Command::StartQuickPlay) &&
                          bytes[7] != static_cast<std::uint8_t>(Command::GetStatus) &&
                          bytes[7] != static_cast<std::uint8_t>(Command::CancelQuickPlay)))
      return Response(Result::UnknownCommand, sequence);
    if (sequence == 0 || sequence <= m_last_sequence)
      return Response(Result::StaleSequence, sequence);
    const bool hello = bytes[7] == static_cast<std::uint8_t>(Command::Hello);
    if (!hello && !m_hello)
      return Response(Result::HelloRequired, sequence);
    if (busy && bytes[7] != static_cast<std::uint8_t>(Command::GetStatus))
      return Response(Result::Busy, sequence);
    m_last_sequence = sequence;
    m_hello = true;
    start_quickplay = bytes[7] == static_cast<std::uint8_t>(Command::StartQuickPlay);
    return Response(Result::Accepted, sequence);
  }

private:
  std::uint32_t m_last_sequence = 0;
  bool m_hello = false;
};
}  // namespace ProjectPlusOnline
