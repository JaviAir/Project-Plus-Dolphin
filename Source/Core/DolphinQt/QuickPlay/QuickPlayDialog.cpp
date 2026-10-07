// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayDialog.h"

#include <QDialogButtonBox>
#include <QFontMetrics>
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
  auto* preview = new QLabel(tr("Quick Play pairs players through the Coordinator Server. "
                                "The host creates a room and the opponent joins automatically."));
  preview->setWordWrap(true);
  layout->addWidget(preview);

  m_status = new QLabel;
  m_status->setAlignment(Qt::AlignCenter);
  m_status->setWordWrap(true);
  // Reserve space for multiline errors even while displaying the short Searching text.
  m_status->setMinimumHeight(m_status->fontMetrics().lineSpacing() * 5);
  layout->addWidget(m_status, 1);

  m_region = new QLabel;
  m_region->setObjectName(QStringLiteral("quickplay_search_region"));
  m_region->setTextFormat(Qt::PlainText);
  m_region->setAlignment(Qt::AlignCenter);
  m_region->setWordWrap(true);
  layout->addWidget(m_region);

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
  m_controller.Cancel();
  QDialog::done(result);
}

void QuickPlayDialog::UpdateState(QuickPlayController::State state)
{
  m_animation_timer.stop();

  m_region->setText(
      tr("Region: %1").arg(QuickPlayClient::GetRegionLabel(m_controller.GetRegion())));
  m_region->setVisible(state != QuickPlayController::State::Idle);

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
  m_status->setText(tr("Searching") + QString(m_dots, QLatin1Char('.')));
}
