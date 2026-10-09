// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include "Common/CommonTypes.h"

namespace NetPlay
{
class NetPlayClient;
class NetPlayServer;
}  // namespace NetPlay

class NetPlayChatUI
{
public:
  NetPlayChatUI(std::function<void(const std::string&)> callback,
                std::weak_ptr<NetPlay::NetPlayClient> client,
                std::weak_ptr<NetPlay::NetPlayServer> server, u32 game);
  ~NetPlayChatUI();

  using Color = std::array<float, 3>;

  void Display();
  void AppendChat(std::string message, Color color);
  void Activate();
  void Collapse();
  void Expand();

private:
  void SendChatMessage();
  void DisplayBuffer();
  std::weak_ptr<NetPlay::NetPlayClient> m_client;
  std::weak_ptr<NetPlay::NetPlayServer> m_server;
  const u32 m_game;
  std::mutex m_mutex;
  char m_message_buf[256] = {};
  bool m_scroll_to_bottom = false;
  bool m_activate = false;
  bool m_is_scrolled_to_bottom = true;

  std::deque<std::pair<std::string, Color>> m_messages;
  std::function<void(const std::string&)> m_message_callback;
};

extern std::atomic<std::shared_ptr<NetPlayChatUI>> g_netplay_chat_ui;
