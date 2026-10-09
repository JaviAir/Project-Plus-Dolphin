// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <limits>
#include <span>

#include <gtest/gtest.h>

#include "Core/ProjectPlusOnline/Protocol.h"
#include "Core/ProjectPlusOnline/Status.h"

namespace
{
using namespace ProjectPlusOnline;
constexpr Packet HELLO{0x50, 0x50, 0x4f, 0x4e, 0, 1, 0, 1, 0, 0, 0, 1};
constexpr Packet START{0x50, 0x50, 0x4f, 0x4e, 0, 1, 0, 2, 0, 0, 0, 2};

TEST(ProjectPlusOnlineProtocol, WireLayoutAndHandshake)
{
  static_assert(PACKET_SIZE == 12);
  Session session;
  bool start_quickplay = true;
  const Packet hello_ack{0x50, 0x50, 0x4f, 0x4e, 0, 1, 0, 0, 0, 0, 0, 1};
  EXPECT_EQ(session.Accept(HELLO, false, start_quickplay), hello_ack);
  EXPECT_FALSE(start_quickplay);
  EXPECT_EQ(session.Accept(START, false, start_quickplay), Response(Result::Accepted, 2));
  EXPECT_TRUE(start_quickplay);
}

TEST(ProjectPlusOnlineProtocol, ExactSizeAndMagic)
{
  Session session;
  bool start_quickplay;
  std::array<std::uint8_t, 13> oversized{};
  std::copy(HELLO.begin(), HELLO.end(), oversized.begin());
  for (std::size_t length = 0; length <= oversized.size(); ++length)
  {
    if (length == PACKET_SIZE)
      continue;
    EXPECT_EQ(session.Accept(std::span(oversized).first(length), false, start_quickplay),
              Response(Result::BadPacket, 0));
    EXPECT_FALSE(start_quickplay);
  }
  for (std::size_t offset = 0; offset < 4; ++offset)
  {
    auto packet = HELLO;
    packet[offset] ^= 1;
    EXPECT_EQ(session.Accept(packet, false, start_quickplay), Response(Result::BadPacket, 0));
    EXPECT_FALSE(start_quickplay);
  }
  EXPECT_EQ(session.Accept(HELLO, false, start_quickplay), Response(Result::Accepted, 1));
}

TEST(ProjectPlusOnlineProtocol, VersionAndCommandAllowlist)
{
  Session session;
  bool start_quickplay;
  for (unsigned value = 0; value <= 0xffff; ++value)
  {
    auto packet = HELLO;
    packet[4] = value >> 8;
    packet[5] = value;
    if (value != VERSION)
    {
      EXPECT_EQ(session.Accept(packet, false, start_quickplay), Response(Result::BadVersion, 1));
      EXPECT_FALSE(start_quickplay);
    }
    packet = HELLO;
    packet[6] = value >> 8;
    packet[7] = value;
    if (value != 1 && value != 2 && value != 3 && value != 4)
    {
      EXPECT_EQ(session.Accept(packet, false, start_quickplay),
                Response(Result::UnknownCommand, 1));
      EXPECT_FALSE(start_quickplay);
    }
  }
  EXPECT_EQ(session.Accept(HELLO, false, start_quickplay), Response(Result::Accepted, 1));
}

TEST(ProjectPlusOnlineProtocol, HelloRequired)
{
  Session session;
  bool start_quickplay;
  EXPECT_EQ(session.Accept(START, false, start_quickplay), Response(Result::HelloRequired, 2));
  EXPECT_FALSE(start_quickplay);
  EXPECT_EQ(session.Accept(HELLO, false, start_quickplay), Response(Result::Accepted, 1));
  EXPECT_EQ(session.Accept(START, false, start_quickplay), Response(Result::Accepted, 2));
}

TEST(ProjectPlusOnlineProtocol, RejectsDuplicatesAndOlderSequences)
{
  Session session;
  bool start_quickplay;
  session.Accept(HELLO, false, start_quickplay);
  session.Accept(START, false, start_quickplay);
  EXPECT_EQ(session.Accept(START, false, start_quickplay), Response(Result::StaleSequence, 2));
  EXPECT_FALSE(start_quickplay);
  EXPECT_EQ(session.Accept(HELLO, false, start_quickplay), Response(Result::StaleSequence, 1));
  EXPECT_FALSE(start_quickplay);
  auto next = START;
  next[11] = 3;
  EXPECT_EQ(session.Accept(next, false, start_quickplay), Response(Result::Accepted, 3));
  EXPECT_TRUE(start_quickplay);
}

TEST(ProjectPlusOnlineProtocol, OnePendingJobBackpressure)
{
  Session session;
  bool start_quickplay;
  session.Accept(HELLO, false, start_quickplay);
  for (unsigned i = 0; i < 1000; ++i)
  {
    EXPECT_EQ(session.Accept(START, true, start_quickplay), Response(Result::Busy, 2));
    EXPECT_FALSE(start_quickplay);
  }
  EXPECT_EQ(session.Accept(START, false, start_quickplay), Response(Result::Accepted, 2));
  EXPECT_TRUE(start_quickplay);
}

TEST(ProjectPlusOnlineProtocol, ReentryHelloDoesNotResetSequence)
{
  Session session;
  bool start_quickplay;
  session.Accept(HELLO, false, start_quickplay);
  session.Accept(START, false, start_quickplay);
  auto reentry = HELLO;
  reentry[11] = 3;
  EXPECT_EQ(session.Accept(reentry, false, start_quickplay), Response(Result::Accepted, 3));
  EXPECT_FALSE(start_quickplay);
  EXPECT_EQ(session.Accept(START, false, start_quickplay), Response(Result::StaleSequence, 2));
}

TEST(ProjectPlusOnlineProtocol, SequenceZeroAndWrapFailClosed)
{
  Session session;
  bool start_quickplay;
  auto packet = HELLO;
  packet[11] = 0;
  EXPECT_EQ(session.Accept(packet, false, start_quickplay), Response(Result::StaleSequence, 0));
  std::fill(packet.begin() + 8, packet.end(), 0xff);
  EXPECT_EQ(session.Accept(packet, false, start_quickplay),
            Response(Result::Accepted, std::numeric_limits<std::uint32_t>::max()));
  EXPECT_EQ(session.Accept(START, false, start_quickplay), Response(Result::StaleSequence, 2));
  EXPECT_FALSE(start_quickplay);
}
}  // namespace

TEST(ProjectPlusOnlineStatus, ReadDoesNotStartOrWaitForStartDispatch)
{
  Session session;
  bool start = true;
  auto request = START;
  request[7] = 3;
  EXPECT_EQ(session.Accept(request, false, start), Response(Result::HelloRequired, 2));
  EXPECT_FALSE(start);
  session.Accept(HELLO, false, start);
  const auto header = session.Accept(request, true, start);
  EXPECT_EQ(header, Response(Result::Accepted, 2));
  EXPECT_FALSE(start);
  const auto response = StatusResponse(header, {GuestStatus::Connected, 0x12345678, Reason::None});
  EXPECT_EQ(response.size(), 24u);
  EXPECT_EQ(response[13], 1);
  EXPECT_EQ(response[15], 4);
  EXPECT_EQ(Read32(response, 16), 0x12345678u);
  EXPECT_EQ(Read32(response, 20), 0u);
  EXPECT_EQ(session.Accept(request, false, start), Response(Result::StaleSequence, 2));
  EXPECT_FALSE(start);
}

TEST(ProjectPlusOnlineStatus, CoherentEpochAndBoundedFailure)
{
  StatusMailbox mailbox;
  const auto initial = mailbox.Read();
  mailbox.Publish(GuestStatus::Searching);
  const auto searching = mailbox.Read();
  EXPECT_EQ(searching.epoch, initial.epoch + 1);
  mailbox.Publish(GuestStatus::Searching);
  EXPECT_EQ(mailbox.Read().epoch, searching.epoch);
  mailbox.Publish(GuestStatus::Error, Reason::AttemptFailed);
  EXPECT_EQ(mailbox.Read().epoch, searching.epoch + 1);
  EXPECT_EQ(mailbox.Read().reason, Reason::AttemptFailed);
  const auto rejected = StatusResponse(Response(Result::BadVersion, 4), mailbox.Read());
  EXPECT_EQ(Read32(rejected, 8), 4u);
  EXPECT_TRUE(std::all_of(rejected.begin() + 12, rejected.end(), [](auto b) { return b == 0; }));
}

TEST(ProjectPlusOnlineCancel, CommandHandshakeBusyAndDuplicate)
{
  Session session;
  auto cancel = START;
  cancel[7] = 4;
  bool start = true;
  EXPECT_EQ(session.Accept(cancel, false, start), Response(Result::HelloRequired, 2));
  session.Accept(HELLO, false, start);
  EXPECT_EQ(session.Accept(cancel, true, start), Response(Result::Busy, 2));
  EXPECT_EQ(session.Accept(cancel, false, start), Response(Result::Accepted, 2));
  EXPECT_FALSE(start);
  EXPECT_EQ(session.Accept(cancel, false, start), Response(Result::StaleSequence, 2));
  cancel[11] = 3;
  EXPECT_EQ(session.Accept(cancel, false, start), Response(Result::Accepted, 3));
  EXPECT_FALSE(start);
}

TEST(ProjectPlusOnlineCancel, ObservedAttemptCannotCancelLaterAttempt)
{
  CancelObservation observation;
  EXPECT_EQ(observation.Target(7), 0u);
  observation.Observe(7, {GuestStatus::Searching, 2, Reason::None}, 7);
  EXPECT_EQ(observation.Target(7), 7u);
  // Assignment and connecting retain the same attempt; invalidation does not.
  EXPECT_EQ(observation.Target(0), 0u);
  EXPECT_EQ(observation.Target(9), 0u);
  observation.Observe(9, {GuestStatus::Connecting, 5, Reason::None}, 9);
  EXPECT_EQ(observation.Target(9), 9u);
  observation.Reset();
  EXPECT_EQ(observation.Target(9), 0u);
}

TEST(ProjectPlusOnlineCancel, PublicationRacesAndNonCancellableOwnershipFailClosed)
{
  CancelObservation observation;
  for (const auto status :
       {GuestStatus::Idle, GuestStatus::Connected, GuestStatus::Error, GuestStatus::Unavailable})
  {
    observation.Observe(7, {status, 3, Reason::None}, 7);
    EXPECT_EQ(observation.Target(7), 0u);
  }
  observation.Observe(7, {GuestStatus::Connecting, 3, Reason::Lifecycle}, 7);
  EXPECT_EQ(observation.Target(7), 0u);
  observation.Observe(7, {GuestStatus::Searching, 3, Reason::None}, 9);
  EXPECT_EQ(observation.Target(9), 0u);
  observation.Observe(0, {GuestStatus::Searching, 3, Reason::None}, 7);
  EXPECT_EQ(observation.Target(7), 0u);
}
