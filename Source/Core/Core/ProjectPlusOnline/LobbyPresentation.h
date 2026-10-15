// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace ProjectPlusOnline
{
enum class EntrySource
{
  Desktop,
  InGame
};

// Capture on callback arrival, before queued GUI delivery or teardown can retire its generation.
class SessionFailureMailbox
{
public:
  void Begin(std::uint64_t generation)
  {
    std::lock_guard lock(m_mutex);
    m_generation = generation;
    m_error.clear();
  }
  std::string Capture(std::uint64_t generation, std::string error)
  {
    std::lock_guard lock(m_mutex);
    if (generation != m_generation)
      return {};
    if (m_error.empty())
      m_error = std::move(error);
    return m_error;
  }
  std::string Read(std::uint64_t generation) const
  {
    std::lock_guard lock(m_mutex);
    return generation == m_generation ? m_error : std::string{};
  }

private:
  mutable std::mutex m_mutex;
  std::uint64_t m_generation = 0;
  std::string m_error;
};

// Presentation provenance survives release of setup ownership; visibility grants no authority.
class LobbyPresentation
{
public:
  void Begin(EntrySource entry, bool persistent, std::uint64_t attempt)
  {
    m_hidden = entry == EntrySource::InGame && persistent && attempt != 0;
    m_initialized = false;
    m_error.clear();
  }
  bool Initialize() { return !std::exchange(m_initialized, true); }
  bool Initialized() const { return m_initialized; }
  bool AutomaticShow() const { return !m_hidden; }
  bool InGame() const { return m_hidden; }
  void SetError(std::string error) { m_error = std::move(error); }
  const std::string& DiagnosticError() const { return m_error; }
  void Reset()
  {
    m_hidden = false;
    m_initialized = false;
  }

private:
  std::string m_error;
  bool m_hidden = false;
  bool m_initialized = false;
};

inline bool CanJoinRoom(bool client, bool server, bool initialized, bool creating_local_client)
{
  return !client && !initialized && (!server || creating_local_client);
}
}  // namespace ProjectPlusOnline
