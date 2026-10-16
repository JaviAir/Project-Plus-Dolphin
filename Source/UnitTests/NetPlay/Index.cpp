// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <iostream>
#include <string>

#include "Core/Config/NetplaySettings.h"
#include "TestSupport.h"
#include "UICommon/NetPlayIndex.h"

int main(int argc, char** argv)
{
  using NetPlayTest::Require;
  Require(argc == 3, "expected scenario and loopback URL");
  const std::string scenario = argv[1];
  NetPlayTest::Configuration config;
  Config::SetBase(Config::NETPLAY_INDEX_URL, std::string(argv[2]));
  NetPlaySession session;
  session.name = "lifecycle regression";
  session.method = "traversal";
  session.region = "NA";
  session.server_id = "abcdef";
  session.game_id = "Project+";
  session.port = 2626;
  session.player_count = 1;
  std::atomic<bool> lost{false};
  {
    NetPlayIndex index;
    index.SetErrorCallback([&] {
      if (scenario == "callback-remove")
        index.Remove();
      lost.store(true);
    });
    Require(index.Add(session), "initial add failed");
    if (scenario != "normal" && scenario != "active-readd")
    {
      NetPlayTest::Wait([&] { return lost.load(); });
      Require(!index.HasActiveSession(), "rejected keepalive retained a session");
    }
    if (scenario == "readd" || scenario == "active-readd")
      Require(index.Add(session), "re-add failed");
    if (scenario == "failed-readd")
      Require(!index.Add(session), "refused re-add unexpectedly succeeded");
    if (scenario != "destroy")
    {
      index.Remove();
      index.Remove();
      Require(!index.HasActiveSession(), "remove retained a session");
    }
  }
  std::cout << "PASS " << scenario << '\n';
  return 0;
}
