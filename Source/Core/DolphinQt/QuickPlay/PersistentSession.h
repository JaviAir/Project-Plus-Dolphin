// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
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
  const std::string& Identity() const { return m_identity; }

private:
  QString m_checkpoint;
  std::string m_identity;
};
