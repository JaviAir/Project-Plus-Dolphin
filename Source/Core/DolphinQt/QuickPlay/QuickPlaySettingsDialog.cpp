// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlaySettingsDialog.h"

#include <chrono>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
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
  auto* form = new QFormLayout;
  m_coordinator = new QLineEdit;
  m_coordinator->setObjectName(QStringLiteral("quickplay_coordinator"));
  form->addRow(tr("Coordinator Server:"), m_coordinator);
  m_region = new QComboBox;
  m_region->setObjectName(QStringLiteral("quickplay_region"));
  for (const auto& region : QuickPlayClient::GetRegions())
    m_region->addItem(region.label, region.value);
  m_canonical_region_count = m_region->count();
  form->addRow(tr("Region:"), m_region);
  layout->addLayout(form);

  m_test = new QPushButton(tr("Test Connection to Coordinator Server"));
  layout->addWidget(m_test);
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
    // Remove the previous custom entry when reopening after a config change.
    while (m_region->count() > m_canonical_region_count)
      m_region->removeItem(m_canonical_region_count);
    const auto region = QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_REGION));
    int index = m_region->findData(QuickPlayClient::NormalizeRegion(region));
    if (index < 0)
    {
      m_region->addItem(tr("Custom (preserved): %1").arg(region), region);
      index = m_region->count() - 1;
    }
    m_region->setCurrentIndex(index);
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
  QString normalized;
  const auto error = QuickPlayClient::NormalizeCoordinatorUrl(m_coordinator->text(), &normalized);
  if (!error.isEmpty())
  {
    m_result->setText(error);
    m_coordinator->setFocus();
    return;
  }
  const auto region = m_region->currentData().toString();
  if (!QuickPlayClient::IsValidRegion(region))
  {
    m_result->setText(tr("The existing custom region is invalid. Choose a listed region. "
                         "Custom regions must be 1–32 ASCII characters without whitespace."));
    return;
  }

  // Match Dolphin's effective-layer convention, and persist even if a command-line override
  // supplied the initial value. Saving explicitly replaces that override for this run too.
  Config::SetBaseOrCurrent(Config::NETPLAY_QUICKPLAY_COORDINATOR, normalized.toStdString());
  Config::SetBase(Config::NETPLAY_QUICKPLAY_COORDINATOR, normalized.toStdString());
  Config::SetBaseOrCurrent(Config::NETPLAY_QUICKPLAY_REGION, region.toStdString());
  Config::SetBase(Config::NETPLAY_QUICKPLAY_REGION, region.toStdString());
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
          m_test->setEnabled(true);
          if (!cancelled->load())
            m_result->setText(message);
        },
        Qt::QueuedConnection);
  });
}
