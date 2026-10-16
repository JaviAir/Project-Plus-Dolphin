// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <iostream>
#include <vector>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "Common/HttpRequest.h"
#include "Core/Config/NetplaySettings.h"
#include "DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.h"
#include "TestSupport.h"

using NetPlayTest::Require;
using State = QuickPlayMatchmaker::State;
using Result = QuickPlayMatchmaker::Result;

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  Require(argc == 3, "expected scenario and loopback URL");
  const std::string scenario = argv[1];
  const std::string url = argv[2];
  NetPlayTest::Configuration config;
  Config::SetBase(Config::NETPLAY_INDEX_URL, url);
  std::atomic<int> seconds{0};
  QuickPlayIndexMatchmaker matchmaker(nullptr, [&] {
    return std::chrono::steady_clock::now() + std::chrono::seconds(seconds.load());
  });
  std::vector<Result> results;
  const auto call = [&](auto action) {
    const auto count = results.size();
    action();
    NetPlayTest::Wait(
        [&] {
          QCoreApplication::processEvents();
          return results.size() > count;
        },
        std::chrono::seconds(15));
    return results.back();
  };
  const auto status = [&] {
    Common::HttpRequest request;
    const auto response = request.Get(url + "/test/state");
    Require(response.has_value(), "fixture status request failed");
    return QJsonDocument::fromJson(
               QByteArray(reinterpret_cast<const char*>(response->data()), response->size()))
        .object();
  };
  const auto poll = [&] { return call([&] { matchmaker.Poll(); }); };
  const auto initial = call([&] {
    matchmaker.Start({QStringLiteral("us-east")}, QStringLiteral("host"),
                     [&](Result result) { results.push_back(std::move(result)); });
  });
  Require(initial.state == State::Searching, "initial listing did not start");
  const bool validate = scenario.starts_with("validate");
  const bool host_wait = scenario.starts_with("host-wait");
  QString match_id;
  if (validate || host_wait)
  {
    seconds += 5;
    auto matched = poll();
    Require(matched.state == (validate ? State::MatchedHost : State::MatchedClient),
            "fixture did not produce the expected match");
    match_id = matched.match_id;
    Require(!match_id.isEmpty(), "empty match agreement");
  }
  NetPlayTest::Wait([&] { return status().value(QStringLiteral("lost")).toBool(); });
  // Wait until the keepalive callback has been delivered (the server saw the request first).
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  seconds += 1;
  const auto recover = [&] {
    if (host_wait)
      return call([&] { matchmaker.PollMatch(match_id); });
    return poll();
  };
  if (scenario == "waiting-inflight" || scenario == "validate-cancel")
  {
    const auto count = results.size();
    matchmaker.Poll();
    NetPlayTest::Wait([&] { return status().value(QStringLiteral("recovery_adds")).toInt() == 1; });
    matchmaker.Cancel();
    QCoreApplication::processEvents();
    Require(results.size() == count, "cancelled recovery delivered a result");
    std::cout << "PASS " << scenario << '\n';
    return 0;
  }
  QTimer deadline;
  if (scenario == "validate-timeout")
  {
    QObject::connect(&deadline, &QTimer::timeout, [&] {
      if (status().value(QStringLiteral("recovery_adds")).toInt() >= 1)
      {
        seconds += 25;
        deadline.stop();
      }
    });
    deadline.start(20);
  }
  auto result = recover();
  if (scenario.ends_with("permanent"))
  {
    Require(result.state == State::Error && result.error.contains(QStringLiteral("DENIED")),
            "permanent restoration rejection was not reported");
    Require(!result.unreachable, "permanent rejection enabled fallback");
  }
  else if (scenario.ends_with("cancel"))
  {
    Require(result.state == State::Searching, "temporary recovery failure ended search");
    Require(status().value(QStringLiteral("recovery_adds")).toInt() == 1,
            "missing initial recovery attempt");
    matchmaker.Cancel();
    seconds += 10;
    matchmaker.Poll();
  }
  else if (scenario.ends_with("timeout"))
  {
    if (validate)
      Require(result.state == State::Error, "validation extended room setup deadline");
    else
    {
      Require(result.state == (host_wait ? State::WaitingForHost : State::Searching),
              "temporary recovery failure ended attempt");
      seconds += host_wait ? 25 : scenario.starts_with("claim") ? 12 : 180;
      result = recover();
      Require(result.state == (host_wait ? State::Error : State::Expired),
              "restoration reset an existing deadline");
    }
  }
  else if (scenario.ends_with("unreachable"))
  {
    for (int i = 0; i < 3 && result.state != State::Error; ++i)
    {
      seconds += 3;
      result = recover();
    }
    Require(result.state == State::Error && result.unreachable,
            "sustained transport failure was not reported");
  }
  else if (validate)
  {
    Require(result.state == State::MatchedHost && result.match_id == match_id,
            "validation lost the original agreement");
    Require(status().value(QStringLiteral("recovery_adds")).toInt() == 2,
            "validation did not restore its listing before succeeding");
  }
  else
  {
    Require(result.state == (host_wait ? State::WaitingForHost : State::Searching),
            "temporary restoration failure discarded the attempt");
    Require(status().value(QStringLiteral("recovery_adds")).toInt() == 1,
            "missing initial recovery attempt");
    // Calling again before the cadence expires must not hit the server.
    recover();
    Require(status().value(QStringLiteral("recovery_adds")).toInt() == 1,
            "recovery retried without a delay");
    seconds += 3;
    result = recover();
    Require(status().value(QStringLiteral("recovery_adds")).toInt() == 2,
            "lost listing was discarded after its first refused restoration");
    Require(result.state == (host_wait ? State::WaitingForHost : State::Searching),
            "restoration changed the phase");
    if (host_wait)
      Require(result.match_id == match_id, "restoration changed the agreement");
  }
  matchmaker.Cancel();
  std::cout << "PASS " << scenario << '\n';
}
