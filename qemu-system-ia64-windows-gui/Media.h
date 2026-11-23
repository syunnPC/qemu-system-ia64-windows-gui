// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform.h"

namespace Ia64
{
struct MediaRecommendation
{
    std::wstring Description;
    std::wstring Machine;
    bool Ahci{};
};

bool HasIsoExtension(const Fs::path& path);
bool IsOpticalImage(const Fs::path& path);
MediaRecommendation InspectIso(const Fs::path& path);
} // namespace Ia64
