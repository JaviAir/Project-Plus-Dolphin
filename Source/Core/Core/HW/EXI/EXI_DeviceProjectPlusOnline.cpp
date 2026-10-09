// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/HW/EXI/EXI_DeviceProjectPlusOnline.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "Common/ChunkFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/System.h"

namespace ExpansionInterface
{
using namespace ProjectPlusOnline;

bool CEXIProjectPlusOnline::IsPrototypeEnabled()
{
  const char* opt_in = std::getenv("PPLUS_ONLINE_EXI_PROTOTYPE");
  const auto game_id = SConfig::GetInstance().GetGameID();
  return opt_in && std::string_view(opt_in) == "1" &&
         (game_id == "ID-Netplay Launcher" || game_id == "ID-Offline Launcher") &&
         Config::Get(Config::MAIN_SLOT_B) == EXIDeviceType::None;
}

CEXIProjectPlusOnline::CEXIProjectPlusOnline(Core::System& system) : IEXIDevice(system)
{
  if (!IsPrototypeEnabled() || Core::WantsDeterminism())
    Invalidate();
}

CEXIProjectPlusOnline::~CEXIProjectPlusOnline()
{
  Invalidate();
}

void CEXIProjectPlusOnline::Invalidate()
{
  m_dispatch->active = false;
  m_response.fill(0);
}

bool CEXIProjectPlusOnline::Eligible()
{
  if (Core::WantsDeterminism())
    Invalidate();
  return m_dispatch->active;
}

bool CEXIProjectPlusOnline::IsPresent() const
{
  return m_dispatch->active && !Core::WantsDeterminism();
}

void CEXIProjectPlusOnline::SetCS(int cs)
{
  m_identity_position = 0;
  m_identity_valid = true;
}

void CEXIProjectPlusOnline::TransferByte(u8& byte)
{
  // Standard EXI ID query: two zero command bytes, followed by four read bytes.
  // Saturate, so an arbitrarily long transaction cannot wrap into another query.
  if (m_identity_position < 2)
    m_identity_valid &= byte == 0;
  byte = Eligible() && m_identity_valid && m_identity_position >= 2 && m_identity_position < 6 ?
             static_cast<u8>(MAGIC >> (8 * (5 - m_identity_position))) :
             0;
  m_identity_position = std::min(m_identity_position + 1, 6u);
}

u8* CEXIProjectPlusOnline::Buffer(u32 address, u32 size)
{
  // Only aligned MEM1/MEM2 DMA buffers (physical or cached/uncached aliases).
  // The SDK writes its buffer pointer directly to MAR. Validate without
  // GetPointerForRange's panic dialog on guest-controlled invalid addresses.
  if ((size != PACKET_SIZE && size != STATUS_PACKET_SIZE) || (address & 31) != 0)
    return nullptr;
  if ((address & 0xc0000000) == 0x40000000)
    return nullptr;
  address &= 0x3fffffff;
  auto& memory = m_system.GetMemory();
  if (memory.GetRAM() && address < memory.GetRamSizeReal() &&
      size <= memory.GetRamSizeReal() - address)
    return memory.GetRAM() + address;
  if (memory.GetEXRAM() && address >= 0x10000000)
  {
    const u32 offset = address - 0x10000000;
    if (offset < memory.GetExRamSizeReal() && size <= memory.GetExRamSizeReal() - offset)
      return memory.GetEXRAM() + offset;
  }
  return nullptr;
}

void CEXIProjectPlusOnline::DMAWrite(u32 address, u32 size)
{
  m_response.fill(0);
  m_response_observation.Reset();
  m_response_size = PACKET_SIZE;
  if (size != PACKET_SIZE || !Eligible())
    return;
  const u8* buffer = Buffer(address, size);
  if (!buffer)
    return;
  Packet request;
  std::copy_n(buffer, request.size(), request.begin());
  bool start_quickplay = false;
  const auto header = m_session.Accept(request, m_dispatch->pending, start_quickplay);
  const bool accepted = header[7] == static_cast<u8>(Result::Accepted);
  if (accepted && request[7] == static_cast<u8>(Command::Hello))
    m_cancel_observation.Reset();
  if (request[6] == 0 && request[7] == static_cast<u8>(Command::GetStatus))
  {
    m_response_size = STATUS_PACKET_SIZE;
    const auto before = guest_cancel_attempt.load();
    const auto snapshot = guest_status.Read();
    const auto after = guest_cancel_attempt.load();
    m_response = StatusResponse(header, snapshot);
    if (accepted)
      m_response_observation.Observe(before, snapshot, after);
  }
  else
  {
    std::copy(header.begin(), header.end(), m_response.begin());
  }
  const bool cancel = accepted && request[7] == static_cast<u8>(Command::CancelQuickPlay);
  const auto cancel_attempt = cancel ? m_cancel_observation.Target(guest_cancel_attempt.load()) : 0;
  if (!start_quickplay && !cancel)
    return;

  m_dispatch->pending = true;
  // Capture only owned bounded state, never the EXI device or guest memory.
  // One job maximum per device. Destruction/reset/load cancels via active.
  Core::QueueHostJob(
      [state = m_dispatch, cancel, cancel_attempt](Core::System& system) {
        const Core::CPUThreadGuard guard(system);
        if (Core::WantsDeterminism())
          state->active = false;
        if (state->active)
        {
          // Host jobs run on the frontend owner thread. Dispatch synchronously so
          // reset/load cannot slip between this eligibility check and delivery.
          // The frontend invokes the existing asynchronous controller path here.
          if (!cancel)
            Host_Message(HostMessageID::WMUserStartQuickPlay);
          else if (cancel_attempt && guest_cancel_attempt.load() == cancel_attempt)
            Host_Message(HostMessageID::WMUserCancelQuickPlay);
        }
        state->pending = false;
      },
      cancel);  // Accepted Cancel may race the old Core's stop notification.
}

void CEXIProjectPlusOnline::DMARead(u32 address, u32 size)
{
  u8* buffer = Buffer(address, size);
  if (!buffer)
    return;
  if (!Eligible())
    m_response.fill(0);
  if (size != m_response_size)
    m_response.fill(0);
  if (size == STATUS_PACKET_SIZE && size == m_response_size && Eligible() &&
      m_response[0] == 0x50 && m_response[7] == static_cast<u8>(Result::Accepted))
    m_cancel_observation = m_response_observation;
  std::copy_n(m_response.begin(), size, buffer);
  m_response.fill(0);  // One response consumption; never return stale ACKs.
}

void CEXIProjectPlusOnline::DoState(PointerWrap& p)
{
  // Intentionally no external state in savestates. Disable until a fresh boot,
  // including when this device was recreated by the EXI savestate factory.
  if (p.IsReadMode())
    Invalidate();
}

void InvalidateProjectPlusOnline(Core::System& system)
{
  if (!system.GetExpansionInterface().GetChannel(1))
    return;
  auto* device = system.GetExpansionInterface().GetDevice(Slot::B);
  if (device && device->m_device_type == EXIDeviceType::ProjectPlusOnline)
    static_cast<CEXIProjectPlusOnline*>(device)->Invalidate();
}
}  // namespace ExpansionInterface
