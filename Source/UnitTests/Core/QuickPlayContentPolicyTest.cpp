// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <gtest/gtest.h>

#include "DolphinQt/QuickPlay/ContentPolicy.h"

namespace
{
using namespace QuickPlayContent;
const QString build = QStringLiteral("7164321368");
const QString recipe = QStringLiteral("portable-css-ab/2");
const QString config = QStringLiteral("effective supported configuration");
TEST(QuickPlayContentPolicy, BuildSelection)
{
#ifdef QUICKPLAY_NO_BULK_SCAN_TRIAL
  EXPECT_EQ(SelectedPolicy(), Policy::DeclaredBundleTrialV1);
#else
  EXPECT_EQ(SelectedPolicy(), Policy::Strict);
#endif
}
TEST(QuickPlayContentPolicy, PolicyBuildRecipeAndConfigurationFencePeerIdentity)
{
  const auto strict = ContractIdentity(Policy::Strict, build, recipe, config);
  const auto trial = ContractIdentity(Policy::DeclaredBundleTrialV1, build, recipe, config);
  EXPECT_EQ(trial.size(), 64u);
  EXPECT_NE(strict, trial);
  EXPECT_EQ(trial, ContractIdentity(Policy::DeclaredBundleTrialV1, build, recipe, config));
  EXPECT_NE(trial, ContractIdentity(Policy::DeclaredBundleTrialV1, QString(build).append(u'x'),
                                    recipe, config));
  EXPECT_NE(trial, ContractIdentity(Policy::DeclaredBundleTrialV1, build,
                                    QStringLiteral("portable-css-ab/3"), config));
  EXPECT_NE(trial, ContractIdentity(Policy::DeclaredBundleTrialV1, build, recipe,
                                    QString(config).append(u'x')));
}
TEST(QuickPlayContentPolicy, LauncherIdentificationUsesDeclarationWithoutReadingFile)
{
  const auto launcher = DeclaredLauncher(QStringLiteral("/does/not/exist.dol"), 100,
                                         "ID-Project+ Netplay Launcher", build, recipe);
  EXPECT_EQ(launcher.identity.game_id, "ID-Project+ Netplay Launcher");
  EXPECT_EQ(launcher.identity.dol_elf_size, 100u);
  EXPECT_EQ(launcher.identity, DeclaredLauncher(QStringLiteral("/other/profile.dol"), 100,
                                                "ID-Project+ Netplay Launcher", build, recipe)
                                   .identity);
  EXPECT_NE(launcher.identity, DeclaredLauncher(QStringLiteral("/other"), 100,
                                                "ID-Project+ Netplay Launcher",
                                                QString(build).append(u'x'), recipe)
                                   .identity);
}
TEST(QuickPlayContentPolicy, DiscIdentificationRejectsMissingMismatchedAndNonWiiHeaders)
{
  QByteArray header(0x20, '\0');
  header.replace(0, 6, "RSBE01");
  header.replace(0x18, 4, QByteArray::fromHex("5d1c9ea3"));
  EXPECT_TRUE(DiscHeadersMatch(header, header));
  EXPECT_FALSE(DiscHeadersMatch({}, header));
  auto other = header;
  other[7] = 2;
  EXPECT_FALSE(DiscHeadersMatch(other, header));
  other[0x18] = 0;
  EXPECT_FALSE(DiscHeadersMatch(other, other));
}
}  // namespace
int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
