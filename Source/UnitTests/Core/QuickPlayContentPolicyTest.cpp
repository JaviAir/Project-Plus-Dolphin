// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "DolphinQt/QuickPlay/ContentPolicy.h"

namespace
{
using namespace QuickPlayContent;
const QString build = QStringLiteral("7164321368-trial-candidate-persistent1");
const QString config = QStringLiteral("effective supported configuration");
QJsonObject Manifest()
{
  QJsonObject json{{QStringLiteral("schema"), 1},
                   {QStringLiteral("build"), QString::fromLatin1(ARCHIVED_BUILD)},
                   {QStringLiteral("config"), config}};
  for (const auto key : CONTENT_KEYS)
    json[QString::fromLatin1(key)] = QString(64, u'a');
  json[QStringLiteral("checkpoint")] = QString::fromLatin1(CHECKPOINT_SHA);
  return json;
}
class ContentFixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ASSERT_TRUE(directory.isValid());
    for (const auto key : CONTENT_KEYS)
    {
      const auto name = QString::fromLatin1(key);
      const auto path = directory.filePath(name);
      paths[name] = path;
      if (name == QStringLiteral("nand") || name.endsWith(QStringLiteral("settings")))
      {
        ASSERT_TRUE(QDir().mkpath(path));
        QFile save(QDir(path).filePath(QStringLiteral("current-save")));
        ASSERT_TRUE(save.open(QIODevice::WriteOnly));
        save.write("writable autosave differs from certified copy");
      }
      else
      {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("uncertified content: trust declaration only in trial");
      }
    }
  }
  QTemporaryDir directory;
  Paths paths;
};
TEST(QuickPlayContentPolicy, BuildSelection)
{
#ifdef QUICKPLAY_NO_BULK_SCAN_TRIAL
  EXPECT_EQ(SelectedPolicy(), Policy::DeclaredBundleTrialV1);
#else
  EXPECT_EQ(SelectedPolicy(), Policy::Strict);
#endif
}
TEST(QuickPlayContentPolicy, ArchivedCompatibilityIsExplicitAndTrialOnly)
{
  auto json = Manifest();
  EXPECT_FALSE(CompatibleManifest(json, Policy::Strict, build, config));
  EXPECT_TRUE(CompatibleManifest(json, Policy::DeclaredBundleTrialV1, build, config));
  json[QStringLiteral("build")] = build;
  EXPECT_TRUE(CompatibleManifest(json, Policy::Strict, build, config));
  json[QStringLiteral("build")] = QStringLiteral("arbitrary-old-build-persistent1");
  EXPECT_FALSE(CompatibleManifest(json, Policy::DeclaredBundleTrialV1, build, config));
}
TEST(QuickPlayContentPolicy, RejectsSchemaConfigurationCheckpointAndMalformedDeclarations)
{
  for (const auto key : CONTENT_KEYS)
  {
    auto json = Manifest();
    json.remove(QString::fromLatin1(key));
    EXPECT_FALSE(CompatibleManifest(json, Policy::DeclaredBundleTrialV1, build, config));
    json[QString::fromLatin1(key)] = QString(64, u'z');
    EXPECT_FALSE(CompatibleManifest(json, Policy::DeclaredBundleTrialV1, build, config));
  }
  auto json = Manifest();
  json[QStringLiteral("checkpoint")] = QString(64, u'a');
  EXPECT_FALSE(CompatibleManifest(json, Policy::DeclaredBundleTrialV1, build, config));
  json = Manifest();
  json[QStringLiteral("schema")] = 1.5;
  EXPECT_FALSE(CompatibleManifest(json, Policy::DeclaredBundleTrialV1, build, config));
  EXPECT_FALSE(CompatibleManifest(Manifest(), Policy::DeclaredBundleTrialV1, build,
                                  QString(config).append(u'x')));
}
TEST_F(ContentFixture, InitialMatchedRepeatAndRecoveryNeverInvokeHasher)
{
  const auto json = Manifest();
  // Re-use the real admission gate at each lifecycle entry. It must not even
  // invoke a file/tree verifier on naturally changed saves or arbitrary bytes.
  for (int search = 0; search < 3; ++search)
  {
    for (int admission = 0; admission < 2; ++admission)
      EXPECT_TRUE(CheckContent(json, paths, Policy::DeclaredBundleTrialV1,
                               [](const QString&, const QString&) {
                                 ADD_FAILURE() << "Trial invoked file/tree content hashing";
                                 return QByteArray{};
                               }).isEmpty());
    for (int restore = 0; restore < 2; ++restore)
      EXPECT_TRUE(CheckpointMayLoad(Policy::DeclaredBundleTrialV1, [] {
        ADD_FAILURE() << "Trial preread checkpoint before ordinary loader";
        return false;
      }));
  }
  QFile save(directory.filePath(QStringLiteral("nand/current-save")));
  ASSERT_TRUE(save.open(QIODevice::ReadOnly));
  EXPECT_EQ(save.readAll(), "writable autosave differs from certified copy");
}
TEST_F(ContentFixture, StrictStillCallsAllContentVerifiersAndRejectsDrift)
{
  const auto json = Manifest();
  int calls = 0;
  EXPECT_TRUE(CheckContent(json, paths, Policy::Strict,
                           [&](const QString& key, const QString&) {
                             ++calls;
                             return json[key].toString().toLatin1();
                           }).isEmpty());
  EXPECT_EQ(calls, 8);
  EXPECT_EQ(CheckContent(json, paths, Policy::Strict,
                         [&](const QString& key, const QString&) {
                           return key == QStringLiteral("nand") ? QByteArray(64, 'b') :
                                                                  json[key].toString().toLatin1();
                         }), QStringLiteral("nand"));
  EXPECT_FALSE(CheckpointMayLoad(Policy::Strict, [] { return false; }));
}
TEST_F(ContentFixture, MissingOrWrongKindRequiredPathsFailWithoutHashing)
{
  for (auto it = paths.begin(); it != paths.end(); ++it)
  {
    auto missing = paths;
    missing[it.key()] = directory.filePath(QStringLiteral("absent"));
    EXPECT_EQ(CheckContent(Manifest(), missing, Policy::DeclaredBundleTrialV1, {}), it.key());
  }
  auto wrong_kind = paths;
  wrong_kind[QStringLiteral("checkpoint")] = paths[QStringLiteral("nand")];
  EXPECT_EQ(CheckContent(Manifest(), wrong_kind, Policy::DeclaredBundleTrialV1, {}),
            QStringLiteral("checkpoint"));
}
TEST_F(ContentFixture, ManifestReadsAreBoundedAndMalformedJsonFails)
{
  const auto path = directory.filePath(QStringLiteral("manifest.json"));
  const auto write = [&](const QByteArray& bytes) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    EXPECT_EQ(file.write(bytes), bytes.size());
  };
  QJsonObject json;
  write(QJsonDocument(Manifest()).toJson());
  EXPECT_TRUE(ReadManifest(path, &json));
  write("{broken}");
  EXPECT_FALSE(ReadManifest(path, &json));
  write(QByteArray(MAX_MANIFEST_BYTES + 1, ' '));
  EXPECT_FALSE(ReadManifest(path, &json));
}
TEST(QuickPlayContentPolicy, PolicyVersionBuildAndDeclaredContentFencePeerIdentity)
{
  const auto json = Manifest();
  const auto strict = ContractIdentity(json, Policy::Strict, build);
  const auto trial = ContractIdentity(json, Policy::DeclaredBundleTrialV1, build);
  EXPECT_EQ(trial.size(), 64u);
  EXPECT_NE(strict, trial);
  EXPECT_EQ(trial, ContractIdentity(json, Policy::DeclaredBundleTrialV1, build));
  EXPECT_NE(trial, ContractIdentity(json, Policy::DeclaredBundleTrialV1, QString(build).append(u'x')));
  auto changed = json;
  changed[QStringLiteral("nand")] = QString(64, u'b');
  EXPECT_NE(trial, ContractIdentity(changed, Policy::DeclaredBundleTrialV1, build));
  EXPECT_EQ(strict, QCryptographicHash::hash(QJsonDocument(json).toJson(QJsonDocument::Compact),
                                           QCryptographicHash::Sha256).toHex().toStdString());
}
TEST(QuickPlayContentPolicy, LauncherIdentificationUsesDeclarationWithoutReadingFile)
{
  const auto launcher = DeclaredLauncher(QStringLiteral("/does/not/exist.dol"), 100, Manifest(), build);
  EXPECT_EQ(launcher.identity.game_id, "ID-Netplay Launcher");
  EXPECT_EQ(launcher.identity.dol_elf_size, 100u);
  EXPECT_EQ(launcher.identity,
            DeclaredLauncher(QStringLiteral("/other/profile.dol"), 100, Manifest(), build).identity);
  auto changed = Manifest();
  changed[QStringLiteral("netplay")] = QString(64, u'b');
  EXPECT_NE(launcher.identity, DeclaredLauncher(QStringLiteral("/other"), 100, changed, build).identity);
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
