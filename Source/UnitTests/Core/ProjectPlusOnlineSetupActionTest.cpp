// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/ProjectPlusOnline/Chord.h"
#include "Core/ProjectPlusOnline/LobbyPresentation.h"
#include "Core/ProjectPlusOnline/SetupAction.h"

namespace
{
using namespace ProjectPlusOnline;
using namespace std::chrono_literals;
const auto now = SetupActionMailbox::Clock::time_point{10s};
const PresentationIdentity identity{1, 2, 3, 4, 0};

TEST(ProjectPlusOnlineSetup, HiddenRoomInitializesOnceWithoutRetiringItsOwner)
{
  LobbyPresentation room;
  room.Begin(EntrySource::InGame, true, 3);
  EXPECT_FALSE(room.AutomaticShow());
  EXPECT_TRUE(room.Initialize());
  EXPECT_TRUE(room.Initialized());
  EXPECT_FALSE(room.Initialize());     // Revealing diagnostics cannot initialize another room.
  EXPECT_FALSE(room.AutomaticShow());  // Releasing setup leaves retained provenance intact.
  EXPECT_TRUE(room.Initialized());
  room.Reset();
  EXPECT_FALSE(room.Initialized());
  room.Begin(EntrySource::Desktop, true, 4);
  EXPECT_TRUE(room.Initialize());
  EXPECT_TRUE(room.AutomaticShow());
  room.Begin(EntrySource::InGame, false, 5);
  EXPECT_TRUE(room.AutomaticShow());  // Unsupported flow keeps its visible fallback.
  room.Begin(EntrySource::InGame, true, 0);
  EXPECT_TRUE(room.AutomaticShow());
}

TEST(ProjectPlusOnlineSetup, ConstructorCauseSurvivesRoomCleanupAndDiagnosticReveal)
{
  LobbyPresentation room;
  room.Begin(EntrySource::InGame, true, 3);
  room.SetError("The server and client's NetPlay versions are incompatible.");
  const auto full = room.DiagnosticError();
  EXPECT_TRUE(room.InGame());  // Established before constructor callbacks.
  room.Reset();
  EXPECT_EQ(room.DiagnosticError(), full);
  EXPECT_FALSE(room.Initialized());
  room.SetError(full + " The local CSS could not be restored.");
  EXPECT_EQ(room.DiagnosticError(), full + " The local CSS could not be restored.");
  room.Begin(EntrySource::Desktop, false, 0);
  EXPECT_TRUE(room.DiagnosticError().empty());  // A deliberate new room owns a new error lifetime.
}

TEST(ProjectPlusOnlineSetup, QueuedFailureIsCapturedBeforeCleanupAndCannotCrossRooms)
{
  SessionFailureMailbox callbacks;
  callbacks.Begin(4);
  const auto real = callbacks.Capture(4, "Traversal server rejected the host lookup.");
  EXPECT_EQ(callbacks.Capture(4, "Could not communicate with host."), real);
  // GUI can copy the callback cause before discarding queued notifications and destroying the
  // client.
  const auto retained = callbacks.Read(4);
  callbacks.Begin(5);
  EXPECT_TRUE(callbacks.Capture(4, "Late old-room disconnect").empty());
  EXPECT_TRUE(callbacks.Read(5).empty());
  EXPECT_EQ(retained, real);
}

TEST(ProjectPlusOnlineSetup, CanonicalAdmissionAllowsOwnedSelfJoinAndRejectsExistingRooms)
{
  EXPECT_TRUE(CanJoinRoom(false, false, false, false));
  EXPECT_TRUE(CanJoinRoom(false, true, false, true));
  EXPECT_FALSE(CanJoinRoom(false, true, false, false));
  EXPECT_FALSE(CanJoinRoom(true, true, false, true));
  EXPECT_FALSE(CanJoinRoom(false, true, true, true));
  EXPECT_FALSE(CanJoinRoom(true, false, false, false));
  EXPECT_FALSE(
      CanJoinRoom(false, false, true, false));  // Hidden initialized room still owns admission.
}

TEST(ProjectPlusOnlineSetup, PausedBRequiresReleaseThenFreshPressAndDispatchesOnce)
{
  PausedCancelObserver reader;
  SetupActionContext context{identity, 1, true};
  EXPECT_FALSE(reader.Poll(context, true, true, now));
  EXPECT_FALSE(reader.Poll(context, true, false, now + 5ms));
  EXPECT_TRUE(reader.Poll(context, true, true, now + 10ms));
  EXPECT_FALSE(reader.Poll(context, true, true, now + 15ms));
  EXPECT_FALSE(reader.Poll(context, true, false, now + 20ms));
  EXPECT_TRUE(reader.Poll(context, true, true, now + 25ms));
}

TEST(ProjectPlusOnlineSetup, OwnershipAndGameAllocationNeverTurnHeldBIntoANewPress)
{
  for (int field = 0; field != 6; ++field)
  {
    PausedCancelObserver reader;
    SetupActionContext context{identity, 1, true};
    reader.Poll(context, true, false, now);
    switch (field)
    {
    case 0:
      ++context.identity.core;
      break;
    case 1:
      ++context.identity.input;
      break;
    case 2:
      ++context.identity.attempt;
      break;
    case 3:
      ++context.identity.room;
      break;
    case 4:
      ++context.identity.game;
      break;
    case 5:
      ++context.revision;
      break;
    }
    EXPECT_FALSE(reader.Poll(context, true, true, now + 5ms));
    EXPECT_FALSE(reader.Poll(context, true, false, now + 10ms));
    EXPECT_TRUE(reader.Poll(context, true, true, now + 15ms));
  }
}

TEST(ProjectPlusOnlineSetup, FocusDeviceEligibilityAndSamplingLossRequireAnotherRelease)
{
  SetupActionContext context{identity, 1, true};
  for (int loss = 0; loss != 4; ++loss)
  {
    PausedCancelObserver reader;
    reader.Poll(context, true, false, now);
    auto unavailable = context;
    if (loss == 2)
      unavailable.enabled = false;
    if (loss != 3)
    {
      EXPECT_FALSE(reader.Poll(unavailable, loss == 2, true, now + 5ms));
    }
    const auto resumed = loss == 3 ? now + 251ms : now + 10ms;
    EXPECT_FALSE(reader.Poll(context, true, true, resumed));
    EXPECT_FALSE(reader.Poll(context, true, false, resumed + 5ms));
    EXPECT_TRUE(reader.Poll(context, true, true, resumed + 10ms));
  }
}

TEST(ProjectPlusOnlineSetup, ReaderAndGUIHeartbeatAreRequiredAndOnlyOneRequestCanBePending)
{
  SetupActionMailbox mailbox;
  const auto context = mailbox.Publish(identity, true, now);
  EXPECT_FALSE(mailbox.Available(context, now));
  EXPECT_FALSE(mailbox.Queue(context, now));
  mailbox.Reader(context, true, now);
  EXPECT_TRUE(mailbox.Available(context, now));
  const auto request = mailbox.Queue(context, now);
  ASSERT_TRUE(request);
  EXPECT_FALSE(mailbox.Queue(context, now + 5ms));
  EXPECT_TRUE(mailbox.Complete(*request, now + 5ms));
  EXPECT_FALSE(mailbox.Complete(*request, now + 5ms));
  EXPECT_FALSE(mailbox.Available(context, now + 251ms));
  // GUI blocking expires authority even if the input thread continues sampling.
  mailbox.Reader(context, true, now + 300ms);
  EXPECT_FALSE(mailbox.Read(now + 300ms).enabled);
  EXPECT_FALSE(mailbox.Queue(context, now + 300ms));
}

TEST(ProjectPlusOnlineSetup, QueuedCancelCannotCrossAnyIdentityOrReaderLoss)
{
  for (int field = 0; field != 8; ++field)
  {
    SetupActionMailbox mailbox;
    const auto context = mailbox.Publish(identity, true, now);
    mailbox.Reader(context, true, now);
    const auto request = mailbox.Queue(context, now);
    ASSERT_TRUE(request);
    auto next = identity;
    switch (field)
    {
    case 0:
      ++next.core;
      break;
    case 1:
      ++next.input;
      break;
    case 2:
      ++next.attempt;
      break;
    case 3:
      ++next.room;
      break;
    case 4:
      ++next.game;
      break;
    case 5:
      mailbox.Invalidate();
      break;
    case 6:
      mailbox.Reader(context, false, now);
      break;
    case 7:
      mailbox.Publish(next, false, now);
      break;
    }
    if (field < 5)
      mailbox.Publish(next, true, now);
    EXPECT_FALSE(mailbox.Complete(*request, now));
    // Completion drains the one pending slot even after invalidation.
    const auto current = mailbox.Publish(next, true, now + 5ms);
    mailbox.Reader(current, true, now + 5ms);
    EXPECT_TRUE(mailbox.Queue(current, now + 5ms));
  }
}

TEST(ProjectPlusOnlineSetup, ReaderLossAndRecoveryCannotResurrectAPendingPress)
{
  SetupActionMailbox mailbox;
  const auto context = mailbox.Publish(identity, true, now);
  mailbox.Reader(context, true, now);
  const auto request = mailbox.Queue(context, now);
  ASSERT_TRUE(request);
  mailbox.Reader(context, false, now + 5ms);
  mailbox.Reader(context, true, now + 10ms);
  EXPECT_TRUE(mailbox.Available(context, now + 10ms));
  EXPECT_FALSE(mailbox.Complete(*request, now + 10ms));
  const auto second = mailbox.Queue(context, now + 10ms);
  ASSERT_TRUE(second);
  mailbox.Publish(identity, true, now + 300ms);
  mailbox.Reader(context, true, now + 300ms);  // Sampling gap while the GUI kept its authority.
  EXPECT_FALSE(mailbox.Complete(*second, now + 300ms));
}

TEST(ProjectPlusOnlineSetup, BlockingPublicationDisarmsReaderAndRevokesAnAlreadyQueuedCancel)
{
  SetupActionMailbox mailbox;
  PausedCancelObserver reader;
  auto context = mailbox.Publish(identity, true, now);
  reader.Poll(context, true, false, now);
  mailbox.Reader(context, true, now);
  auto request = mailbox.Queue(context, now);
  ASSERT_TRUE(request);
  context = mailbox.Publish(identity, false, now + 5ms);  // Prepaint before constructor/hash.
  EXPECT_FALSE(reader.Poll(context, false, true, now + 5ms));
  EXPECT_FALSE(mailbox.Complete(*request, now + 5ms));
  context = mailbox.Publish(identity, true, now + 10ms);
  EXPECT_FALSE(reader.Poll(context, true, true, now + 10ms));
  EXPECT_FALSE(reader.Poll(context, true, false, now + 15ms));
  EXPECT_TRUE(reader.Poll(context, true, true, now + 20ms));
}

TEST(ProjectPlusOnlineSetup, ConsumedBReachesLocalSIAcrossRestoreUntilPhysicalRelease)
{
  CancelReleaseHandoff handoff;
  LocalChord si;
  handoff.Arm(9, 10);  // GUI binds accepted cancel after the restore's new Core/input epochs.
  EXPECT_TRUE(handoff.Consume(9, 10, true, true));
  EXPECT_TRUE(handoff.Consume(9, 10, false, false));  // Device loss is not a physical release.
  EXPECT_TRUE(handoff.Consume(9, 10, true, false, false));  // UI gate zeroes are not release.
  EXPECT_EQ(si.Poll(true, 0x200, 0), LocalChord::Action::None);
  EXPECT_TRUE(handoff.Consume(9, 10, true, true));  // Resume cannot leak CSS Back.
  EXPECT_FALSE(handoff.Consume(9, 10, true, false));
  EXPECT_FALSE(handoff.Consume(9, 10, true, true));  // Next B reaches ordinary local input.
  handoff.Arm(9, 10);
  EXPECT_FALSE(handoff.Consume(10, 10, true, true));  // Never filters an unrelated Core.
  handoff.Arm(9, 10);
  EXPECT_FALSE(handoff.Consume(9, 11, true, true));
}
}  // namespace
