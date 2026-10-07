// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>

#include <QDialog>

#include "Common/WorkQueueThread.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// Retained by its owning window: closing never joins the HTTP worker.
class QuickPlaySettingsDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit QuickPlaySettingsDialog(QWidget* parent = nullptr);
  ~QuickPlaySettingsDialog() override;
  void Open();
  void done(int result) override;

private:
  void Save();
  void TestConnection();
  void InvalidateTest();

  QLineEdit* m_coordinator;
  QComboBox* m_region;
  int m_canonical_region_count;
  QLabel* m_result;
  QPushButton* m_test;
  std::shared_ptr<std::atomic<bool>> m_test_cancelled;
  Common::AsyncWorkThread m_worker{"QuickPlay health"};
};
