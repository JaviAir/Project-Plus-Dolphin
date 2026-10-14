// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "Common/WorkQueueThread.h"
#include "DolphinQt/QuickPlay/QuickPlayMatchmaker.h"

// Coordinator backend. GUI-thread API, serialized HTTP worker. No NetPlay or widget dependencies.
class QuickPlayClient final : public QuickPlayMatchmaker
{
public:
  explicit QuickPlayClient(QObject* parent = nullptr);
  ~QuickPlayClient() override;

  // Uses the configured coordinator URL.
  void Start(const QStringList& regions, const QString& host_preference,
             Callback callback) override;
  // A valid coordinator URL is set (there is no default), so it can take a search.
  static bool HasConfiguredCoordinator();
  // Shared by settings and admission. Returns a user-facing error, or an empty string.
  static QString NormalizeCoordinatorUrl(const QString& input, QString* normalized);
  struct Region
  {
    QString value;
    QString label;
  };
  static std::array<Region, 11> GetRegions();
  static QString GetRegionLabel(const QString& region);
  static QString NormalizeRegion(const QString& region);
  static bool IsValidRegion(const QString& region);
  static constexpr qsizetype MAX_REGIONS = 12;
  // Normalized, valid, distinct tokens in order, at most MAX_REGIONS.
  static QStringList ParseRegions(const QString& comma_separated);
  // Saved regions, else the legacy single region when none are saved yet, else the default.
  static QStringList LoadRegions();
  // "host", "none" or "join"; anything else is "none".
  static QString NormalizeHostPreference(const QString& preference);
  static QString LoadHostPreference();
  void Poll() override;
  void PollMatch(const QString& match_id) override;
  static Result ParseMatchResponse(int status, const QByteArray& body);
  static bool IsValidHostCode(const QString& code);
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
