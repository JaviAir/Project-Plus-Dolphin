// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <QDialog>

#include "Common/WorkQueueThread.h"

class QCheckBox;
class QComboBox;
class QGridLayout;
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
  QPushButton* m_show_coordinator;
  void AddRegion(const QString& value, const QString& label);

  // Listed regions first, then preserved custom ones.
  std::vector<std::pair<QCheckBox*, QString>> m_regions;
  QGridLayout* m_region_grid;
  std::size_t m_canonical_region_count;
  QComboBox* m_host_preference;
  QCheckBox* m_seamless;
  QLabel* m_result;
  QPushButton* m_test;
  std::shared_ptr<std::atomic<bool>> m_test_cancelled;
  Common::AsyncWorkThread m_worker{"QuickPlay health"};
};
