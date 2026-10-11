// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <utility>

#include "Core/ProjectPlusOnline/Presentation.h"

namespace ProjectPlusOnline
{
// GUI-owned action authority, separate from guest status and render text.
struct SetupActionContext
{
  PresentationIdentity identity;
  std::uint64_t revision = 0;
  bool enabled = false;
  bool operator==(const SetupActionContext&) const = default;
};
struct SetupCancelRequest
{
  SetupActionContext context;
  std::uint64_t ticket = 0;
  std::uint64_t reader_epoch = 0;
};

class SetupActionMailbox
{
public:
  using Clock = std::chrono::steady_clock;
  SetupActionContext Publish(PresentationIdentity identity, bool enabled,
                             Clock::time_point now = Clock::now())
  {
    std::lock_guard lock(m_mutex);
    if (identity != m_context.identity || enabled != m_context.enabled)
    {
      m_context = {identity, m_context.revision + 1, enabled};
      m_reader = {};
    }
    m_published = now;
    return m_context;
  }
  void Invalidate()
  {
    std::lock_guard lock(m_mutex);
    m_context.enabled = false;
    ++m_context.revision;
    m_reader = {};
  }
  SetupActionContext Read(Clock::time_point now = Clock::now()) const
  {
    std::lock_guard lock(m_mutex);
    auto result = m_context;
    result.enabled = Fresh(now, m_published) && result.enabled;
    return result;
  }
  void Reader(const SetupActionContext& context, bool available,
              Clock::time_point now = Clock::now())
  {
    std::lock_guard lock(m_mutex);
    if (context.identity != m_context.identity || context.revision != m_context.revision)
      return;
    if (!available || !context.enabled || !Fresh(now, m_reader))
      ++m_reader_epoch;
    m_reader = available && context.enabled ? now : Clock::time_point{};
  }
  bool Available(const SetupActionContext& context, Clock::time_point now = Clock::now()) const
  {
    std::lock_guard lock(m_mutex);
    return AvailableLocked(context, now);
  }
  std::optional<SetupCancelRequest> Queue(const SetupActionContext& context,
                                          Clock::time_point now = Clock::now())
  {
    std::lock_guard lock(m_mutex);
    if (m_pending || !AvailableLocked(context, now))
      return std::nullopt;
    m_pending = ++m_ticket;
    return SetupCancelRequest{context, m_pending, m_reader_epoch};
  }
  bool Complete(const SetupCancelRequest& request, Clock::time_point now = Clock::now())
  {
    std::lock_guard lock(m_mutex);
    if (!request.ticket || m_pending != request.ticket)
      return false;
    m_pending = 0;
    return request.reader_epoch == m_reader_epoch && AvailableLocked(request.context, now);
  }

private:
  static bool Fresh(Clock::time_point now, Clock::time_point then)
  {
    return then != Clock::time_point{} && now >= then &&
           now - then <= std::chrono::milliseconds(250);
  }
  bool AvailableLocked(const SetupActionContext& context, Clock::time_point now) const
  {
    return context == m_context && context.enabled && Fresh(now, m_published) &&
           Fresh(now, m_reader);
  }
  mutable std::mutex m_mutex;
  SetupActionContext m_context;
  Clock::time_point m_published{}, m_reader{};
  std::uint64_t m_pending = 0, m_ticket = 0, m_reader_epoch = 0;
};
inline SetupActionMailbox setup_action;
inline std::atomic<bool> host_ui_input_blocked{false};
// Ownership of a frontend pause survives temporary removal of cancellation availability.
inline std::atomic<bool> persistent_setup_pause{false};

class PausedCancelObserver
{
public:
  bool Poll(SetupActionContext context, bool available, bool b,
            std::chrono::steady_clock::time_point now)
  {
    if (!available || !context.enabled)
    {
      *this = {};
      return false;
    }
    if (context != m_context || now < m_sampled || now - m_sampled > std::chrono::milliseconds(250))
      m_armed = false;
    m_context = context;
    m_sampled = now;
    if (!b)
      m_armed = true;
    if (b && std::exchange(m_armed, false))
      return true;
    return false;
  }

private:
  SetupActionContext m_context;
  std::chrono::steady_clock::time_point m_sampled{};
  bool m_armed = false;
};

// Only the independent local SI branch consumes this. Never shared NetPlay pads.
class CancelReleaseHandoff
{
public:
  void Arm(std::uint64_t core, std::uint64_t input)
  {
    std::lock_guard lock(m_mutex);
    m_core = core;
    m_input = input;
    m_consumed = true;
  }
  bool Consume(std::uint64_t core, std::uint64_t input, bool connected, bool b,
               bool authoritative = true)
  {
    std::lock_guard lock(m_mutex);
    if (m_core != core || m_input != input)
      m_consumed = false;
    if (authoritative && connected && !b)
      m_consumed = false;
    return m_consumed;
  }

private:
  std::mutex m_mutex;
  std::uint64_t m_core = 0, m_input = 0;
  bool m_consumed = false;
};
inline CancelReleaseHandoff cancel_release_handoff;
}  // namespace ProjectPlusOnline
