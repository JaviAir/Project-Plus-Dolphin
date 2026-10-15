// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayDialog.h"

#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

QuickPlayDialog::QuickPlayDialog(QuickPlayController& controller, QWidget* parent)
    : QDialog(parent), m_controller(controller)
{
  setWindowTitle(tr("Project+ Quick Play"));
  setMinimumSize(560, 320);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(18, 18, 18, 18);
  layout->setSpacing(12);
  auto* preview = new QLabel(tr("Quick Play pairs players through the Dolphin lobby, or a custom "
                                "coordinator chosen in Quick Play Settings. The host creates a "
                                "room and the opponent joins automatically."));
  preview->setWordWrap(true);
  layout->addWidget(preview);

  m_status = new QLabel;
  m_status->setTextFormat(Qt::PlainText);
  m_status->setAlignment(Qt::AlignCenter);
  m_status->setWordWrap(true);
  // Reserve space for multiline errors even while displaying the short Searching text.
  m_status->setMinimumHeight(m_status->fontMetrics().lineSpacing() * 5);
  layout->addWidget(m_status, 1);

  // Regions one per line in the first column, the host preference in the second.
  m_details = new QWidget;
  auto* details = new QHBoxLayout(m_details);
  details->setContentsMargins(0, 0, 0, 0);
  details->setSpacing(48);
  m_region = new QLabel;
  m_region->setObjectName(QStringLiteral("quickplay_search_region"));
  m_region->setTextFormat(Qt::PlainText);
  m_region->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  m_host_preference = new QLabel;
  m_host_preference->setObjectName(QStringLiteral("quickplay_search_host_preference"));
  m_host_preference->setTextFormat(Qt::PlainText);
  m_host_preference->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  details->addStretch();
  details->addWidget(m_region, 0, Qt::AlignTop);
  details->addWidget(m_host_preference, 0, Qt::AlignTop);
  details->addStretch();
  layout->addWidget(m_details);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::rejected, this, &QuickPlayDialog::reject);
  layout->addWidget(buttons);

  m_animation_timer.setInterval(400);
  connect(&m_animation_timer, &QTimer::timeout, this, [this] {
    m_dots = m_dots % 3 + 1;
    UpdateSearchingText();
  });
  connect(&m_controller, &QuickPlayController::StateChanged, this, &QuickPlayDialog::UpdateState);
  UpdateState(m_controller.GetState());
}

void QuickPlayDialog::done(int result)
{
  // Covers Cancel, Escape, window close, and programmatic dialog completion.
  m_animation_timer.stop();
  // Successful acceptance transfers presentation to the ordinary lobby. Rejection
  // remains an explicit user cancellation.
  if (result != QDialog::Accepted)
    m_controller.Cancel();
  QDialog::done(result);
}

void QuickPlayDialog::UpdateState(QuickPlayController::State state)
{
  m_animation_timer.stop();

  const auto& regions = m_controller.GetRegions();
  if (!m_controller.GetMatchRegion().isEmpty() || regions.size() == 1)
  {
    const auto& region =
        m_controller.GetMatchRegion().isEmpty() ? regions.front() : m_controller.GetMatchRegion();
    m_region->setText(tr("Region:\n%1").arg(QuickPlayMatchmaker::GetRegionLabel(region)));
  }
  else
  {
    QStringList labels;
    for (const auto& region : regions)
      labels.append(QuickPlayMatchmaker::GetRegionLabel(region));
    m_region->setText(tr("Regions:\n%1").arg(labels.join(QLatin1Char('\n'))));
  }
  const auto& preference = m_controller.GetHostPreference();
  m_host_preference->setText(tr("Host preference:\n%1")
                                 .arg(preference == QStringLiteral("host") ? tr("Prefer to host") :
                                      preference == QStringLiteral("join") ? tr("Prefer to join") :
                                                                             tr("No preference")));
  m_details->setVisible(state != QuickPlayController::State::Idle);

  switch (state)
  {
  case QuickPlayController::State::Idle:
    m_status->setText(tr("Ready for Quick Play."));
    break;
  case QuickPlayController::State::Searching:
    m_dots = 1;
    UpdateSearchingText();
    m_animation_timer.start();
    break;
  case QuickPlayController::State::MatchedHost:
    m_status->setText(tr("Opponent found...\nMatched as host"));
    break;
  case QuickPlayController::State::MatchedClient:
    m_status->setText(tr("Opponent found...\nMatched as client"));
    break;
  case QuickPlayController::State::PreparingHandoff:
  case QuickPlayController::State::WaitingForHandoff:
    m_status->setText(tr("Opponent found. Preparing session..."));
    break;
  case QuickPlayController::State::ValidatingMatch:
    m_status->setText(tr("Checking match..."));
    break;
  case QuickPlayController::State::CreatingHost:
    m_status->setText(tr("Creating room..."));
    break;
  case QuickPlayController::State::WaitingForTraversalCode:
  case QuickPlayController::State::PublishingHostCode:
    m_status->setText(tr("Waiting for connection..."));
    break;
  case QuickPlayController::State::WaitingForOpponent:
    m_status->setText(tr("Room ready. Waiting for opponent..."));
    break;
  case QuickPlayController::State::WaitingForHost:
    m_status->setText(tr("Waiting for host..."));
    break;
  case QuickPlayController::State::Connecting:
    m_status->setText(tr("Connecting..."));
    break;
  case QuickPlayController::State::NetPlayConnected:
  case QuickPlayController::State::NetPlayOwned:
    accept();
    return;
  case QuickPlayController::State::Error:
    m_status->setText(tr("Error\n%1").arg(m_controller.GetErrorMessage()));
    break;
  }

  // Grow for longer messages or larger fonts without shrinking on the next state change.
  layout()->activate();
  resize(size().expandedTo(sizeHint()));
}

void QuickPlayDialog::UpdateSearchingText()
{
  QString text =
      tr("Searching (%1)").arg(m_controller.GetBackendLabel()) + QString(m_dots, QLatin1Char('.'));
  switch (m_controller.GetSearchStage())
  {
  case QuickPlayMatchmaker::SearchStage::JoinOnly:
    text += QLatin1Char('\n') + tr("Looking for a host to join. Listing yourself in %n s.", "",
                                   m_controller.GetSearchStageSeconds());
    break;
  case QuickPlayMatchmaker::SearchStage::JoinListed:
    text += QLatin1Char('\n') + tr("No host was waiting, so you are listed too. You host only if "
                                   "your opponent also prefers to join.");
    break;
  case QuickPlayMatchmaker::SearchStage::Normal:
    break;
  }
  if (!m_controller.GetSearchNotice().isEmpty())
    text += QLatin1Char('\n') + m_controller.GetSearchNotice();
  m_status->setText(text);
}
