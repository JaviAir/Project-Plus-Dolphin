// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <gtest/gtest.h>

#include "Core/ProjectPlusOnline/SetupAction.h"

Q_DECLARE_METATYPE(ProjectPlusOnline::SetupCancelRequest)

// An isolated Qt seam: no Settings, emulator, network, renderer or profile bootstrap.
class CancelEmitter final : public QObject
{
  Q_OBJECT
signals:
  void Cancel(ProjectPlusOnline::SetupCancelRequest request);
};

namespace
{
using namespace ProjectPlusOnline;
using namespace std::chrono_literals;
const auto now = SetupActionMailbox::Clock::time_point{10s};

TEST(ProjectPlusOnlineQueuedSetup, TypedDeliveryCopiesIdentityAndRejectsRetiredAttempt)
{
  SetupActionMailbox mailbox;
  CancelEmitter emitter;
  QObject gui;
  int cancellations = 0;
  const auto context = mailbox.Publish({1, 2, 3, 4, 0}, true, now);
  mailbox.Reader(context, true, now);
  auto request = mailbox.Queue(context, now);
  ASSERT_TRUE(request);
  QObject::connect(
      &emitter, &CancelEmitter::Cancel, &gui,
      [&](SetupCancelRequest copy) {
        EXPECT_EQ(copy.context, context);
        if (mailbox.Complete(copy, now))
          ++cancellations;
      },
      Qt::QueuedConnection);
  emit emitter.Cancel(*request);
  EXPECT_EQ(cancellations, 0);
  request->context.identity.attempt = 9;  // Delivery owns its copy.
  mailbox.Publish({1, 2, 5, 6, 0}, true, now);
  QCoreApplication::sendPostedEvents(&gui, QEvent::MetaCall);
  EXPECT_EQ(cancellations, 0);
}

TEST(ProjectPlusOnlineQueuedSetup, QueuedPressIsSingleAndCannotOutliveFocusLossAndRecovery)
{
  SetupActionMailbox mailbox;
  CancelEmitter emitter;
  QObject gui;
  int cancellations = 0;
  auto context = mailbox.Publish({1, 2, 3, 4, 0}, true, now);
  mailbox.Reader(context, true, now);
  QObject::connect(
      &emitter, &CancelEmitter::Cancel, &gui,
      [&](SetupCancelRequest copy) {
        if (mailbox.Complete(copy, now + 20ms))
          ++cancellations;
      },
      Qt::QueuedConnection);
  auto request = mailbox.Queue(context, now);
  ASSERT_TRUE(request);
  emit emitter.Cancel(*request);
  emit emitter.Cancel(*request);
  EXPECT_FALSE(mailbox.Queue(context, now));
  QCoreApplication::sendPostedEvents(&gui, QEvent::MetaCall);
  EXPECT_EQ(cancellations, 1);
  request = mailbox.Queue(context, now + 5ms);
  ASSERT_TRUE(request);
  emit emitter.Cancel(*request);
  mailbox.Reader(context, false, now + 10ms);
  mailbox.Reader(context, true, now + 15ms);
  QCoreApplication::sendPostedEvents(&gui, QEvent::MetaCall);
  EXPECT_EQ(cancellations, 1);
}
}  // namespace

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  qRegisterMetaType<SetupCancelRequest>();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "ProjectPlusOnlineQueuedSetupTest.moc"
