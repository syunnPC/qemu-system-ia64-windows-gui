// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Localization.h"
#include "Platform.h"
#include <cwchar>

namespace Ia64
{
UiLanguage WindowsUiLanguage()
{
    ULONG count{}, length{};
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &length))
    {
        WinError(L"GetUserPreferredUILanguages");
    }
    std::wstring names(length, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &length))
    {
        WinError(L"GetUserPreferredUILanguages");
    }
    for (const wchar_t* name = names.c_str(); *name; name += std::wcslen(name) + 1)
    {
        switch (PRIMARYLANGID(LocaleNameToLCID(name, LOCALE_ALLOW_NEUTRAL_NAMES)))
        {
        case LANG_JAPANESE:
            return UiLanguage::Japanese;
        case LANG_ENGLISH:
            return UiLanguage::English;
        }
    }
    return UiLanguage::English;
}

void ApplyUiLanguage(UiLanguage preference)
{
    const auto language = preference == UiLanguage::System ? WindowsUiLanguage() : preference;
    if (!SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, LanguageInfoFor(language).Tag, nullptr))
    {
        WinError(L"SetProcessPreferredUILanguages");
    }
    // The UI thread and background jobs should resolve the same resources.
    if (!SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, nullptr, nullptr))
    {
        WinError(L"SetThreadPreferredUILanguages");
    }
}
} // namespace Ia64
