// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

#include <QObject>
#include <QString>
#include <QStringList>

// One Quick Play rendezvous backend. GUI-thread API; every call answers with at most one
// queued Result per request, never synchronously. QuickPlayController drives the sequence:
// Start, Poll until matched, Poll once more after the handoff (validation), then the host
// publishes its traversal code or the client polls the match until the code is ready.
class QuickPlayMatchmaker : public QObject
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
  // A search phase that changes what the player can be matched as, shown while it lasts.
  enum class SearchStage
  {
    Normal,
    // Prefer to join: only claims waiting players, and lists itself after stage_seconds.
    JoinOnly,
    // Prefer to join, after JoinOnly found nobody: listed too, and hosts only for an opponent
    // who also prefers to join.
    JoinListed,
  };
  struct Result
  {
    State state = State::Error;
    QString error;
    // Capabilities: never display or log these values.
    QString ticket;
    QString match_id;
    QString host_code;
    // Agreed region of a match; displayable.
    QString region;
    // Searching only: a displayable status note, e.g. why this player is not listed yet.
    QString notice;
    // Searching only.
    SearchStage stage = SearchStage::Normal;
    int stage_seconds = 0;
    // Error only: the backend itself could not be reached, so another backend may be tried.
    bool unreachable = false;
  };
  using Callback = std::function<void(Result)>;

  using QObject::QObject;

  // `origin_digest` is the seamless origin to match on, empty for an ordinary search.
  virtual void Start(const QStringList& regions, const QString& host_preference,
                     const std::string& origin_digest, Callback callback) = 0;
  virtual void Poll() = 0;
  virtual void PollMatch(const QString& match_id) = 0;
  virtual void PublishHostCode(const QString& match_id, const QString& host_code) = 0;
  // Releases the rendezvous (ticket or listing) without waiting. Drops any queued result.
  virtual void Cancel() = 0;
};
