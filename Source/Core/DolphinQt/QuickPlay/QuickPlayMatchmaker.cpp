// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayMatchmaker.h"

#include <algorithm>

#include "Common/Config/Config.h"
#include "Common/TraversalProto.h"
#include "Core/Config/NetplaySettings.h"

bool QuickPlayMatchmaker::IsField(const QString& value, qsizetype limit)
{
  return !value.isEmpty() && value.size() <= limit &&
         std::all_of(value.begin(), value.end(),
                     [](QChar c) { return c.unicode() >= 0x21 && c.unicode() <= 0x7e; });
}

std::array<QuickPlayMatchmaker::Region, 11> QuickPlayMatchmaker::GetRegions()
{
  return {{
      {QStringLiteral("us-east"), tr("United States — East")},
      {QStringLiteral("us-south"), tr("United States — South")},
      {QStringLiteral("us-midwest"), tr("United States — Midwest")},
      {QStringLiteral("us-west"), tr("United States — West")},
      {QStringLiteral("canada"), tr("Canada")},
      {QStringLiteral("mexico-central-america"), tr("Mexico / Central America")},
      {QStringLiteral("caribbean"), tr("Caribbean")},
      {QStringLiteral("south-america"), tr("South America")},
      {QStringLiteral("europe"), tr("Europe")},
      {QStringLiteral("asia"), tr("Asia")},
      {QStringLiteral("australia"), tr("Australia")},
  }};
}

QString QuickPlayMatchmaker::GetRegionLabel(const QString& region)
{
  for (const auto& choice : GetRegions())
  {
    if (region == choice.value)
      return choice.label;
  }
  return region;
}

QString QuickPlayMatchmaker::NormalizeRegion(const QString& region)
{
  const QString normalized = region.trimmed().toLower();
  if (normalized == QStringLiteral("na-east"))
    return QStringLiteral("us-east");
  if (normalized == QStringLiteral("na-central"))
    return QStringLiteral("us-midwest");
  if (normalized == QStringLiteral("na-west"))
    return QStringLiteral("us-west");
  if (normalized == QStringLiteral("eu"))
    return QStringLiteral("europe");
  // Retired: tick the individual US regions instead. Invalid, so dropped from saved lists.
  if (normalized == QStringLiteral("us-nationwide"))
    return {};

  for (const auto& region_choice : GetRegions())
  {
    if (normalized == region_choice.value)
      return region_choice.value;
  }
  // Exact matching makes even case changes significant for unknown custom regions.
  return region;
}

bool QuickPlayMatchmaker::IsValidRegion(const QString& region)
{
  return IsField(region, 32);
}

QStringList QuickPlayMatchmaker::ParseRegions(const QString& comma_separated)
{
  QStringList regions;
  for (const auto& token : comma_separated.split(QLatin1Char(',')))
  {
    const QString region = NormalizeRegion(token.trimmed());
    if (regions.size() < MAX_REGIONS && IsValidRegion(region) && !regions.contains(region))
      regions.append(region);
  }
  return regions;
}

QStringList QuickPlayMatchmaker::LoadRegions()
{
  const auto saved =
      ParseRegions(QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_REGIONS)));
  if (!saved.isEmpty())
    return saved;
  const auto legacy =
      ParseRegions(QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_REGION)));
  if (!legacy.isEmpty())
    return legacy;
  return ParseRegions(QString::fromStdString(Config::NETPLAY_QUICKPLAY_REGION.GetDefaultValue()));
}

QString QuickPlayMatchmaker::NormalizeHostPreference(const QString& preference)
{
  const QString normalized = preference.trimmed().toLower();
  if (normalized == QStringLiteral("host") || normalized == QStringLiteral("join"))
    return normalized;
  return QStringLiteral("none");
}

QString QuickPlayMatchmaker::LoadHostPreference()
{
  return NormalizeHostPreference(
      QString::fromStdString(Config::Get(Config::NETPLAY_QUICKPLAY_HOST_PREFERENCE)));
}

bool QuickPlayMatchmaker::IsValidHostCode(const QString& code)
{
  return !code.isEmpty() && code.size() <= static_cast<qsizetype>(Common::NETPLAY_CODE_SIZE) &&
         std::all_of(code.begin(), code.end(), [](QChar c) {
           const auto u = c.unicode();
           return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9');
         });
}
