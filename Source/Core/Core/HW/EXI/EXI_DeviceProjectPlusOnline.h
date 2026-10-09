// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <memory>

#include "Core/HW/EXI/EXI_Device.h"
#include "Core/ProjectPlusOnline/Status.h"

namespace ExpansionInterface
{
// Development-only command endpoint. No networking or Qt dependencies.
class CEXIProjectPlusOnline final : public IEXIDevice
{
public:
  explicit CEXIProjectPlusOnline(Core::System& system);
  ~CEXIProjectPlusOnline() override;
  bool IsPresent() const override;
  void SetCS(int cs) override;
  void DMAWrite(u32 address, u32 size) override;
  void DMARead(u32 address, u32 size) override;
  void DoState(PointerWrap& p) override;
  void Invalidate();
  static bool IsPrototypeEnabled();

private:
  void TransferByte(u8& byte) override;
  bool Eligible();
  u8* Buffer(u32 address, u32 size);
  struct DispatchState
  {
    std::atomic<bool> active{true};
    std::atomic<bool> pending{false};
  };
  std::shared_ptr<DispatchState> m_dispatch = std::make_shared<DispatchState>();
  ProjectPlusOnline::Session m_session;
  ProjectPlusOnline::CancelObservation m_cancel_observation;
  ProjectPlusOnline::CancelObservation m_response_observation;
  ProjectPlusOnline::StatusPacket m_response{};
  u32 m_response_size = ProjectPlusOnline::PACKET_SIZE;
  u32 m_identity_position = 0;
  bool m_identity_valid = true;
};

// Called on the CPU thread when the emulated reset button is pressed.
void InvalidateProjectPlusOnline(Core::System& system);
}  // namespace ExpansionInterface
