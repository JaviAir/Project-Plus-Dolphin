// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compile the real version accessors with a generated fixture scmrev.h. Standard headers
// are loaded first; the alternate platform macro affects only the display metadata branch
// in Version.cpp. This does not produce or claim to test a Windows application binary.
#include <string>
#include "Common/Version.h"

#ifdef NETPLAY_TEST_OTHER_PLATFORM
#ifdef _WIN32
#undef _WIN32
#define __APPLE__ 1
#else
#undef __APPLE__
#define _WIN32 1
#endif
#endif
#include "../../Core/Common/Version.cpp"
