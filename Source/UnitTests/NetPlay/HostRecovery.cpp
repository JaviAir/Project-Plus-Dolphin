// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <iostream>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "Common/HttpRequest.h"
#include "Core/Config/NetplaySettings.h"
#include "DolphinQt/QuickPlay/QuickPlayController.h"
#include "TestSupport.h"

using NetPlayTest::Require;
using State = QuickPlayController::State;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  Require(argc == 3, "expected scenario and private index URL");
  const std::string scenario = argv[1];
  const std::string url = argv[2];
  NetPlayTest::Configuration config;
  Config::SetBase(Config::NETPLAY_INDEX_URL, url);
  Config::SetBase(Config::NETPLAY_QUICKPLAY_MATCHMAKING, std::string("lobby"));
  Config::SetBase(Config::NETPLAY_QUICKPLAY_REGIONS, std::string("us-east"));
  Config::SetBase(Config::NETPLAY_QUICKPLAY_HOST_PREFERENCE, std::string("host"));
  // A post-match failure must never fall back to this coordinator.
  Config::SetBase(Config::NETPLAY_QUICKPLAY_COORDINATOR, url);

  quint64 host_attempt = 0;
  int host_calls = 0;
  bool published = false;
  bool finished = false;
  Clock::time_point matched;
  QuickPlayController* controller_ptr = nullptr;
  QuickPlayController controller(
      [&](auto attempt) {
        host_attempt = attempt;
        ++host_calls;
        controller_ptr->OnHostTraversalChanged(attempt, QStringLiteral("ABC123"), false);
        return QString{};
      },
      [](auto) {},
      [](const QString&) {
        Require(false, "host unexpectedly joined another room");
        return QString{};
      });
  controller_ptr = &controller;
  QObject::connect(&controller, &QuickPlayController::PrepareHandoff, [&](auto attempt) {
    matched = Clock::now();
    controller.OnHandoffReady(attempt);
  });

  const auto finish = [&](State expected) {
    Require(!finished, "attempt completed twice");
    finished = true;
    // Let any in-flight restoration and queued cleanup finish. Late results must not
    // overwrite cancellation or take ownership back from ordinary NetPlay.
    QTimer::singleShot(1500, &app, [&, expected] {
      Require(controller.GetState() == expected, "late maintenance changed terminal state");
      Require(host_calls == 1, "recovery recreated the NetPlay room");
      std::cout << "PASS " << scenario << '\n';
      app.quit();
    });
  };
  QObject::connect(&controller, &QuickPlayController::StateChanged, [&](auto state) {
    if (state == State::WaitingForOpponent)
      published = true;
    if (state == State::NetPlayConnected)
      controller.OnLaunchOwned(host_attempt);
    if (state != State::Error)
      return;
    Require(published, "failed before publishing the room");
    if (scenario == "permanent")
      Require(controller.GetErrorMessage().contains(QStringLiteral("DENIED")),
              "permanent restoration rejection was not delivered");
    else if (scenario == "timeout")
    {
      Require(controller.GetErrorMessage().contains(QStringLiteral("timed out")),
              "wrong error for setup deadline");
      const auto elapsed = Clock::now() - matched;
      Require(elapsed >= std::chrono::seconds(24) && elapsed < std::chrono::seconds(28),
              "host recovery renewed the setup deadline");
    }
    else
      Require(false, "recoverable host listing loss ended the attempt");
    finish(State::Error);
  });

  QTimer observe;
  QObject::connect(&observe, &QTimer::timeout, [&] {
    if (!published || finished)
      return;
    Common::HttpRequest request;
    request.SetTimeout(std::chrono::seconds(1));
    const auto response = request.Get(url + "/test/state");
    Require(response.has_value(), "fixture status unavailable");
    const auto status =
        QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(response->data()),
                                           static_cast<qsizetype>(response->size())))
            .object();
    const int restores = status.value(QStringLiteral("restores")).toInt();
    if (scenario == "recover" && status.value(QStringLiteral("restored")).toBool())
    {
      Require(restores == 2, "capacity rejection did not retry exactly once");
      controller.OnHostOpponentConnected(host_attempt);
      finish(State::NetPlayOwned);
    }
    else if (restores == 1 && scenario == "cancel-inflight")
    {
      controller.Cancel();
      finish(State::Idle);
    }
    else if (restores == 1 && scenario == "connect-inflight")
    {
      controller.OnHostOpponentConnected(host_attempt);
      finish(State::NetPlayOwned);
    }
  });
  observe.start(100);
  QTimer::singleShot(scenario == "timeout" ? 40000 : 22000, &app, [] {
    Require(false, "host recovery did not complete within fixture deadline");
  });
  Require(controller.Start(), "controller did not start");
  return app.exec();
}
