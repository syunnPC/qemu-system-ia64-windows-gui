// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Model.h"
#include "Resource.h"
#include <array>

namespace Ia64::Ui
{
inline constexpr std::array DeviceOptions{std::pair{L"i8042", IDS_I8042}, std::pair{L"usb", IDS_Usb},
                                          std::pair{L"pcie", IDS_Pcie}, std::pair{L"firmware-ide-dma", IDS_Dma},
                                          std::pair{L"usb1", IDS_Usb1}};

std::wstring NetworkDefault(std::wstring_view machine, const Capabilities& capabilities);
std::wstring VgaName(std::wstring_view value);
void ShowProfileDetails(HWND list, const Profile& profile, const Capabilities* capabilities, bool review = false,
                        std::optional<unsigned> newDiskMiB = {}, int unavailable = IDS_DefaultsUnavailable);
} // namespace Ia64::Ui
