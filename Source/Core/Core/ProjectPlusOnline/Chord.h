// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>

namespace ProjectPlusOnline
{
// Same L+R+D-pad Up search chord and release-to-rearm semantics as the
// guest command adapter. Host-only; an epoch change resets it to disarmed.
class LocalChord
{
public:
  enum class Action
  {
    None,
    Search,
    Cancel
  };
  Action Poll(bool eligible, std::uint16_t buttons, std::uint64_t cancel_attempt)
  {
    if (!eligible)
    {
      *this = {};
      return Action::None;
    }
    const auto chord = buttons & 0x68;
    if (!chord)
      m_search_armed = true;
    const bool b = (buttons & 0x200) != 0;
    if (!b)
    {
      m_cancel_armed = cancel_attempt != 0;
      m_consume_cancel = false;
    }
    if (b && cancel_attempt && m_cancel_armed)
    {
      m_cancel_armed = false;
      m_consume_cancel = true;
      return Action::Cancel;
    }
    if (chord == 0x68 && m_search_armed && !cancel_attempt)
    {
      m_search_armed = false;
      return Action::Search;
    }
    return Action::None;
  }
  bool ConsumesCancel() const { return m_consume_cancel; }

private:
  bool m_search_armed = false;
  bool m_cancel_armed = false;
  bool m_consume_cancel = false;
};

// Session leave: hold digital L + R + D-pad Right for one second. Any other
// D-pad direction, Z, A, B, X, Y or Start restarts the hold. One leave per hold;
// all three components must be released to rearm. A sampling gap or loss of
// eligibility disarms, so a held chord never carries into a new context.
class LeaveChord
{
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::uint16_t CHORD = 0x0062;   // L | R | D-pad Right
  static constexpr std::uint16_t OTHERS = 0x1F1D;  // Left/Down/Up, Z, A, B, X, Y, Start
  static constexpr auto HOLD = std::chrono::milliseconds(1000);
  static constexpr auto MAX_GAP = std::chrono::milliseconds(250);

  bool Poll(bool eligible, std::uint16_t buttons, Clock::time_point now)
  {
    if (!eligible)
    {
      *this = {};
      return false;
    }
    if (m_sampled != Clock::time_point{} && (now < m_sampled || now - m_sampled > MAX_GAP))
    {
      m_armed = false;
      m_hold_start = {};
    }
    m_sampled = now;
    if (!(buttons & CHORD))
    {
      m_armed = true;
      m_hold_start = {};
      return false;
    }
    if (!m_armed || (buttons & CHORD) != CHORD || (buttons & OTHERS))
    {
      m_hold_start = {};
      return false;
    }
    if (m_hold_start == Clock::time_point{})
      m_hold_start = now;
    if (now - m_hold_start < HOLD)
      return false;
    m_armed = false;
    m_hold_start = {};
    return true;
  }

private:
  Clock::time_point m_sampled{};
  Clock::time_point m_hold_start{};
  bool m_armed = false;
};
}  // namespace ProjectPlusOnline
