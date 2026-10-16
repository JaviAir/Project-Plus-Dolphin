// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include "Common/Config/Config.h"

namespace NetPlayTest
{
inline void Require(bool condition, const char* message)
{
  if (!condition)
  {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}

template <typename Predicate>
void Wait(Predicate predicate, std::chrono::seconds timeout = std::chrono::seconds(8))
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate())
  {
    Require(std::chrono::steady_clock::now() < deadline, "timed out waiting for assertion");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Never read or write the developer's Dolphin configuration.
class MemoryLoader final : public Config::ConfigLayerLoader
{
public:
  MemoryLoader() : ConfigLayerLoader(Config::LayerType::Base) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

struct Configuration
{
  Configuration()
  {
    Config::Init();
    Config::AddLayer(std::make_unique<MemoryLoader>());
  }
  ~Configuration() { Config::Shutdown(); }
};
}  // namespace NetPlayTest
