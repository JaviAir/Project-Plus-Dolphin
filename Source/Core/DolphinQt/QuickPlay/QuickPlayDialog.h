// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QTimer>

#include "DolphinQt/QuickPlay/QuickPlayController.h"

class QLabel;

class QuickPlayDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit QuickPlayDialog(QuickPlayController& controller, QWidget* parent = nullptr);

  void done(int result) override;

private:
  void UpdateState(QuickPlayController::State state);
  void UpdateSearchingText();

  QuickPlayController& m_controller;
  QLabel* m_status;
  QLabel* m_region;
  QTimer m_animation_timer;
  int m_dots = 1;
};
