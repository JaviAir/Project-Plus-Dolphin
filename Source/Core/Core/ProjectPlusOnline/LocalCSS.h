// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Core
{
class System;
}
namespace ProjectPlusOnline
{
// Real CPU owner only; also used at explicitly owned paused restore boundaries.
bool ObserveLocalCSS(Core::System& system);
}  // namespace ProjectPlusOnline
