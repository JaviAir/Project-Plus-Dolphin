// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include <QString>
#include <QStringList>

#include "Common/WorkQueueThread.h"
#include "DolphinQt/QuickPlay/QuickPlayMatchmaker.h"

// Dolphin lobby backend: a rendezvous through the NetPlay index (NetPlay/IndexServer), with no
// server of our own. Searchers exchange short-lived listings whose names carry the handshake:
//   W  waiting:     region mask, host preference, origin digest, own nonce
//   C  claim:       region mask, host preference, origin digest, claimed waiter, own nonce
//   A  accepted:    who hosts, origin digest, waiter, claimer
//   H  host ready:  origin digest, waiter, claimer; server_id is the traversal code, obfuscated
//                   with a key derived from the protocol
// The origin digest is always 00000000: the handoff is ordinary NetPlay.
// A searcher claims a random compatible W, or lists its own W. A waiter accepts one claim, and
// only then do both players leave their local game. Each player has at most one listing.
// GUI-thread API; the state machine and every index request run on one serialized worker.
class QuickPlayIndexMatchmaker final : public QuickPlayMatchmaker
{
public:
  // A monotonic clock can be supplied by bounded integration fixtures.
  using Now = std::function<std::chrono::steady_clock::time_point()>;
  explicit QuickPlayIndexMatchmaker(QObject* parent = nullptr,
                                    Now now = std::chrono::steady_clock::now);
  ~QuickPlayIndexMatchmaker() override;

  void Start(const QStringList& regions, const QString& host_preference,
             Callback callback) override;
  void Poll() override;
  void PollMatch(const QString& match_id) override;
  void PublishHostCode(const QString& match_id, const QString& host_code) override;
  void Cancel() override;

private:
  struct Attempt;
  void Push(std::function<Result(Attempt&)> step);
  void Deliver(const std::shared_ptr<Attempt>& attempt, Result result);

  const Now m_now;
  std::shared_ptr<Attempt> m_attempt;
  Callback m_callback;
  bool m_pending = false;
  Common::AsyncWorkThread m_worker{"QuickPlay lobby"};
};
