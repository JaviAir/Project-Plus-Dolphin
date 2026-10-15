// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <QByteArray>
#include <QString>

#include "Common/WorkQueueThread.h"
#include "DolphinQt/QuickPlay/QuickPlayMatchmaker.h"

// Coordinator backend. GUI-thread API, serialized HTTP worker. No NetPlay or widget dependencies.
class QuickPlayClient final : public QuickPlayMatchmaker
{
public:
  explicit QuickPlayClient(QObject* parent = nullptr);
  ~QuickPlayClient() override;

  void Start(const QStringList& regions, const QString& host_preference,
             Callback callback) override;
  // A valid coordinator URL is set (there is no default), so it can take a search.
  static bool HasConfiguredCoordinator();
  // Shared by settings and admission. Returns a user-facing error, or an empty string.
  static QString NormalizeCoordinatorUrl(const QString& input, QString* normalized);
  void Poll() override;
  void PollMatch(const QString& match_id) override;
  static Result ParseMatchResponse(int status, const QByteArray& body);
  void PublishHostCode(const QString& match_id, const QString& host_code) override;
  static Result ParseHostResponse(int status, const QByteArray& body);
  void Cancel() override;

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
