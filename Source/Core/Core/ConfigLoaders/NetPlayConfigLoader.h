// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

namespace Config
{
class ConfigLayerLoader;
}

namespace NetPlay
{
struct NetSettings;
}

namespace ConfigLoaders
{
std::unique_ptr<Config::ConfigLayerLoader>
GenerateNetPlayConfigLoader(const NetPlay::NetSettings& settings);
// Validate the effective execution/device policy before attaching a live Core.
// `mismatch` names the first differing setting with both values.
bool ValidateRunningNetPlayConfig(const NetPlay::NetSettings& settings,
                                  std::string* mismatch = nullptr);
std::string RunningNetPlayConfigIdentity();
// Retained research wrapper.
bool ResearchValidateRunningNetPlayConfig(const NetPlay::NetSettings& settings);
}
