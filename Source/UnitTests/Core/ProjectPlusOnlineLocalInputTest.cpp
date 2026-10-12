// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string>

#include <gtest/gtest.h>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"
#include "Core/ProjectPlusOnline/LocalInput.h"
#include "Core/ProjectPlusOnline/SetupAction.h"

namespace
{
using namespace ProjectPlusOnline;
using namespace std::chrono_literals;
using Action = LocalChord::Action;
using SerialInterface::SIDevices;
constexpr auto STANDARD = SerialInterface::SIDEVICE_GC_CONTROLLER;
constexpr auto WIIU = SerialInterface::SIDEVICE_WIIU_ADAPTER;
constexpr auto NONE = SerialInterface::SIDEVICE_NONE;
constexpr std::uint16_t SEARCH = PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_UP;
constexpr std::array SOURCES{LocalPadSource::Mapped, LocalPadSource::WiiUAdapter};

// What each source reports for a focused, connected controller holding `buttons`.
LocalCommandSample Held(LocalPadSource source, std::uint16_t buttons)
{
  GCPadStatus status;
  status.button = buttons;
  return MakeLocalCommandSample(source, status, true, true);
}

TEST(ProjectPlusOnlineLocalInput, SourceFollowsTheConfiguredPortOneType)
{
  EXPECT_EQ(LocalPadSourceFor(STANDARD), LocalPadSource::Mapped);
  EXPECT_EQ(LocalPadSourceFor(WIIU), LocalPadSource::WiiUAdapter);
  for (int type = 0; type != SerialInterface::SIDEVICE_COUNT; ++type)
  {
    const auto device = static_cast<SIDevices>(type);
    if (device != WIIU)
      EXPECT_EQ(LocalPadSourceFor(device), LocalPadSource::Mapped) << type;  // Same as NetPlay.
    EXPECT_EQ(SupportsLocalCommands(device), device == STANDARD || device == WIIU) << type;
  }
}

TEST(ProjectPlusOnlineLocalInput, EachSourceReportsConnectionReleaseAuthorityAndFocus)
{
  GCPadStatus status;
  status.button = PAD_BUTTON_B;
  // Mapped input is gated upstream; a closed gate never proves a release.
  auto mapped = MakeLocalCommandSample(LocalPadSource::Mapped, status, false, true);
  EXPECT_TRUE(mapped.connected);
  EXPECT_FALSE(mapped.authoritative);
  EXPECT_FALSE(mapped.focused);
  mapped = MakeLocalCommandSample(LocalPadSource::Mapped, status, true, false);
  EXPECT_FALSE(mapped.authoritative);
  status.isConnected = false;
  EXPECT_FALSE(MakeLocalCommandSample(LocalPadSource::Mapped, status, true, true).connected);

  // The adapter's GCPadStatus is "connected" even with an empty port; the port state decides.
  status.isConnected = true;
  auto adapter = MakeLocalCommandSample(LocalPadSource::WiiUAdapter, status, true, false);
  EXPECT_FALSE(adapter.connected);
  EXPECT_FALSE(adapter.authoritative);
  adapter = MakeLocalCommandSample(LocalPadSource::WiiUAdapter, status, false, true);
  EXPECT_TRUE(adapter.connected);
  EXPECT_TRUE(adapter.authoritative);  // Physical state, independent of focus.
  EXPECT_FALSE(adapter.focused);
  EXPECT_EQ(adapter.buttons, PAD_BUTTON_B);
}

TEST(ProjectPlusOnlineLocalInput, SearchRequiresReleaseAndDispatchesOncePerPressOnBothSources)
{
  for (const auto source : SOURCES)
  {
    LocalCommandReader reader;
    // Held from the start of the input lifetime: no search.
    EXPECT_EQ(reader.Poll(1, true, Held(source, SEARCH), 0).action, Action::None);
    EXPECT_TRUE(reader.Poll(1, true, Held(source, 0), 0).observed);
    EXPECT_EQ(reader.Poll(1, true, Held(source, PAD_TRIGGER_L | PAD_TRIGGER_R), 0).action,
              Action::None);
    EXPECT_EQ(reader.Poll(1, true, Held(source, SEARCH), 0).action, Action::Search);
    EXPECT_EQ(reader.Poll(1, true, Held(source, SEARCH), 0).action, Action::None);
    // Partial release keeps it latched; all three components must be released.
    EXPECT_EQ(reader.Poll(1, true, Held(source, PAD_BUTTON_UP), 0).action, Action::None);
    EXPECT_EQ(reader.Poll(1, true, Held(source, SEARCH), 0).action, Action::None);
    reader.Poll(1, true, Held(source, PAD_BUTTON_A), 0);
    EXPECT_EQ(reader.Poll(1, true, Held(source, SEARCH | PAD_BUTTON_A), 0).action, Action::Search);
    // A pending attempt owns the chord: no second search while cancellable.
    reader.Poll(1, true, Held(source, 0), 7);
    EXPECT_EQ(reader.Poll(1, true, Held(source, SEARCH), 7).action, Action::None);
  }
}

TEST(ProjectPlusOnlineLocalInput, CancelIsReleaseThenBAndConsumedThroughReleaseOnBothSources)
{
  for (const auto source : SOURCES)
  {
    LocalCommandReader reader;
    // B held when the attempt appears is not a cancel.
    EXPECT_EQ(reader.Poll(1, true, Held(source, PAD_BUTTON_B), 7).action, Action::None);
    reader.Poll(1, true, Held(source, 0), 7);
    const auto cancel = reader.Poll(1, true, Held(source, PAD_BUTTON_B), 7);
    EXPECT_EQ(cancel.action, Action::Cancel);
    EXPECT_TRUE(cancel.consume_b);
    // Attempt cleared while B is still held: still consumed, no Back leak or new cancel.
    const auto held = reader.Poll(1, true, Held(source, PAD_BUTTON_B), 0);
    EXPECT_EQ(held.action, Action::None);
    EXPECT_TRUE(held.consume_b);
    EXPECT_FALSE(reader.Poll(1, true, Held(source, 0), 0).consume_b);
    EXPECT_FALSE(reader.Poll(1, true, Held(source, PAD_BUTTON_B), 0).consume_b);
  }
}

TEST(ProjectPlusOnlineLocalInput, HeldInputNeverBecomesAFreshPressAcrossSourcesOrEpochs)
{
  LocalCommandReader reader;
  reader.Poll(1, true, Held(LocalPadSource::Mapped, 0), 0);
  // Port 1 switches to the adapter while its controller already holds the chord.
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, SEARCH), 0).action,
            Action::None);
  reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, 0), 0);
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, SEARCH), 0).action,
            Action::Search);
  // And back: the mapped controller's held chord is not a new search either.
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::Mapped, SEARCH), 0).action, Action::None);

  // B armed on one source cannot cancel through the other while held.
  reader.Poll(1, true, Held(LocalPadSource::Mapped, 0), 9);
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, PAD_BUTTON_B), 9).action,
            Action::None);
  reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, 0), 9);
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, PAD_BUTTON_B), 9).action,
            Action::Cancel);

  // Retirement/restore advances the input epoch: a chord held through it is ignored.
  reader.Poll(2, true, Held(LocalPadSource::WiiUAdapter, 0), 0);
  EXPECT_EQ(reader.Poll(3, true, Held(LocalPadSource::WiiUAdapter, SEARCH), 0).action,
            Action::None);
}

TEST(ProjectPlusOnlineLocalInput, IneligibleUnpluggedOrUnfocusedInputCannotDispatch)
{
  LocalCommandReader reader;
  reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, 0), 0);
  // Not local CSS / persistent local input disabled: no observation, disarmed.
  const auto outside = reader.Poll(1, false, Held(LocalPadSource::WiiUAdapter, SEARCH), 0);
  EXPECT_FALSE(outside.observed);
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, SEARCH), 0).action,
            Action::None);

  // Empty adapter port: no hint, nothing dispatches, even though the status says "connected".
  GCPadStatus status;
  status.button = SEARCH;
  reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, 0), 0);
  const auto empty = reader.Poll(
      1, true, MakeLocalCommandSample(LocalPadSource::WiiUAdapter, status, true, false), 0);
  EXPECT_FALSE(empty.observed);
  EXPECT_EQ(empty.action, Action::None);

  // Unfocused adapter press is latched and dropped; refocus while held does nothing.
  reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, 0), 0);
  const auto unfocused = reader.Poll(
      1, true, MakeLocalCommandSample(LocalPadSource::WiiUAdapter, status, false, true), 0);
  EXPECT_TRUE(unfocused.observed);
  EXPECT_EQ(unfocused.action, Action::None);
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, SEARCH), 0).action,
            Action::None);
  reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, 0), 0);
  EXPECT_EQ(reader.Poll(1, true, Held(LocalPadSource::WiiUAdapter, SEARCH), 0).action,
            Action::Search);
}

TEST(ProjectPlusOnlineLocalInput, PausedCancelHandoffEndsOnlyAtAnAuthoritativeRelease)
{
  for (const auto source : SOURCES)
  {
    CancelReleaseHandoff handoff;
    handoff.Arm(4, 5);
    const auto b = Held(source, PAD_BUTTON_B);
    EXPECT_TRUE(handoff.Consume(4, 5, b.connected, true, b.authoritative));
    // Unplugged/unfocused neutral readings cannot prove the button was released.
    GCPadStatus neutral;
    const auto unplugged = MakeLocalCommandSample(source, neutral, true, false);
    EXPECT_TRUE(handoff.Consume(4, 5, unplugged.connected, false, unplugged.authoritative));
    if (source == LocalPadSource::Mapped)
    {
      const auto unfocused = MakeLocalCommandSample(source, neutral, false, true);
      EXPECT_TRUE(handoff.Consume(4, 5, unfocused.connected, false, unfocused.authoritative));
    }
    const auto released = Held(source, 0);
    EXPECT_FALSE(handoff.Consume(4, 5, released.connected, false, released.authoritative));
    EXPECT_FALSE(handoff.Consume(4, 5, b.connected, true, b.authoritative));  // Fresh B reaches the guest.
  }
}

TEST(ProjectPlusOnlineLocalInput, PausedSetupBNeedsReleaseOnTheCurrentSource)
{
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::time_point{10s};
  const SetupActionContext context{{1, 2, 3, 4, 0}, 1, true};
  for (const auto source : SOURCES)
  {
    PausedCancelObserver observer;
    EXPECT_FALSE(observer.Poll(context, true, true, now, source));
    EXPECT_FALSE(observer.Poll(context, true, false, now + 5ms, source));
    EXPECT_TRUE(observer.Poll(context, true, true, now + 10ms, source));
    EXPECT_FALSE(observer.Poll(context, true, true, now + 15ms, source));
  }
  PausedCancelObserver observer;
  EXPECT_FALSE(observer.Poll(context, true, false, now, LocalPadSource::Mapped));
  // Source switched with B already held on the adapter.
  EXPECT_FALSE(observer.Poll(context, true, true, now + 5ms, LocalPadSource::WiiUAdapter));
  EXPECT_FALSE(observer.Poll(context, true, false, now + 10ms, LocalPadSource::WiiUAdapter));
  EXPECT_TRUE(observer.Poll(context, true, true, now + 15ms, LocalPadSource::WiiUAdapter));
}

TEST(ProjectPlusOnlineLocalInput, AdmissionAcceptsOnlyAStandardContractWithEmptyPorts3And4)
{
  const auto konga = SerialInterface::SIDEVICE_GC_TARUKONGA;
  EXPECT_TRUE(AdmitsPersistentSIDevices({STANDARD, STANDARD, NONE, NONE}));
  EXPECT_TRUE(AdmitsPersistentSIDevices({WIIU, STANDARD, NONE, NONE}));
  // The adapter is only accepted as the local player's port 1.
  EXPECT_FALSE(AdmitsPersistentSIDevices({STANDARD, WIIU, NONE, NONE}));
  EXPECT_FALSE(AdmitsPersistentSIDevices({WIIU, WIIU, NONE, NONE}));
  EXPECT_FALSE(AdmitsPersistentSIDevices({WIIU, STANDARD, WIIU, NONE}));
  EXPECT_FALSE(AdmitsPersistentSIDevices({STANDARD, STANDARD, STANDARD, NONE}));
  EXPECT_FALSE(AdmitsPersistentSIDevices({STANDARD, STANDARD, NONE, WIIU}));
  EXPECT_FALSE(AdmitsPersistentSIDevices({konga, STANDARD, NONE, NONE}));
  EXPECT_FALSE(AdmitsPersistentSIDevices({NONE, STANDARD, NONE, NONE}));

  // Guest-visible/NetPlay-requested device for each configured local port.
  EXPECT_EQ(ContractSIDevice(0, WIIU), STANDARD);
  EXPECT_EQ(ContractSIDevice(0, STANDARD), STANDARD);
  EXPECT_EQ(ContractSIDevice(0, konga), konga);
  EXPECT_EQ(ContractSIDevice(1, WIIU), WIIU);
  EXPECT_EQ(ContractSIDevice(2, WIIU), WIIU);
}

class ProjectPlusOnlineLocalInputConfig : public ::testing::Test
{
protected:
  void SetUp() override { Config::Init(); }
  void TearDown() override { Config::Shutdown(); }
  static std::string Identity(const std::array<SIDevices, 4>& devices)
  {
    for (int port = 0; port < 4; ++port)
      Config::SetCurrent(Config::GetInfoForSIDevice(port), devices[port]);
    return ConfigLoaders::RunningNetPlayConfigIdentity();
  }
};

TEST_F(ProjectPlusOnlineLocalInputConfig, MixedAdapterAndMappedPeersShareTheGuestContract)
{
  const auto mapped = Identity({STANDARD, STANDARD, NONE, NONE});
  const auto adapter = Identity({WIIU, STANDARD, NONE, NONE});
  EXPECT_FALSE(mapped.empty());
  EXPECT_EQ(adapter, mapped);
  // Unchanged bytes for the existing receipt's standard configuration.
  EXPECT_NE(mapped.find("SI device=" + std::to_string(int(STANDARD)) + "\n"), std::string::npos);
  EXPECT_EQ(mapped.find("SI device=" + std::to_string(int(WIIU)) + "\n"), std::string::npos);
}

TEST_F(ProjectPlusOnlineLocalInputConfig, GuestVisibleDeviceDifferencesStillMismatch)
{
  const auto mapped = Identity({STANDARD, STANDARD, NONE, NONE});
  EXPECT_NE(Identity({STANDARD, WIIU, NONE, NONE}), mapped);
  EXPECT_NE(Identity({SerialInterface::SIDEVICE_GC_TARUKONGA, STANDARD, NONE, NONE}), mapped);
  EXPECT_NE(Identity({WIIU, STANDARD, STANDARD, NONE}), mapped);
}

TEST_F(ProjectPlusOnlineLocalInputConfig, HostObserverReportsUnsupportedOrUnpluggedPortOneAsDisconnected)
{
  // No adapter attached in the test process; a standard mapping is not initialized either.
  Config::SetCurrent(Config::GetInfoForSIDevice(0), WIIU);
  auto sample = PeekLocalCommandPad();
  EXPECT_EQ(sample.source, LocalPadSource::WiiUAdapter);
  EXPECT_FALSE(sample.connected);
  EXPECT_FALSE(LocalCommandDeviceConnected());
  // Adapter sampling never falls back to the mapped Standard Controller.
  EXPECT_EQ(SampleLocalPad(0).button, 0);

  Config::SetCurrent(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_TARUKONGA);
  EXPECT_FALSE(PeekLocalCommandPad().connected);
  EXPECT_FALSE(LocalCommandDeviceConnected());
}
}  // namespace
