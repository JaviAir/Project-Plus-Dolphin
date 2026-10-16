// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <iostream>
#include <memory>
#include <vector>

#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"
#include "Core/HW/Wiimote.h"
#include "Protocol.h"

using namespace NetPlayTest;
using Mapping = NetPlay::PadMappingArray;

namespace
{
void CheckLoader(const Mapping& gc, const Mapping& wii)
{
  NetPlay::NetSettings settings{};
  settings.pad_map = gc;
  settings.wiimote_map = wii;
  settings.local_player_id = 1;
  Config::Layer layer(Config::LayerType::Netplay);
  ConfigLoaders::GenerateNetPlayConfigLoader(settings)->Load(&layer);
  for (int i = 0; i < 4; ++i)
    Require(layer.Get(Config::GetInfoForWiimoteSource(i)) ==
                (wii[i] ? WiimoteSource::Emulated : WiimoteSource::None),
            "NetPlay config loader enabled an unmapped Wii Remote or disabled a mapped one");
}

void Expect(const std::vector<Peer*>& peers, const Mapping& gc, const Mapping& wii)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do
  {
    bool ready = true;
    for (auto* peer : peers)
    {
      peer->Pump();
      ready &= peer->pid != 0 && peer->gc == gc && peer->wii == wii;
    }
    if (ready)
    {
      CheckLoader(gc, wii);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  for (const auto* peer : peers)
  {
    std::cerr << "player " << int(peer->pid) << " received Wii:";
    for (const auto value : peer->wii)
      std::cerr << ' ' << int(value);
    std::cerr << '\n';
  }
  Require(false, "clients did not receive the expected GC/manual Wii mappings");
}

void Defaults(bool quickplay)
{
  NetPlay::NetPlayServer server(0, false, nullptr, NetPlay::NetTraversalConfig{false, "", 0},
                                quickplay ? 1 : 0);
  Require(server.is_connected, "could not start private NetPlay server");
  Peer first(server.GetPort());
  Expect({&first}, {1, 0, 0, 0}, {});
  // A later admission must also leave Wii ports manual; a one-time clear is insufficient.
  const auto broadcasts = first.wii_broadcasts.size();
  server.SetWiimoteMapping({});
  Wait([&] {
    first.Pump();
    return first.wii_broadcasts.size() > broadcasts;
  });
  Peer second(server.GetPort());
  Expect({&first, &second}, {1, 2, 0, 0}, {});
  if (!quickplay)
  {
    second.Disconnect();
    Wait([&] {
      first.Pump();
      second.Pump();
      return second.disconnected;
    });
    Expect({&first}, {1, 0, 0, 0}, {});
    Peer rejoined(server.GetPort());
    Expect({&first, &rejoined}, {1, 2, 0, 0}, {});
    Peer third(server.GetPort());
    Expect({&first, &rejoined, &third}, {1, 2, 3, 0}, {});
  }
}

void Manual(bool quickplay)
{
  NetPlay::NetPlayServer server(0, false, nullptr, NetPlay::NetTraversalConfig{false, "", 0},
                                quickplay ? 1 : 0);
  Require(server.is_connected, "could not start private NetPlay server");
  Peer first(server.GetPort());
  Expect({&first}, {1, 0, 0, 0}, {});
  // An explicit mapping survives another admission, with the other slots still unassigned.
  server.SetWiimoteMapping({0, 1, 0, 0});
  Expect({&first}, {1, 0, 0, 0}, {0, 1, 0, 0});
  Peer second(server.GetPort());
  Expect({&first, &second}, {1, 2, 0, 0}, {0, 1, 0, 0});
  server.SetWiimoteMapping({2, 1, 0, 0});
  Expect({&first, &second}, {1, 2, 0, 0}, {2, 1, 0, 0});
  Require(!first.wii_broadcasts.empty() && !second.wii_broadcasts.empty(),
          "manual Wii mapping was not broadcast to both clients");
  // Verify explicit clearing reaches both clients too, then restore for disconnect cleanup.
  server.SetWiimoteMapping({});
  Expect({&first, &second}, {1, 2, 0, 0}, {});
  server.SetWiimoteMapping({2, 1, 0, 0});
  Expect({&first, &second}, {1, 2, 0, 0}, {2, 1, 0, 0});
  second.Disconnect();
  Wait([&] {
    first.Pump();
    second.Pump();
    return second.disconnected;
  });
  Expect({&first}, {1, 0, 0, 0}, {0, 1, 0, 0});
  if (!quickplay)
  {
    Peer rejoined(server.GetPort());
    Expect({&first, &rejoined}, {1, 2, 0, 0}, {0, 1, 0, 0});
  }
  else
  {
    // Preserve Quick Play's existing one-opponent admission policy after disconnect.
    Peer late(server.GetPort());
    Wait([&] {
      first.Pump();
      late.Pump();
      return late.rejection.has_value();
    });
    Require(late.rejection == NetPlay::ConnectionError::ServerFull,
            "controller fix changed Quick Play's admission policy");
  }
}
}  // namespace

int main()
{
  Configuration config;
  ConfigureServer();
  for (int i = 0; i < 4; ++i)
    Config::SetBase(Config::GetInfoForWiimoteSource(i), WiimoteSource::None);
  for (const bool quickplay : {false, true})
  {
    Defaults(quickplay);
    Manual(quickplay);
    std::cout << "PASS " << (quickplay ? "Quick Play" : "ordinary")
              << " defaults, repeated admissions, manual broadcasts, cleanup, config loader\n";
  }
}
