// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <iostream>

#include "Core/Config/NetplaySettings.h"
#include "Protocol.h"

int main()
{
  using namespace NetPlayTest;
  Configuration config;
  ConfigureServer();
  NetPlay::NetPlayServer server(0, false, nullptr, NetPlay::NetTraversalConfig{false, "", 0});
  Require(server.is_connected, "could not start private NetPlay server");
  Peer linux_peer(server.GetPort(), Common::GetScmRevGitStr(), "same display Lin");
  Peer windows_peer(server.GetPort(), Common::GetScmRevGitStr(), "same display Win");
  Wait([&] {
    linux_peer.Pump();
    windows_peer.Pump();
    return linux_peer.pid && windows_peer.pid;
  });
  Require(linux_peer.pid != windows_peer.pid, "duplicate admitted player IDs");
  Peer wrong_version(server.GetPort(), Common::GetScmRevGitStr() + "-different",
                     "same display Lin");
  Wait([&] {
    linux_peer.Pump();
    windows_peer.Pump();
    wrong_version.Pump();
    return wrong_version.rejection.has_value();
  });
  Require(wrong_version.rejection == NetPlay::ConnectionError::VersionMismatch,
          "ordinary NetPlay no longer rejects differing full revisions");
  std::cout << "PASS ordinary NetPlay exact revision / independent platform metadata\n";
}
