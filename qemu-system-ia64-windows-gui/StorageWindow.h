// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "UI.h"

namespace Ia64::Ui
{
bool EditStorage(HWND owner, Appearance appearance, Profile& profile, const Engine& engine, const Machine& machine,
                 const Capabilities& caps, const Fs::path& pendingDisk, std::wstring selection = L"onboard",
                 bool readOnly = false);
}
