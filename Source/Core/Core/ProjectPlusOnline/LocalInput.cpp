// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/ProjectPlusOnline/LocalInput.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/GCPad.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/InputConfig.h"

namespace ProjectPlusOnline
{
GCPadStatus SampleLocalPad(int local_pad)
{
  if (LocalPadSourceFor(Config::Get(Config::GetInfoForSIDevice(local_pad))) ==
      LocalPadSource::WiiUAdapter)
  {
    return GCAdapter::Input(local_pad);
  }
  return Pad::GetStatus(local_pad);
}

bool LocalCommandDeviceConnected()
{
  const auto configured = Config::Get(Config::GetInfoForSIDevice(0));
  if (!SupportsLocalCommands(configured))
    return false;
  if (LocalPadSourceFor(configured) == LocalPadSource::WiiUAdapter)
    return GCAdapter::DeviceConnected(0);
  if (!Pad::IsInitialized())
    return false;
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  return Pad::GetConfig()->GetController(0)->IsDefaultDeviceConnected();
}

LocalCommandSample PeekLocalCommandPad()
{
  const auto configured = Config::Get(Config::GetInfoForSIDevice(0));
  const auto source = LocalPadSourceFor(configured);
  if (!LocalCommandDeviceConnected())
  {
    GCPadStatus none;
    none.isConnected = false;
    return MakeLocalCommandSample(source, none, ControlReference::GetInputGate(), false);
  }
  // PeekInput leaves a new adapter connection's origin report for the SI/NetPlay sampler.
  const GCPadStatus status =
      source == LocalPadSource::WiiUAdapter ? GCAdapter::PeekInput(0) : Pad::GetStatus(0);
  return MakeLocalCommandSample(source, status, ControlReference::GetInputGate(), true);
}
}  // namespace ProjectPlusOnline
