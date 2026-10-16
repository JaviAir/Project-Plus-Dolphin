// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "Common/ENet.h"
#include "Common/SFMLHelper.h"
#include "Common/Version.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/NetPlayServer.h"
#include "TestSupport.h"

namespace NetPlayTest
{
// A minimal protocol peer, not an emulated controller or a replacement NetPlay server.
class Peer
{
public:
  explicit Peer(u16 port, std::string revision = Common::GetScmRevGitStr(),
                std::string display = Common::GetNetplayDolphinVer())
      : m_revision(std::move(revision)), m_display(std::move(display))
  {
    m_host = enet_host_create(nullptr, 1, NetPlay::CHANNEL_COUNT, 0, 0);
    Require(m_host != nullptr, "could not create loopback client");
    ENetAddress address{};
    Require(enet_address_set_host(&address, "127.0.0.1") == 0, "invalid loopback address");
    address.port = port;
    m_peer = enet_host_connect(m_host, &address, NetPlay::CHANNEL_COUNT, 0);
    Require(m_peer != nullptr, "could not connect loopback client");
  }
  ~Peer() { enet_host_destroy(m_host); }
  Peer(const Peer&) = delete;
  Peer& operator=(const Peer&) = delete;

  void Disconnect()
  {
    enet_peer_disconnect(m_peer, 0);
    enet_host_flush(m_host);
  }

  void Pump()
  {
    ENetEvent event;
    // Do not let a flood of events make an individual test step unbounded.
    for (int count = 0; count < 100 && enet_host_service(m_host, &event, 1) > 0; ++count)
    {
      if (event.type == ENET_EVENT_TYPE_CONNECT)
      {
        sf::Packet packet;
        packet << m_revision << m_display << std::string("fixture");
        Common::ENet::SendPacket(m_peer, packet, NetPlay::DEFAULT_CHANNEL);
        enet_host_flush(m_host);
      }
      if (event.type == ENET_EVENT_TYPE_DISCONNECT)
        disconnected = true;
      if (event.type != ENET_EVENT_TYPE_RECEIVE)
        continue;
      sf::Packet packet;
      packet.append(event.packet->data, event.packet->dataLength);
      const auto size = event.packet->dataLength;
      enet_packet_destroy(event.packet);
      if (!pid && size == 1)
      {
        NetPlay::ConnectionError error;
        Require(static_cast<bool>(packet >> error), "invalid rejection packet");
        rejection = error;
        continue;
      }
      NetPlay::MessageID id;
      Require(static_cast<bool>(packet >> id), "empty server packet");
      if (id == NetPlay::MessageID::ConnectionSuccessful)
        Require(static_cast<bool>(packet >> pid), "missing player ID");
      if (id == NetPlay::MessageID::PadMapping || id == NetPlay::MessageID::WiimoteMapping)
      {
        auto& mapping = id == NetPlay::MessageID::PadMapping ? gc : wii;
        for (auto& value : mapping)
          Require(static_cast<bool>(packet >> value), "truncated mapping packet");
        if (id == NetPlay::MessageID::WiimoteMapping)
          wii_broadcasts.push_back(mapping);
      }
      if (id == NetPlay::MessageID::Ping)
      {
        u32 key;
        Require(static_cast<bool>(packet >> key), "invalid ping");
        sf::Packet pong;
        pong << NetPlay::MessageID::Pong << key;
        Common::ENet::SendPacket(m_peer, pong, NetPlay::DEFAULT_CHANNEL);
        enet_host_flush(m_host);
      }
    }
  }

  NetPlay::PlayerId pid = 0;
  bool disconnected = false;
  std::optional<NetPlay::ConnectionError> rejection;
  NetPlay::PadMappingArray gc{255, 255, 255, 255};
  NetPlay::PadMappingArray wii{255, 255, 255, 255};
  std::vector<NetPlay::PadMappingArray> wii_broadcasts;

private:
  std::string m_revision;
  std::string m_display;
  ENetHost* m_host = nullptr;
  ENetPeer* m_peer = nullptr;
};

inline void ConfigureServer()
{
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);
  Config::SetBase(Config::NETPLAY_ENABLE_QOS, false);
}
}  // namespace NetPlayTest
