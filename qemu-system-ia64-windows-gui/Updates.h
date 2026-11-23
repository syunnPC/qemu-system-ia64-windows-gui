// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Model.h"

namespace Ia64
{
enum class Channel
{
    Stable,
    BetaStable,
    Experimental
};
std::wstring_view ChannelName(Channel channel);
std::optional<Engine> UpdateEngine(Channel channel, const Engine& current);
} // namespace Ia64
