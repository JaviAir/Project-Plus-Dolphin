// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include "DolphinQt/QuickPlay/ContentPolicy.h"
#include <functional>
#include <string>

namespace Core
{
class System;
}

// GUI-thread owner of one supported same-Core admission. The bundle pins the
// validated CSS image, effective configuration and external content/storage.
// It never saves or merges the player's pre-search guest state.
class PersistentQuickPlaySession final
{
public:
  static bool IsConfigured();
  bool Validate(Core::System& system, QString* error);
  void Restore(Core::System& system, std::function<void(bool)> finished);
  bool IsTrial() const { return m_policy == QuickPlayContent::Policy::DeclaredBundleTrialV1; }
  const std::optional<QuickPlayContent::TrialLauncher>& TrialLauncher() const { return m_trial_launcher; }
  const std::string& Identity() const { return m_identity; }

private:
  const QuickPlayContent::Policy m_policy = QuickPlayContent::SelectedPolicy();
  std::optional<QuickPlayContent::TrialLauncher> m_trial_launcher;
  QString m_checkpoint;
  std::string m_identity;
};
