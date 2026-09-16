// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QObject>
#include <QString>

#include "Common/WorkQueueThread.h"

// GUI-thread API, serialized HTTP worker. No NetPlay or widget dependencies.
class QuickPlayClient final : public QObject
{
public:
  enum class State
  {
    Searching,
    MatchedHost,
    MatchedClient,
    HostPublished,
    WaitingForHost,
    HostReady,
    Expired,
    Error,
  };
  struct Result
  {
    State state = State::Error;
    QString error;
    // Capabilities: never display or log these values.
    QString ticket;
    QString match_id;
    QString host_code;
  };
  using Callback = std::function<void(Result)>;

  explicit QuickPlayClient(QObject* parent = nullptr);
  ~QuickPlayClient() override;

  void Start(const QString& base_url, const QString& region, Callback callback);
  void Poll();
  void PollMatch(const QString& match_id);
  static Result ParseMatchResponse(int status, const QByteArray& body);
  static bool IsValidHostCode(const QString& code);
  void PublishHostCode(const QString& match_id, const QString& host_code);
  static Result ParseHostResponse(int status, const QByteArray& body);
  void Cancel();

  // Pure schema parser shared by join/poll; transport failures are handled separately.
  static Result ParseResponse(int status, const QByteArray& body, bool joining);

private:
  struct Attempt;
  void Deliver(const std::shared_ptr<Attempt>& attempt, Result result);
  static void DeleteTicket(const std::shared_ptr<Attempt>& attempt);

  std::shared_ptr<Attempt> m_attempt;
  Callback m_callback;
  bool m_pending = false;
  Common::AsyncWorkThread m_worker{"QuickPlay HTTP"};
};
