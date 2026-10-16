// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlaySettingsDialog.h"

#include <algorithm>
#include <chrono>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/HttpRequest.h"
#include "Core/Config/NetplaySettings.h"
#include "DolphinQt/QuickPlay/QuickPlayClient.h"

QuickPlaySettingsDialog::QuickPlaySettingsDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Quick Play Settings"));
  setWindowModality(Qt::WindowModal);
  setMinimumWidth(460);
  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(16);
  auto* form = new QFormLayout;
  form->setVerticalSpacing(14);
  m_matchmaking = new QComboBox;
  m_matchmaking->setObjectName(QStringLiteral("quickplay_matchmaking"));
  m_matchmaking->addItem(tr("Dolphin lobby (default)"), QStringLiteral("lobby"));
  m_matchmaking->addItem(tr("Custom coordinator"), QStringLiteral("coordinator"));
  m_matchmaking->setToolTip(
      tr("The Dolphin lobby needs no server of your own: players meet through Dolphin's NetPlay "
         "server browser. A custom coordinator keeps a fair queue and finer region matching."));
  form->addRow(tr("Matchmaking:"), m_matchmaking);
  m_coordinator = new QLineEdit;
  m_coordinator->setObjectName(QStringLiteral("quickplay_coordinator"));
  m_coordinator->setPlaceholderText(QStringLiteral("https://your-coordinator.example"));
  // Hidden by default: a private coordinator URL is effectively an access secret.
  m_coordinator->setEchoMode(QLineEdit::Password);
  m_show_coordinator = new QPushButton(tr("Show"));
  m_show_coordinator->setCheckable(true);
  auto* coordinator_row = new QHBoxLayout;
  coordinator_row->addWidget(m_coordinator);
  coordinator_row->addWidget(m_show_coordinator);
  form->addRow(tr("Coordinator Server:"), coordinator_row);
  m_test = new QPushButton(tr("Test Connection to Coordinator Server"));
  m_test->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  form->addRow(QString(), m_test);
  auto* region_list = new QWidget;
  region_list->setObjectName(QStringLiteral("quickplay_regions"));
  m_region_grid = new QGridLayout(region_list);
  m_region_grid->setContentsMargins(0, 0, 0, 0);
  m_region_grid->setVerticalSpacing(8);
  m_region_grid->setHorizontalSpacing(16);
  for (const auto& region : QuickPlayMatchmaker::GetRegions())
    AddRegion(region.value, region.label);
  m_canonical_region_count = m_regions.size();
  form->addRow(tr("Regions:"), region_list);
  m_host_preference = new QComboBox;
  m_host_preference->setObjectName(QStringLiteral("quickplay_host_preference"));
  m_host_preference->addItem(tr("Prefer to host"), QStringLiteral("host"));
  m_host_preference->addItem(tr("No preference"), QStringLiteral("none"));
  m_host_preference->addItem(tr("Prefer to join"), QStringLiteral("join"));
  m_host_preference->setToolTip(
      tr("Used when it complements your opponent's preference. Otherwise the player who "
         "searched first hosts. It never delays a match."));
  form->addRow(tr("Host Preference:"), m_host_preference);
  layout->addLayout(form);

  m_result = new QLabel;
  m_result->setObjectName(QStringLiteral("quickplay_settings_result"));
  m_result->setTextFormat(Qt::PlainText);
  m_result->setWordWrap(true);
  layout->addWidget(m_result);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &QuickPlaySettingsDialog::Save);
  connect(buttons, &QDialogButtonBox::rejected, this, &QuickPlaySettingsDialog::reject);
  connect(m_test, &QPushButton::clicked, this, &QuickPlaySettingsDialog::TestConnection);
  connect(m_show_coordinator, &QPushButton::toggled, this, [this](bool shown) {
    m_coordinator->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
    m_show_coordinator->setText(shown ? tr("Hide") : tr("Show"));
  });
  connect(m_matchmaking, &QComboBox::currentIndexChanged, this,
          &QuickPlaySettingsDialog::UpdateMatchmaking);
  connect(m_coordinator, &QLineEdit::textChanged, this, [this] {
    InvalidateTest();
    m_result->clear();
  });
}

QuickPlaySettingsDialog::~QuickPlaySettingsDialog()
{
  InvalidateTest();
  // Join before QObject teardown; queued deliveries are removed by QObject destruction.
  // Only final owner destruction waits, never Save/Cancel/Escape/window close.
  m_worker.Shutdown();
}

void QuickPlaySettingsDialog::AddRegion(const QString& value, const QString& label)
{
  auto* box = new QCheckBox(label);
  const int index = static_cast<int>(m_regions.size());
  m_region_grid->addWidget(box, index / 2, index % 2);
  m_regions.emplace_back(box, value);
}

void QuickPlaySettingsDialog::UpdateMatchmaking()
{
  const bool coordinator = m_matchmaking->currentData().toString() == QStringLiteral("coordinator");
  m_coordinator->setEnabled(coordinator);
  m_show_coordinator->setEnabled(coordinator);
  m_test->setEnabled(coordinator);
  InvalidateTest();
  m_result->clear();
}

void QuickPlaySettingsDialog::InvalidateTest()
{
  if (m_test_cancelled)
    m_test_cancelled->store(true);
}

void QuickPlaySettingsDialog::Open()
{
  if (!isVisible())
  {
    m_coordinator->setText(
        QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_COORDINATOR)));
    m_show_coordinator->setChecked(false);
    const int matchmaking = m_matchmaking->findData(
        QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_MATCHMAKING)));
    m_matchmaking->setCurrentIndex(matchmaking < 0 ? 0 : matchmaking);
    UpdateMatchmaking();
    // Remove previous custom entries when reopening after a config change.
    while (m_regions.size() > m_canonical_region_count)
    {
      delete m_regions.back().first;
      m_regions.pop_back();
    }
    const auto saved = QuickPlayMatchmaker::LoadRegions();
    for (const auto& region : saved)
    {
      const auto it = std::find_if(m_regions.begin(), m_regions.end(),
                                   [&](const auto& entry) { return entry.second == region; });
      if (it == m_regions.end())
        AddRegion(region, tr("Custom (preserved): %1").arg(region));
    }
    for (const auto& [box, value] : m_regions)
      box->setChecked(saved.contains(value));
    m_host_preference->setCurrentIndex(
        m_host_preference->findData(QuickPlayMatchmaker::LoadHostPreference()));
    m_result->clear();
  }
  show();
  raise();
  activateWindow();
}

void QuickPlaySettingsDialog::done(int result)
{
  InvalidateTest();
  QDialog::done(result);
}

void QuickPlaySettingsDialog::Save()
{
  const auto matchmaking = m_matchmaking->currentData().toString().toStdString();
  QString normalized;
  // In lobby mode an explicitly blank optional URL disables coordinator fallback. Keep
  // normalization's nonempty requirement for custom-coordinator mode.
  const bool clear_optional = matchmaking == "lobby" && m_coordinator->text().trimmed().isEmpty();
  const auto error = clear_optional ? QString{} :
                                     QuickPlayClient::NormalizeCoordinatorUrl(m_coordinator->text(),
                                                                              &normalized);
  // The lobby ignores the coordinator field; an unusable entry there is left unsaved.
  if (!error.isEmpty() && matchmaking == "coordinator")
  {
    m_result->setText(m_coordinator->text().trimmed().isEmpty() ?
                          tr("Custom coordinator needs a coordinator URL.") :
                          error);
    m_coordinator->setFocus();
    return;
  }
  QStringList regions;
  for (const auto& [box, value] : m_regions)
  {
    if (box->isChecked())
      regions.append(value);
  }
  if (regions.isEmpty())
  {
    m_result->setText(tr("Choose at least one region."));
    return;
  }
  if (regions.size() > QuickPlayMatchmaker::MAX_REGIONS)
  {
    m_result->setText(tr("Choose at most %1 regions.").arg(QuickPlayMatchmaker::MAX_REGIONS));
    return;
  }
  const auto joined = regions.join(QLatin1Char(',')).toStdString();
  const auto host_preference = m_host_preference->currentData().toString().toStdString();

  // Match Dolphin's effective-layer convention, and persist even if a command-line override
  // supplied the initial value. Saving explicitly replaces that override for this run too.
  Config::SetBaseOrCurrent(Config::NETPLAY_QUICKPLAY_MATCHMAKING, matchmaking);
  Config::SetBase(Config::NETPLAY_QUICKPLAY_MATCHMAKING, matchmaking);
  if (error.isEmpty())
  {
    Config::SetBaseOrCurrent(Config::NETPLAY_QUICKPLAY_COORDINATOR, normalized.toStdString());
    Config::SetBase(Config::NETPLAY_QUICKPLAY_COORDINATOR, normalized.toStdString());
  }
  Config::SetBaseOrCurrent(Config::NETPLAY_QUICKPLAY_REGIONS, joined);
  Config::SetBase(Config::NETPLAY_QUICKPLAY_REGIONS, joined);
  // Migrated into QuickPlayRegions above.
  Config::DeleteKey(Config::LayerType::Base, Config::NETPLAY_QUICKPLAY_REGION);
  Config::SetBaseOrCurrent(Config::NETPLAY_QUICKPLAY_HOST_PREFERENCE, host_preference);
  Config::SetBase(Config::NETPLAY_QUICKPLAY_HOST_PREFERENCE, host_preference);
  Config::Save();
  accept();
}

void QuickPlaySettingsDialog::TestConnection()
{
  QString normalized;
  const auto error = QuickPlayClient::NormalizeCoordinatorUrl(m_coordinator->text(), &normalized);
  if (!error.isEmpty())
  {
    m_result->setText(error);
    return;
  }
  m_test->setEnabled(false);
  m_result->setText(tr("Testing connection…"));
  const auto cancelled = std::make_shared<std::atomic<bool>>(false);
  m_test_cancelled = cancelled;
  const auto endpoint =
      QUrl(normalized, QUrl::StrictMode).toString(QUrl::FullyEncoded).toStdString() + "/health";
  m_worker.Push([this, cancelled, endpoint] {
    constexpr auto timeout = std::chrono::milliseconds{5000};
    Common::HttpRequest http(timeout,
                             [cancelled](auto, auto, auto, auto) { return !cancelled->load(); });
    http.SetSensitive(true);
    http.SetTimeout(timeout);
    QString message = tr("Unable to reach coordinator, or the connection timed out (5 seconds). "
                         "Check the URL and your network connection.");
    if (!cancelled->load() && http.IsValid())
    {
      const auto response = http.Get(endpoint, {}, Common::HttpRequest::AllowedReturnCodes::All);
      if (response)
      {
        const int status = http.GetLastResponseCode();
        if (status != 200)
          message = tr("Coordinator health check failed (HTTP %1).").arg(status);
        else
        {
          const auto document =
              QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(response->data()),
                                                 static_cast<qsizetype>(response->size())));
          message =
              document.isObject() && document.object().value(QStringLiteral("status")).toString() ==
                                         QStringLiteral("ok") ?
                  tr("Connected") :
                  tr("Coordinator returned an invalid health response.");
        }
      }
    }
    QMetaObject::invokeMethod(
        this,
        [this, cancelled, message] {
          m_test->setEnabled(m_coordinator->isEnabled());
          if (!cancelled->load())
            m_result->setText(message);
        },
        Qt::QueuedConnection);
  });
}
