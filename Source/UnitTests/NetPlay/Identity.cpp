// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <iostream>

#include <QCoreApplication>

#include "Common/HttpRequest.h"
#include "Common/Version.h"
#include "Core/Config/NetplaySettings.h"
#include "DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.h"
#include "TestSupport.h"

int main(int argc, char** argv)
{
  using NetPlayTest::Require;
  using State = QuickPlayMatchmaker::State;
  QCoreApplication app(argc, argv);
  Require(argc == 5, "expected URL, outcome, peer ID, expected key");
  const std::string url = argv[1];
  const bool expect_match = std::string(argv[2]) == "match";
  const std::string peer = argv[3];
  const std::string expected_key = argv[4];
  NetPlayTest::Configuration config;
  Config::SetBase(Config::NETPLAY_INDEX_URL, url);
  std::atomic<int> offset{0};
  QuickPlayIndexMatchmaker matchmaker(nullptr, [&] {
    return std::chrono::steady_clock::now() + std::chrono::seconds(offset.load());
  });
  std::optional<QuickPlayMatchmaker::Result> result;
  const auto wait_result = [&] {
    NetPlayTest::Wait([&] {
      QCoreApplication::processEvents();
      return result.has_value();
    });
  };
  matchmaker.Start({QStringLiteral("us-east")}, QStringLiteral("host"),
                   [&](auto value) { result = std::move(value); });
  wait_result();
  std::cout << "IDENTITY " << Common::GetScmRevGitStr() << ' ' << Common::GetScmDescStr() << ' '
            << Common::GetNetplayDolphinVer() << std::endl;
  std::cout << "READY" << std::endl;
  std::optional<QuickPlayMatchmaker::Result> matched;
  for (int i = 0; i < 16; ++i)
  {
    Require(result->state == State::Searching || result->state == State::MatchedHost ||
                result->state == State::MatchedClient,
            "unexpected matchmaking failure");
    if (result->state != State::Searching)
    {
      Require(expect_match, "different full revisions matched through display-only identity");
      matched = *result;
      break;
    }
    offset += 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    result.reset();
    matchmaker.Poll();
    wait_result();
  }
  Require(matched.has_value() == expect_match, "same full revision failed to match");
  if (matched)
  {
    Common::HttpRequest request;
    auto response =
        request.Get(url + "/test/matched?peer=" + peer + "&id=" + matched->match_id.toStdString() +
                    "&role=" + (matched->state == State::MatchedHost ? "host" : "client"));
    Require(response.has_value(), "could not report match to fixture");
    NetPlayTest::Wait([&] {
      auto state = request.Get(url + "/test/both-matched");
      return state && std::string(state->begin(), state->end()) == "yes";
    });
  }
  // The fixture checks the actual HTTP publishing and filtering values against expected_key.
  Common::HttpRequest request;
  Require(request
              .Get(url + "/test/key?peer=" + peer +
                   "&expected=" + request.EscapeComponent(expected_key))
              .has_value(),
          "could not verify fixture compatibility key");
  matchmaker.Cancel();
  std::cout << "PASS identity " << peer << '\n';
}
