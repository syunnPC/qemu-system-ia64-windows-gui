// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Resource.h"
#include <Windows.h>
#include <array>
#include <format>
#include <string>

namespace Ia64
{
enum class UiLanguage
{
    System,
    Japanese,
    English
};
struct LanguageInfo
{
    const wchar_t* Tag;
    const wchar_t* NativeName;
};
// Windows takes a double-null-terminated list of language names.
inline constexpr std::array InterfaceLanguages{LanguageInfo{L"system", L""}, LanguageInfo{L"ja-JP\0", L"日本語"},
                                               LanguageInfo{L"en-US\0", L"English"}};
inline const LanguageInfo& LanguageInfoFor(UiLanguage language)
{
    return InterfaceLanguages.at(static_cast<size_t>(language));
}
UiLanguage WindowsUiLanguage();
void ApplyUiLanguage(UiLanguage preference);

inline std::wstring LText(int id)
{
    const wchar_t* resource = nullptr;
    const auto count = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&resource), 0);
    return count ? std::wstring(resource, count) : L"[" + std::to_wstring(id) + L"]";
}

template <typename... Args> std::wstring LFmt(int id, const Args&... arguments)
{
    return std::vformat(LText(id), std::make_wformat_args(arguments...));
}
} // namespace Ia64
