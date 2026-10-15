// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <functional>

#include <QObject>
#include <QString>
#include <QTimer>

#include "DolphinQt/QuickPlay/QuickPlayClient.h"
#include "DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.h"

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
    PreparingHandoff,
    WaitingForHandoff,
    ValidatingMatch,
    CreatingHost,
    WaitingForTraversalCode,
    PublishingHostCode,
    WaitingForOpponent,
    WaitingForHost,
    Connecting,
    NetPlayConnected,
    NetPlayOwned,
    Error,
  };
  Q_ENUM(State)
  enum class Backend
  {
    Lobby,
    Coordinator,
  };

  using StartHost = std::function<QString(std::uint64_t)>;
  using CancelHost = std::function<void(std::uint64_t)>;
  using JoinHost = std::function<QString(const QString&)>;
  QuickPlayController(StartHost start_host, CancelHost cancel_host, JoinHost join_host,
                      QObject* parent = nullptr);
  ~QuickPlayController() override;

  State GetState() const { return m_state; }
  // Regions searched by this attempt, and the agreed region once matched.
  const QStringList& GetRegions() const { return m_regions; }
  const QString& GetMatchRegion() const { return m_match_region; }
  // "host", "none" or "join", as sent with this attempt.
  const QString& GetHostPreference() const { return m_host_preference; }
  const QString& GetErrorMessage() const { return m_error_message; }
  // The backend of the current attempt, and its latest displayable search note.
  QString GetBackendLabel() const;
  const QString& GetSearchNotice() const { return m_search_notice; }
  QuickPlayMatchmaker::SearchStage GetSearchStage() const { return m_search_stage; }
  int GetSearchStageSeconds() const { return m_search_stage_seconds; }
  bool Start();
  void Cancel();
  // Called by the owner once the frontend can host or join, or with the reason it can't.
  void OnHandoffReady(std::uint64_t attempt, const QString& error = {});
  bool OwnsAttempt(std::uint64_t attempt) const { return attempt == m_attempt; }
  void OnHostTraversalChanged(quint64 attempt, const QString& code, bool failed);
  void OnHostOpponentConnected(quint64 attempt);
  void OnHostClosed(std::uint64_t attempt);
  void OnLaunchOwned(std::uint64_t attempt);
  void OnLaunchFailed(std::uint64_t attempt, const QString& error);

signals:
  void PrepareHandoff(quint64 attempt);
  void StateChanged(QuickPlayController::State state);

private:
  void SetState(State state);
  void StartBackend(Backend backend);
  void HandleResult(QuickPlayMatchmaker::Result result);

  void ClaimMatch(const QuickPlayMatchmaker::Result& result);
  void BeginHost(std::uint64_t attempt);
  void BeginJoin(std::uint64_t attempt);
  void ConnectHost(std::uint64_t attempt);
  void ConnectJoin(std::uint64_t attempt);
  void Fail(QString error);
  void Complete();
  StartHost m_start_host;
  CancelHost m_cancel_host;
  JoinHost m_join_host;
  bool m_is_host = false;
  bool m_joining = false;
  QString m_host_code;
  QString m_match_id;
  bool m_creating_host = false;
  QTimer m_setup_timer;

  QStringList m_regions;
  QString m_match_region;
  QString m_error_message;
  State m_state = State::Idle;
  std::uint64_t m_attempt = 0;
  bool m_entered_queue = false;
  QTimer m_poll_timer;
  QuickPlayClient m_coordinator;
  QuickPlayIndexMatchmaker m_lobby;
  Backend m_backend = Backend::Lobby;
  QuickPlayMatchmaker* m_matchmaker = &m_lobby;
  QString m_host_preference;
  QString m_search_notice;
  QuickPlayMatchmaker::SearchStage m_search_stage = QuickPlayMatchmaker::SearchStage::Normal;
  int m_search_stage_seconds = 0;
};
