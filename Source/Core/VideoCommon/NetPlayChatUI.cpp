// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/NetPlayChatUI.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Host.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"

#include <algorithm>

#include <imgui.h>

constexpr float DEFAULT_WINDOW_WIDTH = 330.0f;
constexpr float DEFAULT_WINDOW_HEIGHT = 400.0f;

constexpr size_t MAX_BACKLOG_SIZE = 100;

std::atomic<std::shared_ptr<NetPlayChatUI>> g_netplay_chat_ui;

NetPlayChatUI::NetPlayChatUI(std::function<void(const std::string&)> callback,
                             std::weak_ptr<NetPlay::NetPlayClient> client,
                             std::weak_ptr<NetPlay::NetPlayServer> server, u32 game)
    : m_client(std::move(client)), m_server(std::move(server)), m_game(game),
      m_message_callback{std::move(callback)}
{
}

NetPlayChatUI::~NetPlayChatUI() = default;

void NetPlayChatUI::Display()
{
  const auto client = m_client.lock();
  if (!client)
    return;
  const auto state = client->GetLobbyState();
  if (!state.connected || !state.running || state.current_game != m_game)
    return;

  std::lock_guard lock(m_mutex);
  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextWindowPos(ImVec2(10.0f * scale, 40.0f * scale), ImGuiCond_FirstUseEver);
  const float height = std::max(1.0f, ImGui::GetIO().DisplaySize.y - 40.0f * scale);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(std::min(DEFAULT_WINDOW_WIDTH * scale, ImGui::GetIO().DisplaySize.x),
             std::min(DEFAULT_WINDOW_HEIGHT * scale, height)),
      ImVec2(ImGui::GetIO().DisplaySize.x, height));

  if (!ImGui::Begin("Chat (hide/show with <-/->)", nullptr, ImGuiWindowFlags_None))
  {
    ImGui::End();
    return;
  }

  if (ImGui::CollapsingHeader("NetPlay buffer", ImGuiTreeNodeFlags_DefaultOpen))
    DisplayBuffer();

  ImGui::BeginChild("Scrolling", ImVec2(0, -30 * scale), true, ImGuiWindowFlags_None);
  for (const auto& [text, color] : m_messages)
  {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(color[0], color[1], color[2], 1.0f), "%s", text.c_str());
    ImGui::PopTextWrapPos();
  }

  if (m_scroll_to_bottom)
  {
    ImGui::SetScrollHereY(1.0f);
    m_scroll_to_bottom = false;
  }

  m_is_scrolled_to_bottom = ImGui::GetScrollY() == ImGui::GetScrollMaxY();

  ImGui::EndChild();

  ImGui::Spacing();

  ImGui::PushItemWidth(-50.0f * scale);

  if (ImGui::InputText("##NetplayMessageBuffer", m_message_buf, IM_ARRAYSIZE(m_message_buf),
                       ImGuiInputTextFlags_EnterReturnsTrue))
  {
    SendChatMessage();
  }

  if (m_activate)
  {
    if (ImGui::IsItemActive())
      ImGui::SetWindowFocus(nullptr);
    else
      ImGui::SetKeyboardFocusHere(-1);
    m_activate = false;
  }

  ImGui::PopItemWidth();

  ImGui::SameLine();

  if (ImGui::Button("Send"))
    SendChatMessage();

  ImGui::End();
}

void NetPlayChatUI::AppendChat(std::string message, Color color)
{
  std::lock_guard lock(m_mutex);
  if (m_messages.size() > MAX_BACKLOG_SIZE)
    m_messages.pop_front();

  m_messages.emplace_back(std::move(message), color);

  // Only scroll to bottom, if we were at the bottom previously
  if (m_is_scrolled_to_bottom)
    m_scroll_to_bottom = true;
}

void NetPlayChatUI::SendChatMessage()
{
  // Check whether the input field is empty
  if (m_message_buf[0] != '\0')
  {
    if (m_message_callback)
      m_message_callback(m_message_buf);

    // 'Empty' the buffer
    m_message_buf[0] = '\0';
  }
}

void NetPlayChatUI::Activate()
{
  std::lock_guard lock(m_mutex);
  m_activate = true;
}

void NetPlayChatUI::Collapse()
{
  Config::SetCurrent(Config::GFX_SHOW_NETPLAY_MESSAGES, false);
}

void NetPlayChatUI::Expand()
{
  Config::SetCurrent(Config::GFX_SHOW_NETPLAY_MESSAGES, true);
}

void NetPlayChatUI::DisplayBuffer()
{
  const auto client = m_client.lock();
  if (!client)
    return;
  const auto state = client->GetLobbyState();
  if (!state.connected || !state.running || state.current_game != m_game ||
      state.host_input_authority ||
      std::any_of(state.wiimotes.begin(), state.wiimotes.end(), [](auto id) { return id > 0; }))
    return;

  const auto server = m_server.lock();
  const u32 minimum = server ? server->GetMinimumPadBufferSize() : state.minimum_buffer;
  const auto player_for = [&state](auto id) {
    return std::find_if(state.players.begin(), state.players.end(),
                        [id](const auto& player) { return player.pid == id; });
  };
  const auto local = player_for(state.local_player);
  const auto mapped =
      std::count_if(state.pads.begin(), state.pads.end(), [](auto id) { return id > 0; });
  const auto local_pads = std::count(state.pads.begin(), state.pads.end(), state.local_player);
  const bool known = std::all_of(state.pads.begin(), state.pads.end(), [&](auto id) {
    return id <= 0 || player_for(id) != state.players.end();
  });
  // Pn denotes the in-game controller slot, never a network player ID or host role.
  const bool singles = mapped == 2 && local_pads == 1 && known;

  ImGui::TextUnformatted("NETPLAY");
  if (singles && local != state.players.end())
  {
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "YOUR BUFFER  %u",
                       std::max(state.minimum_buffer, local->buffer));
  }
  ImGui::TextUnformatted("BUFFER");
  ImGui::Text("MIN BUFFER  %u", minimum);
  if (server)
  {
    // Uses the same ImGui input forwarded by RenderWidget as chat. Background Input
    // permits guest input, not host-authoritative UI edits in an unfocused renderer.
    const bool focused = Host_RendererHasFocus();
    ImGui::SameLine();
    ImGui::BeginDisabled(!focused || minimum == 0);
    if (ImGui::SmallButton("-##Minimum") && Host_RendererHasFocus())
      server->AdjustMinimumPadBufferSizeIfRunning(minimum - 1, m_game);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!focused || minimum >= 99);
    if (ImGui::SmallButton("+##Minimum") && Host_RendererHasFocus())
      server->AdjustMinimumPadBufferSizeIfRunning(minimum + 1, m_game);
    ImGui::EndDisabled();
  }
  for (size_t pad = 0; pad < state.pads.size(); ++pad)
  {
    const auto owner = state.pads[pad];
    if (owner <= 0)
      continue;
    const auto player = player_for(owner);
    if (singles)
      ImGui::Text("%-10s  %u", owner == state.local_player ? "YOU" : "OPPONENT", player->buffer);
    else if (player != state.players.end())
      ImGui::Text("P%zu         %u", pad + 1, player->buffer);
    else
      ImGui::Text("P%zu         --", pad + 1);
  }
  if (!server)
    ImGui::TextDisabled("HOST CONTROLS MIN BUFFER");
  ImGui::TextDisabled("Targets in input polls; not ping");
  ImGui::TextDisabled("Target = max(MIN, player)");
  ImGui::TextDisabled("Lower MIN retains player buffers");
  ImGui::Separator();
}
