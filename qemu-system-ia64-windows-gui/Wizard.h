// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "UI.h"

namespace Ia64::Ui
{
struct ProfileResult
{
    Profile Config;
    bool NewDisk{};
};

using CommandPreview = std::function<std::wstring(const Profile&, const Machine&)>;
std::optional<ProfileResult> ShowProfileWindow(HWND owner, Appearance appearance, const Engine& engine,
                                               const Capabilities& capabilities, const Profile* existing,
                                               CommandPreview preview);
void ShowRunningProfile(HWND owner, Appearance appearance, Engine engine, Capabilities capabilities, Machine machine,
                        Profile profile, std::wstring command);
} // namespace Ia64::Ui
