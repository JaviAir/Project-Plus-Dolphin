// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include "Core/ProjectPlusOnline/LobbyPresentation.h"
#include "Core/ProjectPlusOnline/Status.h"

namespace ProjectPlusOnline
{
enum class PresentationPhase
{
  Hidden,
  LocalCSS,
  Checking,
  Searching,
  Setup,
  Active,
  Error,
  Cancelled
};
struct PresentationIdentity
{
  std::uint64_t core = 0;
  std::uint64_t input = 0;
  std::uint64_t attempt = 0;
  std::uint64_t room = 0;
  std::uint32_t game = 0;
  bool operator==(const PresentationIdentity&) const = default;
};
// Host-only copied values. Never serialized into guest status or a savestate.
struct PresentationSnapshot
{
  std::uint64_t revision = 0;
  PresentationIdentity identity;
  EntrySource entry = EntrySource::Desktop;
  PresentationPhase phase = PresentationPhase::Hidden;
  bool cancel = false;
  bool leave = false;
  std::array<char, 512> text{};
  std::array<char, 64> cancel_text{};
  std::array<char, 64> leave_text{};
  bool operator==(const PresentationSnapshot&) const = default;
};
struct PrepaintFence
{
  PresentationIdentity identity;
  std::uint64_t request = 0;
  std::uintptr_t window = 0;
  bool operator==(const PrepaintFence&) const = default;
};
inline std::atomic<std::uint64_t> presentation_core_generation{1};

class PresentationMailbox
{
public:
  bool Publish(const PresentationSnapshot& value,
               std::optional<std::uint64_t> observed_revision = std::nullopt)
  {
    std::lock_guard lock(m_mutex);
    // A network retirement may invalidate while the GUI copies lobby data.
    // Never resurrect that observation after its publication lifetime changed.
    if (observed_revision && *observed_revision != m_revision)
      return false;
    auto copy = value;
    copy.revision = m_value.revision;
    if (m_value == copy)
      return false;
    copy.revision = ++m_revision;
    m_value = copy;
    return true;
  }
  void Invalidate()
  {
    std::lock_guard lock(m_mutex);
    m_value = {};
    m_value.revision = ++m_revision;
  }
  PresentationSnapshot Read(std::uint64_t core, std::uint64_t input) const
  {
    std::lock_guard lock(m_mutex);
    return m_value.identity.core == core && m_value.identity.input == input ?
               m_value :
               PresentationSnapshot{};
  }
  bool IsCurrent(const PresentationSnapshot& value) const
  {
    std::lock_guard lock(m_mutex);
    return value == m_value;
  }
  std::uint64_t Revision() const
  {
    std::lock_guard lock(m_mutex);
    return m_revision;
  }

private:
  mutable std::mutex m_mutex;
  std::uint64_t m_revision = 0;
  PresentationSnapshot m_value;
};
inline PresentationMailbox presentation;

struct LocalCSSObservation
{
  std::uint64_t core = 0;
  std::uint64_t input = 0;
  bool eligible = false;
  std::chrono::steady_clock::time_point observed{};
  bool IsFresh(std::uint64_t current_core, std::uint64_t current_input,
               std::chrono::steady_clock::time_point now) const
  {
    return eligible && core == current_core && input == current_input && now >= observed &&
           now - observed <= std::chrono::milliseconds(500);
  }
};
// The CPU is the only observer of native scene memory. GUI/video copy this bounded value.
class LocalCSSMailbox
{
public:
  void Observe(std::uint64_t core, std::uint64_t input, bool eligible)
  {
    std::lock_guard lock(m_mutex);
    m_value = {core, input, eligible, std::chrono::steady_clock::now()};
  }
  LocalCSSObservation Read() const
  {
    std::lock_guard lock(m_mutex);
    return m_value;
  }

private:
  mutable std::mutex m_mutex;
  LocalCSSObservation m_value;
};
inline LocalCSSMailbox local_css;

// May run on any lifecycle owner: immediately revoke retained render copies and
// pending prepaint continuations before reset/load/boot/stop changes the Core.
inline void InvalidatePresentationCore()
{
  ++presentation_core_generation;
  ++persistent_input_epoch;
  presentation.Invalidate();
}
inline bool CanPresentCancel(bool running, bool eligible, bool dispatch_ready,
                             std::uint64_t attempt)
{
  return running && eligible && dispatch_ready && attempt != 0;
}
}  // namespace ProjectPlusOnline
