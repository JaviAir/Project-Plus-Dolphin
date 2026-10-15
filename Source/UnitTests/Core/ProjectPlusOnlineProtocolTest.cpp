// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/ProjectPlusOnline/Chord.h"

TEST(ProjectPlusOnlineProtocol, PersistentChordRequiresReleaseAndEligibleLocalCSS)
{
  ProjectPlusOnline::LocalChord chord;
  using Action = ProjectPlusOnline::LocalChord::Action;
  EXPECT_EQ(chord.Poll(true, 0x68, 0), Action::None);
  EXPECT_EQ(chord.Poll(true, 0, 0), Action::None);
  EXPECT_EQ(chord.Poll(true, 0x68, 0), Action::Search);
  EXPECT_EQ(chord.Poll(true, 0x68, 0), Action::None);
  EXPECT_EQ(chord.Poll(true, 0x60, 0), Action::None);
  EXPECT_EQ(chord.Poll(true, 0x68, 0), Action::None);
  EXPECT_EQ(chord.Poll(false, 0, 0), Action::None);
  EXPECT_EQ(chord.Poll(true, 0x68, 0), Action::None);
  EXPECT_EQ(chord.Poll(true, 0, 7), Action::None);
  EXPECT_EQ(chord.Poll(true, 0x68, 7), Action::None);
}

TEST(ProjectPlusOnlineProtocol, PersistentCancelOwnsPressThroughRelease)
{
  ProjectPlusOnline::LocalChord chord;
  using Action = ProjectPlusOnline::LocalChord::Action;
  EXPECT_EQ(chord.Poll(true, 0x200, 7), Action::None);
  EXPECT_EQ(chord.Poll(true, 0, 7), Action::None);
  EXPECT_EQ(chord.Poll(true, 0x200, 7), Action::Cancel);
  EXPECT_TRUE(chord.ConsumesCancel());
  EXPECT_EQ(chord.Poll(true, 0x200, 0), Action::None);
  EXPECT_TRUE(chord.ConsumesCancel());
  EXPECT_EQ(chord.Poll(true, 0, 0), Action::None);
  EXPECT_FALSE(chord.ConsumesCancel());
  EXPECT_EQ(chord.Poll(true, 0x200, 0), Action::None);
  EXPECT_FALSE(chord.ConsumesCancel());
}
