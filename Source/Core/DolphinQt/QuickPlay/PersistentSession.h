// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include "Core/QuickPlayRecipe.h"
#include "DolphinQt/QuickPlay/ContentPolicy.h"
#include <functional>
#include <optional>
#include <string>

namespace Core
{
class System;
}

// GUI-thread owner of one supported same-Core admission. The origin is the CSS state this
// Core generated under the Quick Play recipe at boot; the contract pins the build, recipe
// and effective configuration. It never saves or merges the player's pre-search guest state.
class PersistentQuickPlaySession final
{
public:
  // This Core generated a recipe origin.
  static bool IsConfigured();
  bool Validate(Core::System& system, QString* error);
  void Restore(Core::System& system, std::function<void(bool)> finished);
  // This run's hand-free local CSS (QuickPlayRecipe::LocalSnapshotPath), for leaving a match.
  // Not the shared origin: no digest check. `finished(false)` if it is missing or fails to load.
  static void RestoreLocal(Core::System& system, std::function<void(bool)> finished);
  bool IsTrial() const { return m_policy == QuickPlayContent::Policy::DeclaredBundleTrialV1; }
  const std::optional<QuickPlayContent::TrialLauncher>& TrialLauncher() const { return m_trial_launcher; }
  const std::string& Identity() const { return m_identity; }
  const std::string& ContractText() const { return m_contract_text; }
  const std::string& OriginDigest() const { return m_origin.digest; }

private:
  const QuickPlayContent::Policy m_policy = QuickPlayContent::SelectedPolicy();
  std::optional<QuickPlayContent::TrialLauncher> m_trial_launcher;
  QuickPlayRecipe::Origin m_origin;
  std::string m_identity;
  std::string m_contract_text;
};
