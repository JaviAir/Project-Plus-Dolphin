// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <iostream>
#include <memory>
#include <string>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "Core/Config/NetplaySettings.h"
#include "DolphinQt/QuickPlay/QuickPlayClient.h"
#include "DolphinQt/QuickPlay/QuickPlaySettingsDialog.h"
#include "TestSupport.h"

using NetPlayTest::Require;

namespace
{
struct Stored
{
  std::string url = "https://old-coordinator.example";
  std::string matchmaking = "lobby";
  int saves = 0;
};

// A persistent in-memory store survives removal/recreation of all configuration layers.
// Save() is called by the real dialog; no user's configuration files are accessed.
class SettingsLoader final : public Config::ConfigLayerLoader
{
public:
  SettingsLoader(std::shared_ptr<Stored> stored, bool command_line = false)
      : ConfigLayerLoader(command_line ? Config::LayerType::CommandLine : Config::LayerType::Base),
        m_stored(std::move(stored)), m_command_line(command_line)
  {
  }
  void Load(Config::Layer* layer) override
  {
    layer->Set(Config::NETPLAY_QUICKPLAY_COORDINATOR,
               m_command_line ? "https://override.example" : m_stored->url);
    if (!m_command_line)
    {
      layer->Set(Config::NETPLAY_QUICKPLAY_MATCHMAKING, m_stored->matchmaking);
      layer->Set(Config::NETPLAY_QUICKPLAY_REGIONS, "us-east");
    }
  }
  void Save(Config::Layer* layer) override
  {
    if (!m_command_line)
    {
      m_stored->url = layer->Get(Config::NETPLAY_QUICKPLAY_COORDINATOR);
      m_stored->matchmaking = layer->Get(Config::NETPLAY_QUICKPLAY_MATCHMAKING);
      ++m_stored->saves;
    }
  }

private:
  std::shared_ptr<Stored> m_stored;
  bool m_command_line;
};

void Check(const QString& input, bool custom, bool override, bool accepted,
           const std::string& expected)
{
  const auto stored = std::make_shared<Stored>();
  Config::Init();
  Config::AddLayer(std::make_unique<SettingsLoader>(stored));
  if (override)
    Config::AddLayer(std::make_unique<SettingsLoader>(stored, true));
  {
    QuickPlaySettingsDialog dialog;
    dialog.Open();
    auto* backend = dialog.findChild<QComboBox*>(QStringLiteral("quickplay_matchmaking"));
    auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("quickplay_coordinator"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    Require(backend && url && buttons, "settings controls missing");
    backend->setCurrentIndex(1);
    url->setText(input);
    backend->setCurrentIndex(custom ? 1 : 0);
    buttons->button(QDialogButtonBox::Save)->click();
    QApplication::processEvents();
    Require((dialog.result() == QDialog::Accepted) == accepted, "unexpected dialog save outcome");
    if (!accepted)
    {
      auto* error = dialog.findChild<QLabel*>(QStringLiteral("quickplay_settings_result"));
      Require(dialog.isVisible() && error && !error->text().isEmpty(),
              "invalid custom coordinator was not explained");
      Require(stored->saves == 0, "invalid settings were persisted");
      Require(Config::GetBase(Config::NETPLAY_QUICKPLAY_COORDINATOR) == stored->url,
              "rejected settings changed base configuration");
    }
    else
    {
      Require(stored->saves == 1 && stored->url == expected,
              "dialog did not persist the explicitly saved coordinator URL");
      Require(Config::GetBase(Config::NETPLAY_QUICKPLAY_COORDINATOR) == expected,
              "base URL was not updated");
      Require(Config::Get(Config::NETPLAY_QUICKPLAY_COORDINATOR) == expected,
              "effective override retained the old coordinator URL");
      Require(QuickPlayClient::HasConfiguredCoordinator() == !expected.empty(),
              "cleared coordinator still enabled fallback");
      dialog.Open();
      Require(url->text().toStdString() == expected, "reopened dialog retained the old URL");
      dialog.reject();
    }
  }
  Config::Shutdown();
  if (accepted)
  {
    Config::Init();
    Config::AddLayer(std::make_unique<SettingsLoader>(stored));
    {
      QuickPlaySettingsDialog reopened;
      reopened.Open();
      auto* url = reopened.findChild<QLineEdit*>(QStringLiteral("quickplay_coordinator"));
      Require(url && url->text().toStdString() == expected,
              "reloaded persisted settings retained the old URL");
      Require(QuickPlayClient::HasConfiguredCoordinator() == !expected.empty(),
              "reloaded settings enabled unwanted fallback");
    }
    Config::Shutdown();
  }
}
}  // namespace

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  Check(QString{}, false, false, true, "");
  Check(QStringLiteral(" \t\n "), false, false, true, "");
  Check(QString{}, false, true, true, "");
  Check(QStringLiteral(" \t "), false, true, true, "");
  Check(QStringLiteral(" https://new-coordinator.example/ "), true, true, true,
        "https://new-coordinator.example");
  Check(QStringLiteral("https://fallback.example"), false, true, true, "https://fallback.example");
  Check(QString{}, true, false, false, "");
  Check(QStringLiteral("  "), true, true, false, "");
  Check(QStringLiteral("invalid URL"), true, false, false, "");
  // Preserve the lobby's existing treatment of unusable nonempty optional entries.
  Check(QStringLiteral("invalid URL"), false, false, true, "https://old-coordinator.example");
  std::cout << "PASS settings: clearing, override, reload, valid saving, custom validation\n";
}
