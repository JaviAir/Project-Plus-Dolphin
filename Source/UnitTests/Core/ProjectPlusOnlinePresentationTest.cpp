// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/ProjectPlusOnline/Presentation.h"

namespace
{
using namespace ProjectPlusOnline;

TEST(ProjectPlusOnlinePresentation, RetainedCopiesLoseAuthorityAtEveryOwnershipChange)
{
  PresentationMailbox mailbox;
  PresentationSnapshot value;
  value.identity = {1, 2, 3, 4, 5};
  value.entry = EntrySource::InGame;
  value.phase = PresentationPhase::Searching;
  value.cancel = true;
  value.text[0] = 'x';
  EXPECT_TRUE(mailbox.Publish(value));
  EXPECT_FALSE(mailbox.Publish(value));  // Unchanged publication needs no paused redraw.
  const auto retained = mailbox.Read(1, 2);
  EXPECT_EQ(retained.identity, value.identity);
  EXPECT_EQ(retained.text, value.text);
  EXPECT_EQ(mailbox.Read(2, 2).phase, PresentationPhase::Hidden);
  EXPECT_EQ(mailbox.Read(1, 3).phase, PresentationPhase::Hidden);
  for (int field = 0; field != 5; ++field)
  {
    auto replacement = value;
    switch (field)
    {
    case 0:
      ++replacement.identity.core;
      break;
    case 1:
      ++replacement.identity.input;
      break;
    case 2:
      ++replacement.identity.attempt;
      break;
    case 3:
      ++replacement.identity.room;
      break;
    case 4:
      ++replacement.identity.game;
      break;
    }
    mailbox.Publish(replacement);
    EXPECT_FALSE(mailbox.IsCurrent(retained));
    mailbox.Publish(value);
  }
  mailbox.Invalidate();
  EXPECT_FALSE(mailbox.IsCurrent(retained));
  EXPECT_EQ(mailbox.Read(1, 2).phase, PresentationPhase::Hidden);
  mailbox.Publish(value);
  EXPECT_FALSE(mailbox.IsCurrent(retained));  // Same values cannot resurrect an old copy.
  const auto observation = mailbox.Revision();
  mailbox.Invalidate();
  EXPECT_FALSE(mailbox.Publish(value, observation));  // Disconnect during GUI copy.
  EXPECT_EQ(mailbox.Read(1, 2).phase, PresentationPhase::Hidden);
}

TEST(ProjectPlusOnlinePresentation, ErrorWithoutAttemptAndEntryProvenanceAreCopied)
{
  PresentationMailbox mailbox;
  PresentationSnapshot value;
  value.identity = {1, 2, 0, 3, 0};  // Admission failed before controller/ticket exists.
  value.entry = EntrySource::InGame;
  value.phase = PresentationPhase::Error;
  value.text[0] = '%';
  mailbox.Publish(value);
  value.text[0] = 'x';
  EXPECT_EQ(mailbox.Read(1, 2).text[0], '%');
  EXPECT_EQ(mailbox.Read(1, 2).entry, EntrySource::InGame);
  EXPECT_FALSE(mailbox.Read(1, 2).cancel);
  value.entry = EntrySource::Desktop;
  mailbox.Publish(value);
  EXPECT_EQ(mailbox.Read(1, 2).entry, EntrySource::Desktop);
}

TEST(ProjectPlusOnlinePresentation, CPUObservationExpiresAndCannotCrossCoreOrInputEpoch)
{
  const auto now = std::chrono::steady_clock::now();
  const LocalCSSObservation css{1, 2, true, now};
  EXPECT_TRUE(css.IsFresh(1, 2, now));
  EXPECT_TRUE(css.IsFresh(1, 2, now + std::chrono::milliseconds(500)));
  EXPECT_FALSE(css.IsFresh(1, 2, now + std::chrono::milliseconds(501)));
  EXPECT_FALSE(css.IsFresh(2, 2, now));
  EXPECT_FALSE(css.IsFresh(1, 3, now));
  EXPECT_FALSE(css.IsFresh(1, 2, now - std::chrono::milliseconds(1)));
  LocalCSSMailbox mailbox;
  mailbox.Observe(1, 2, true);
  mailbox.Observe(1, 2, false);  // Device loss/unsupported scene revokes eligibility.
  EXPECT_FALSE(mailbox.Read().IsFresh(1, 2, std::chrono::steady_clock::now()));
}

TEST(ProjectPlusOnlinePresentation, CancelRequiresAllDispatchableRunningBoundaries)
{
  EXPECT_TRUE(CanPresentCancel(true, true, true, 4));
  EXPECT_FALSE(CanPresentCancel(false, true, true, 4));  // Paused observer belongs to Step2b.
  EXPECT_FALSE(CanPresentCancel(true, false, true, 4));
  EXPECT_FALSE(CanPresentCancel(true, true, false, 4));  // Hash/constructor/handshake wait.
  EXPECT_FALSE(CanPresentCancel(true, true, true, 0));   // No admission/owned attempt.
}

TEST(ProjectPlusOnlinePresentation, PrepaintContinuationIsBoundToCompleteRequestIdentity)
{
  const PrepaintFence request{{1, 2, 3, 4, 5}, 6, 7};
  for (int field = 0; field != 7; ++field)
  {
    auto current = request;
    switch (field)
    {
    case 0:
      ++current.identity.core;
      break;
    case 1:
      ++current.identity.input;
      break;
    case 2:
      ++current.identity.attempt;
      break;
    case 3:
      ++current.identity.room;
      break;
    case 4:
      ++current.identity.game;
      break;
    case 5:
      ++current.request;
      break;
    case 6:
      ++current.window;
      break;
    }
    EXPECT_NE(request, current);
  }
}
}  // namespace
