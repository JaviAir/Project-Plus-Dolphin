// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include "Common/HttpRequest.h"
#include "Core/Config/NetplaySettings.h"
#include "DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.h"
#include "TestSupport.h"

using NetPlayTest::Require;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using State = QuickPlayMatchmaker::State;

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  Require(argc == 3, "expected scenario and loopback URL");
  const std::string scenario = argv[1];
  const std::string url = argv[2];
  NetPlayTest::Configuration config;
  Config::SetBase(Config::NETPLAY_INDEX_URL, url);
  auto matchmaker = std::make_unique<QuickPlayIndexMatchmaker>();
  std::vector<QuickPlayMatchmaker::Result> results;
  const auto start = [&] {
    matchmaker->Start({QStringLiteral("us-east")}, QStringLiteral("host"),
                      [&](auto result) { results.push_back(std::move(result)); });
  };
  const auto wait_result = [&] {
    NetPlayTest::Wait([&] {
      QCoreApplication::processEvents();
      return !results.empty();
    });
  };
  const auto began = Clock::now();
  start();
  if (scenario.ends_with("timeout") || scenario == "add-rejected")
  {
    wait_result();
    Require(results.back().state == State::Error, "request failure was not reported");
    Require(results.back().unreachable == (scenario != "add-rejected"),
            "failure classification changed");
    if (scenario.ends_with("timeout"))
      Require(Clock::now() - began >= 2s && Clock::now() - began < 5s,
              "stream did not reach the finite request deadline");
  }
  else
  {
    // Synchronize on the fixture entering the actual HTTP operation, not a guessed sleep.
    NetPlayTest::Wait([&] {
      Common::HttpRequest request;
      request.SetTimeout(1s);
      auto response = request.Get(url + "/test/state");
      Require(response.has_value(), "fixture state unavailable");
      return QJsonDocument::fromJson(
                 QByteArray(reinterpret_cast<const char*>(response->data()), response->size()))
          .object()
          .value(QStringLiteral("fault_started"))
          .toBool();
    });
    const auto cancelled = Clock::now();
    matchmaker->Cancel();
    if (scenario == "retry")
    {
      start();
      wait_result();
      Require(results.size() == 1 && results.back().state == State::Searching,
              "retry failed or cancelled attempt delivered a stale result");
      Require(Clock::now() - cancelled < 2s, "cancelled poll blocked retry");
    }
    matchmaker.reset();
    // Streaming list/keepalive must observe cancellation; removal uses its full deadline.
    const auto bound = scenario == "remove-stream" ? 5s : 2s;
    Require(Clock::now() - cancelled < bound, "destruction exceeded cancellation bound");
    QCoreApplication::processEvents();
    if (scenario != "retry")
      Require(results.empty(), "cancelled attempt delivered a stale result");
  }
  matchmaker.reset();
  std::cout << "PASS " << scenario << '\n';
}
