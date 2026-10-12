// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

#include "Core/HW/SI/SI_Device.h"
#include "Core/ProjectPlusOnline/Chord.h"
#include "InputCommon/GCPadStatus.h"

// One local-command source: the logical GC buttons of this instance's local
// controller 1, whichever supported input drives it. Search, cancel, paused
// setup B and later session actions read this, never a particular device.
namespace ProjectPlusOnline
{
enum class LocalPadSource : std::uint8_t
{
  Mapped,       // Standard Controller mapping: keyboard, evdev/SDL, PC-mode adapter.
  WiiUAdapter,  // The matching port of a GameCube Adapter for Wii U.
};

// Same rule as NetPlayClient local sampling.
constexpr LocalPadSource LocalPadSourceFor(SerialInterface::SIDevices configured)
{
  return configured == SerialInterface::SIDEVICE_WIIU_ADAPTER ? LocalPadSource::WiiUAdapter :
                                                                LocalPadSource::Mapped;
}
constexpr bool SupportsLocalCommands(SerialInterface::SIDevices configured)
{
  return configured == SerialInterface::SIDEVICE_GC_CONTROLLER ||
         configured == SerialInterface::SIDEVICE_WIIU_ADAPTER;
}

// Device the guest sees for a configured local port in a persistent session.
// A Wii U adapter on local port 1 is only an input source: the restored
// checkpoint, NetPlay device requests and peer identity keep the standard
// controller, so both peers emulate identical SI devices.
constexpr SerialInterface::SIDevices ContractSIDevice(int port,
                                                      SerialInterface::SIDevices configured)
{
  return port == 0 && configured == SerialInterface::SIDEVICE_WIIU_ADAPTER ?
             SerialInterface::SIDEVICE_GC_CONTROLLER :
             configured;
}
constexpr bool AdmitsPersistentSIDevices(const std::array<SerialInterface::SIDevices, 4>& configured)
{
  for (int port = 0; port < 4; ++port)
  {
    if (ContractSIDevice(port, configured[port]) != (port < 2 ? SerialInterface::SIDEVICE_GC_CONTROLLER :
                                                                SerialInterface::SIDEVICE_NONE))
      return false;
  }
  return true;
}

struct LocalCommandSample
{
  LocalPadSource source = LocalPadSource::Mapped;
  std::uint16_t buttons = 0;
  bool connected = false;      // A real device currently drives local controller 1.
  bool authoritative = false;  // A neutral reading proves a physical release.
  bool focused = false;        // Input gate open: commands may dispatch.
};

// Command view of an already sampled status; never alters what the guest receives.
// Mapped input is gated upstream, so a closed gate reads neutral but cannot prove
// release. The adapter reports physical state regardless of focus; it proves
// release whenever its port is connected, and focus only gates dispatch.
constexpr LocalCommandSample MakeLocalCommandSample(LocalPadSource source, const GCPadStatus& status,
                                                    bool input_gate, bool device_connected)
{
  if (source == LocalPadSource::WiiUAdapter)
    return {source, status.button, device_connected, device_connected, input_gate};
  return {source, status.button, status.isConnected, input_gate && device_connected, input_gate};
}

// CPU-owned search/cancel recognition for the independent local CSS branch.
class LocalCommandReader
{
public:
  struct Result
  {
    LocalChord::Action action = LocalChord::Action::None;
    bool observed = false;   // Eligible local CSS with a connected source.
    bool consume_b = false;  // Keep an accepted cancel's B out of the guest until release.
  };
  Result Poll(std::uint64_t epoch, bool eligible, const LocalCommandSample& sample,
              std::uint64_t cancel_attempt)
  {
    // A new input lifetime or a different source never turns a held chord/B
    // into a fresh press: rearming needs a release seen on the current source.
    if (epoch != m_epoch || sample.source != m_source)
    {
      m_chord = {};
      m_epoch = epoch;
      m_source = sample.source;
    }
    Result result;
    result.observed = eligible && sample.connected;
    const auto action = m_chord.Poll(result.observed, sample.buttons, cancel_attempt);
    // An unfocused press is latched and dropped; the user releases and presses again.
    result.action = sample.focused ? action : LocalChord::Action::None;
    result.consume_b = m_chord.ConsumesCancel();
    return result;
  }

private:
  LocalChord m_chord;
  std::uint64_t m_epoch = 0;
  LocalPadSource m_source = LocalPadSource::Mapped;
};

// Implemented in Core. CPU/SI and NetPlay sampling: consumes the adapter's
// new-connection origin report exactly as the device or NetPlay would.
GCPadStatus SampleLocalPad(int local_pad);
// Host/GUI threads: a non-consuming view of local controller 1, or a
// disconnected sample when its configured type supports no local commands.
LocalCommandSample PeekLocalCommandPad();
bool LocalCommandDeviceConnected();
}  // namespace ProjectPlusOnline
