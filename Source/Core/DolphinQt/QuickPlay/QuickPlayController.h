// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

#include <QObject>
#include <QString>
#include <QTimer>

#include "DolphinQt/QuickPlay/QuickPlayClient.h"

// Orchestration and callbacks run on the GUI thread; the client owns HTTP work.
class QuickPlayController final : public QObject
{
  Q_OBJECT

public:
  enum class State
  {
    Idle,
    Searching,
    MatchedHost,
    MatchedClient,
    CreatingHost,
    WaitingForTraversalCode,
    PublishingHostCode,
    WaitingForOpponent,
    WaitingForHost,
    Connecting,
    NetPlayConnected,
    Error,
  };
  Q_ENUM(State)

  using StartHost = std::function<QString(std::uint64_t)>;
  using CancelHost = std::function<void(std::uint64_t)>;
  using JoinHost = std::function<bool(const QString&)>;
  QuickPlayController(StartHost start_host, CancelHost cancel_host, JoinHost join_host,
                      QObject* parent = nullptr);
  ~QuickPlayController() override;

  State GetState() const { return m_state; }
  const QString& GetRegion() const { return m_region; }
  const QString& GetErrorMessage() const { return m_error_message; }
  bool Start();
  void Cancel();
  void OnHostTraversalChanged(quint64 attempt, const QString& code, bool failed);
  void OnHostOpponentConnected(quint64 attempt);
  void OnHostClosed(std::uint64_t attempt);

signals:
  void StateChanged(QuickPlayController::State state);

private:
  void SetState(State state);
  void HandleResult(QuickPlayClient::Result result);

  void BeginHost(std::uint64_t attempt);
  void BeginJoin(std::uint64_t attempt);
  void Fail(QString error);
  void Complete();
  StartHost m_start_host;
  CancelHost m_cancel_host;
  JoinHost m_join_host;
  bool m_joining = false;
  QString m_host_code;
  QString m_match_id;
  bool m_creating_host = false;
  QTimer m_setup_timer;

  QString m_region;
  QString m_error_message;
  State m_state = State::Idle;
  std::uint64_t m_attempt = 0;
  bool m_entered_queue = false;
  QTimer m_poll_timer;
  QuickPlayClient m_client;
};
