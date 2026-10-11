// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
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
}  // namespace ProjectPlusOnline
