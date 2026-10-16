// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include "Common/HttpRequest.h"
#include "Common/Logging/LogManager.h"
#include "Core/Config/NetplaySettings.h"
#include "DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.h"
#include "TestSupport.h"

using NetPlayTest::Require;
using State = QuickPlayMatchmaker::State;
using Result = QuickPlayMatchmaker::Result;

class Messages final : public Common::Log::LogListener
{
public:
  void Log(Common::Log::LogLevel, const char* message) override
  {
    std::lock_guard guard(m_mutex);
    m_messages += message;
  }
  bool Contains(const std::string& text)
  {
    std::lock_guard guard(m_mutex);
    return m_messages.find(text) != std::string::npos;
  }

private:
  std::mutex m_mutex;
  std::string m_messages;
};

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  Require(argc == 3, "expected scenario and loopback URL");
  const std::string scenario = argv[1];
  const std::string url = argv[2];
  NetPlayTest::Configuration config;
  Config::SetBase(Config::NETPLAY_INDEX_URL, url);
  Common::Log::LogManager::Init();
  auto* logger = Common::Log::LogManager::GetInstance();
  auto messages = std::make_unique<Messages>();
  auto* captured = messages.get();
  logger->RegisterListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, std::move(messages));
  logger->EnableListener(Common::Log::LogListener::CONSOLE_LISTENER, false);
  logger->EnableListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, true);
  logger->SetConfigLogLevel(Common::Log::LogLevel::LINFO);
  logger->SetEnable(Common::Log::LogType::NETPLAY, true);
  {
    std::atomic<int> seconds{0};
    QuickPlayIndexMatchmaker matchmaker(nullptr, [&] {
      return std::chrono::steady_clock::now() + std::chrono::seconds(seconds.load());
    });
    std::vector<Result> results;
    const auto call = [&](auto action) {
      const auto count = results.size();
      action();
      NetPlayTest::Wait([&] {
        QCoreApplication::processEvents();
        return results.size() > count;
      });
      return results.back();
    };
    const auto poll = [&] {
      seconds += 5;
      return call([&] { matchmaker.Poll(); });
    };
    auto result = call([&] {
      matchmaker.Start({QStringLiteral("us-east")}, QStringLiteral("host"),
                       [&](Result value) { results.push_back(std::move(value)); });
    });
    Require(result.state == State::Searching, "initial admission failed");
    NetPlayTest::Wait([&] {
      Common::HttpRequest request;
      request.SetTimeout(std::chrono::seconds(1));
      auto response = request.Get(url + "/test/state");
      Require(response.has_value(), "fixture state unavailable");
      return QJsonDocument::fromJson(
                 QByteArray(reinterpret_cast<const char*>(response->data()), response->size()))
          .object()
          .value(QStringLiteral("active_waiting"))
          .toBool();
    });
    // Keepalive is pending but has not failed yet. RestoreListing checks its flag before
    // List starts; the fixture then rejects the keepalive during this List request.
    result = poll();
    Require(result.state == State::Searching, "one list failure ended the search");
    Require(captured->Contains("list failed (LIST_DENIED)"), "lost directory error diagnostic");
    result = poll();
    Require(captured->Contains("listing lost (BAD_SESSION)"),
            "directory failure overwrote the keepalive error diagnostic");
    if (scenario == "permanent")
    {
      Require(result.state == State::Error && !result.unreachable &&
                  result.error.contains(QStringLiteral("DENIED")),
              "permanent restoration error classification changed");
    }
    else if (scenario == "transport")
    {
      for (int i = 0; i < 2; ++i)
        result = poll();
      Require(result.state == State::Error && result.unreachable,
              "transport restoration failure did not enable pre-match fallback");
    }
    else
    {
      Require(result.state == State::Searching && !result.notice.isEmpty(),
              "capacity rejection lost its notice");
      result = poll();
      Require(result.state == State::Searching && result.notice.isEmpty(),
              "successful recovery did not clear the notice");
      if (scenario == "list-outage")
      {
        result = poll();
        Require(result.state == State::Error && result.unreachable,
                "consecutive directory failures lost their classification");
      }
    }
    matchmaker.Cancel();
  }
  Common::Log::LogManager::Shutdown();
  std::cout << "PASS " << scenario << '\n';
}
