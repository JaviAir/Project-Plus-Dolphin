// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>
#include <QUuid>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/TraversalProto.h"
#include "Common/Version.h"

namespace
{
using Codes = Common::HttpRequest::AllowedReturnCodes;
constexpr auto REQUEST_TIMEOUT = std::chrono::milliseconds{5000};
constexpr auto CANCEL_TIMEOUT = std::chrono::milliseconds{3000};
constexpr int QUICKPLAY_PROTOCOL = 1;

bool IsField(const QString& value, qsizetype limit)
{
  return !value.isEmpty() && value.size() <= limit &&
         std::all_of(value.begin(), value.end(),
                     [](QChar c) { return c.unicode() >= 0x21 && c.unicode() <= 0x7e; });
}

bool IsId(const QString& value)
{
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](QChar c) {
           return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                  (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
         });
}

QString Platform()
{
#if defined(_WIN32)
  return QStringLiteral("windows");
#elif defined(__APPLE__)
  return QStringLiteral("macos");
#elif defined(__linux__)
  return QStringLiteral("linux");
#else
  return QStringLiteral("other");
#endif
}

QuickPlayClient::Result TransportError()
{
  return {QuickPlayClient::State::Error,
          QObject::tr("Cannot reach the Quick Play coordinator, or the request timed out. "
                      "Check the server address and connection, then cancel and try again."),
          {},
          {}};
}
}  // namespace

struct QuickPlayClient::Attempt
{
  std::atomic<bool> cancelled{false};
  std::string base_url;     // Immutable after submission.
  QString ticket;           // Accessed only on the serialized worker.
  QString client_match_id;  // GUI thread only; bound to this admission and role.
};

QuickPlayClient::QuickPlayClient(QObject* parent) : QObject(parent)
{
}

QuickPlayClient::~QuickPlayClient()
{
  Cancel();
  // Join before QObject teardown: the worker may still queue Deliver() on this object.
  // Dialog cancellation never waits. Only final destruction drains bounded HTTP/DELETE work.
  m_worker.Shutdown();
}

QuickPlayClient::Result QuickPlayClient::ParseResponse(int status, const QByteArray& body,
                                                       bool joining)
{
  Result result;
  result.error = tr("The Quick Play coordinator returned an invalid or incompatible response. "
                    "Cancel and try again.");
  if (status != (joining ? 201 : 200) && status != 410)
  {
    result.error = tr("Quick Play coordinator request failed (HTTP %1). "
                      "Check the server configuration, then cancel and try again.")
                       .arg(status);
    return result;
  }

  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(body, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return result;
  const auto object = document.object();
  const QString state = object.value(QStringLiteral("state")).toString();
  if (status == 410)
  {
    if (state == QStringLiteral("expired"))
    {
      result.state = State::Expired;
      result.error = tr("The Quick Play queue expired or was cancelled. Cancel and try again.");
    }
    return result;
  }

  if (joining)
  {
    const auto ticket = object.value(QStringLiteral("ticket")).toString();
    if (!IsId(ticket))
      return result;
    // Keep a valid ticket even if the remaining schema is bad, so it can be cancelled.
    result.ticket = ticket;
  }
  if (state == QStringLiteral("searching"))
  {
    if (object.contains(QStringLiteral("match_id")) || object.contains(QStringLiteral("role")))
      return result;
    result.state = State::Searching;
  }
  else if (state == QStringLiteral("matched"))
  {
    const auto match_id = object.value(QStringLiteral("match_id")).toString();
    const auto role = object.value(QStringLiteral("role")).toString();
    if (!IsId(match_id) || (role != QStringLiteral("host") && role != QStringLiteral("client")) ||
        (joining && role != QStringLiteral("client")))
      return result;
    result.match_id = match_id;
    result.state = role == QStringLiteral("host") ? State::MatchedHost : State::MatchedClient;
  }
  else
  {
    return result;
  }
  result.error.clear();
  return result;
}

void QuickPlayClient::Start(const QString& base_url, const QString& region, Callback callback)
{
  Cancel();
  auto attempt = std::make_shared<Attempt>();
  m_attempt = attempt;
  m_callback = std::move(callback);
  m_pending = true;

  const QUrl url(base_url, QUrl::StrictMode);
  const QString build = QString::fromStdString(Common::GetScmRevGitStr());
  const QString version = QString::fromStdString(Common::GetScmDescStr());
  if (!url.isValid() || url.host().isEmpty() ||
      (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
      !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || !IsField(region, 32) ||
      !IsField(build, 128) || !IsField(version, 64))
  {
    Deliver(attempt, {State::Error,
                      tr("Invalid Quick Play configuration or build metadata. Check "
                         "[NetPlay] QuickPlayCoordinator and QuickPlayRegion in Dolphin.ini."),
                      {},
                      {}});
    return;
  }
  attempt->base_url = url.toString(QUrl::FullyEncoded).toStdString();
  while (attempt->base_url.back() == '/')
    attempt->base_url.pop_back();

  const QJsonObject fields{
      {QStringLiteral("protocol"), QUICKPLAY_PROTOCOL},
      {QStringLiteral("build"), build},
      {QStringLiteral("pplus_version"), version},
      {QStringLiteral("region"), region},
      {QStringLiteral("platform"), Platform()},
      {QStringLiteral("player_nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
  const auto body = QJsonDocument(fields).toJson(QJsonDocument::Compact).toStdString();
  INFO_LOG_FMT(NETPLAY, "QuickPlay: queue request started");
  m_worker.Push([this, attempt, body] {
    if (attempt->cancelled.load())
      return;
    // POST is not idempotent. Let an in-flight join finish so Cancel can recover and DELETE its
    // ticket. Never retry admission automatically, even if the response is lost.
    Common::HttpRequest http(REQUEST_TIMEOUT);
    http.SetSensitive(true);
    http.SetTimeout(REQUEST_TIMEOUT);
    auto result = TransportError();
    if (http.IsValid())
    {
      const auto response = http.Post(attempt->base_url + "/v1/queue", std::string_view(body),
                                      {{"Content-Type", "application/json"}}, Codes::All);
      if (response)
      {
        result = ParseResponse(http.GetLastResponseCode(),
                               QByteArray(reinterpret_cast<const char*>(response->data()),
                                          static_cast<qsizetype>(response->size())),
                               true);
        attempt->ticket = result.ticket;
      }
    }
    Deliver(attempt, std::move(result));
  });
}

void QuickPlayClient::Poll()
{
  if (!m_attempt || m_pending || m_attempt->cancelled.load())
    return;
  m_pending = true;
  m_worker.Push([this, attempt = m_attempt] {
    if (attempt->cancelled.load() || attempt->ticket.isEmpty())
      return;
    Common::HttpRequest http(
        REQUEST_TIMEOUT, [attempt](auto, auto, auto, auto) { return !attempt->cancelled.load(); });
    http.SetSensitive(true);
    http.SetTimeout(REQUEST_TIMEOUT);
    auto result = TransportError();
    if (http.IsValid())
    {
      const auto response = http.Get(
          attempt->base_url + "/v1/queue/" + attempt->ticket.toStdString(), {}, Codes::All);
      if (response)
        result = ParseResponse(http.GetLastResponseCode(),
                               QByteArray(reinterpret_cast<const char*>(response->data()),
                                          static_cast<qsizetype>(response->size())),
                               false);
    }
    Deliver(attempt, std::move(result));
  });
}

bool QuickPlayClient::IsValidHostCode(const QString& code)
{
  return !code.isEmpty() && code.size() <= static_cast<qsizetype>(Common::NETPLAY_CODE_SIZE) &&
         std::all_of(code.begin(), code.end(), [](QChar c) {
           const auto u = c.unicode();
           return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9');
         });
}

QuickPlayClient::Result QuickPlayClient::ParseMatchResponse(int status, const QByteArray& body)
{
  if (status == 410)
    return ParseHostResponse(status, body);
  Result result;
  result.error =
      tr("The Quick Play match returned an invalid response (HTTP %1). Cancel and try again.")
          .arg(status);
  if (status != 200)
    return result;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(body, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return result;
  const auto object = document.object();
  const auto state = object.value(QStringLiteral("state")).toString();
  if (state == QStringLiteral("waiting_for_host") && !object.contains(QStringLiteral("host_code")))
    result.state = State::WaitingForHost;
  else if (state == QStringLiteral("ready"))
  {
    const auto code = object.value(QStringLiteral("host_code")).toString();
    if (!IsValidHostCode(code))
      return result;
    result.state = State::HostReady;
    result.host_code = code;
  }
  else
    return result;
  result.error.clear();
  return result;
}

void QuickPlayClient::PollMatch(const QString& match_id)
{
  if (!m_attempt || m_pending || m_attempt->cancelled.load())
    return;
  m_pending = true;
  if (!IsId(match_id) || m_attempt->client_match_id != match_id)
  {
    Deliver(m_attempt, {State::Error, tr("Invalid Quick Play client match."), {}, {}});
    return;
  }
  m_worker.Push([this, attempt = m_attempt, match_id] {
    if (attempt->cancelled.load())
      return;
    auto result = TransportError();
    Common::HttpRequest http(
        REQUEST_TIMEOUT, [attempt](auto, auto, auto, auto) { return !attempt->cancelled.load(); });
    http.SetSensitive(true);
    http.SetTimeout(REQUEST_TIMEOUT);
    if (http.IsValid() && !attempt->ticket.isEmpty())
    {
      const auto response =
          http.Get(attempt->base_url + "/v1/matches/" + match_id.toStdString(),
                   {{"Authorization", "Bearer " + attempt->ticket.toStdString()}}, Codes::All);
      if (response)
        result = ParseMatchResponse(http.GetLastResponseCode(),
                                    QByteArray(reinterpret_cast<const char*>(response->data()),
                                               static_cast<qsizetype>(response->size())));
    }
    result.match_id = match_id;
    Deliver(attempt, std::move(result));
  });
}

QuickPlayClient::Result QuickPlayClient::ParseHostResponse(int status, const QByteArray& body)
{
  if (status == 204 && body.isEmpty())
    return {State::HostPublished, {}, {}, {}};
  if (status == 410)
  {
    auto result = ParseResponse(status, body, false);
    if (result.state == State::Expired)
      result.error = tr("The Quick Play match expired or was cancelled. Cancel and try again.");
    return result;
  }
  return {State::Error,
          tr("Quick Play host publication returned an invalid response (HTTP %1). "
             "Cancel and try again.")
              .arg(status),
          {},
          {}};
}

void QuickPlayClient::PublishHostCode(const QString& match_id, const QString& host_code)
{
  if (!m_attempt || m_pending || m_attempt->cancelled.load())
    return;
  m_pending = true;
  const bool valid_code =
      !host_code.isEmpty() && host_code.size() <= 64 &&
      std::all_of(host_code.begin(), host_code.end(), [](QChar c) {
        const auto u = c.unicode();
        return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9');
      });
  if (!IsId(match_id) || !valid_code)
  {
    Deliver(m_attempt, {State::Error, tr("Invalid Quick Play traversal host code."), {}, {}});
    return;
  }
  m_worker.Push([this, attempt = m_attempt, match_id, host_code] {
    if (attempt->cancelled.load())
      return;
    if (attempt->ticket.isEmpty())
    {
      Deliver(attempt, {State::Error, tr("Missing Quick Play host ticket."), {}, {}});
      return;
    }
    const auto body = QJsonDocument(QJsonObject{{QStringLiteral("ticket"), attempt->ticket},
                                                {QStringLiteral("host_code"), host_code}})
                          .toJson(QJsonDocument::Compact)
                          .toStdString();
    auto result = TransportError();
    // Publication is idempotent for the identical code. Retry once on transport/server failure;
    // never retry admission or change the body, ticket, match, or endpoint.
    for (int retry = 0; retry < 2 && !attempt->cancelled.load(); ++retry)
    {
      Common::HttpRequest http(REQUEST_TIMEOUT, [attempt](auto, auto, auto, auto) {
        return !attempt->cancelled.load();
      });
      http.SetSensitive(true);
      http.SetTimeout(REQUEST_TIMEOUT);
      bool retryable = true;
      result = TransportError();
      if (http.IsValid())
      {
        const auto response =
            http.Post(attempt->base_url + "/v1/matches/" + match_id.toStdString() + "/host",
                      std::string_view(body), {{"Content-Type", "application/json"}}, Codes::All);
        if (response)
        {
          const int status = http.GetLastResponseCode();
          result =
              ParseHostResponse(status, QByteArray(reinterpret_cast<const char*>(response->data()),
                                                   static_cast<qsizetype>(response->size())));
          retryable = status == 408 || status >= 500;
        }
      }
      if (!retryable)
        break;
    }
    Deliver(attempt, std::move(result));
  });
}

void QuickPlayClient::Deliver(const std::shared_ptr<Attempt>& attempt, Result result)
{
  QMetaObject::invokeMethod(
      this,
      [this, attempt, result = std::move(result)]() mutable {
        if (attempt != m_attempt || attempt->cancelled.load())
          return;
        if (result.state == State::MatchedClient)
          attempt->client_match_id = result.match_id;
        m_pending = false;
        // Copy before invoking: callback may call Cancel(), clearing m_callback.
        auto callback = m_callback;
        if (callback)
          callback(std::move(result));
      },
      Qt::QueuedConnection);
}

void QuickPlayClient::DeleteTicket(const std::shared_ptr<Attempt>& attempt)
{
  if (attempt->ticket.isEmpty())
    return;
  Common::HttpRequest http(CANCEL_TIMEOUT);
  http.SetSensitive(true);
  http.SetTimeout(CANCEL_TIMEOUT);
  const auto response =
      http.IsValid() ? http.Delete(attempt->base_url + "/v1/queue/" + attempt->ticket.toStdString(),
                                   {}, Codes::All) :
                       Common::HttpRequest::Response{};
  if (!response || http.GetLastResponseCode() != 204)
    WARN_LOG_FMT(NETPLAY, "QuickPlay: coordinator cancellation failed; ticket will expire");
  attempt->ticket.clear();
}

void QuickPlayClient::Cancel()
{
  auto attempt = std::exchange(m_attempt, nullptr);
  m_callback = {};
  m_pending = false;
  if (!attempt)
    return;
  attempt->cancelled.store(true);
  // Serialized after admission/poll: even a late join result is cleaned up. Pending polls skip
  // HTTP entirely. The old endpoint/ticket remain attached to this attempt across new starts.
  m_worker.Push([attempt] { DeleteTicket(attempt); });
}
