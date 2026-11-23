// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Model.h"
#include <commctrl.h>
#include <functional>
#include <initializer_list>
#include <span>

namespace Ia64::Ui
{
struct Palette
{
    COLORREF Background{};
    COLORREF Panel{};
    COLORREF Field{};
    COLORREF Text{};
    COLORREF Muted{};
    COLORREF Border{};
    COLORREF Accent{};
    COLORREF OnAccent{};
    COLORREF Selection{};
    bool Dark{};
    bool HighContrast{};
};

enum class TextRole
{
    Body,
    Title,
    Section,
    Secondary,
    Monospace
};

struct LayoutMetrics
{
    int Line{}, Character{}, Unit{}, Gap{}, Margin{}, Control{}, Row{};
    int Footer() const
    {
        return Control + Gap * 2;
    }
    int Header() const
    {
        return Line * 2 + Gap * 2;
    }
    int PathWidth() const
    {
        return Character * 46;
    }
};

class Window
{
public:
    explicit Window(Appearance appearance) : _appearance(appearance)
    {
    }
    virtual ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    HWND Get() const
    {
        return _window;
    }
    void Open(std::wstring_view title, HWND owner, DWORD style, DWORD extended = WS_EX_CONTROLPARENT,
              HWND positionRelativeTo = nullptr);
    void Modal();
    virtual bool TranslateKey(const MSG&)
    {
        return false;
    }
    virtual void SetAppearance(Appearance appearance);
    void RefreshLanguage();
    static void NotifyTextScale(double factor, DWORD uiThread);
    int Px(int units) const
    {
        return MulDiv(units, _layoutDpi, USER_DEFAULT_SCREEN_DPI);
    }
    int Dip(int pixels) const
    {
        return MulDiv(pixels, USER_DEFAULT_SCREEN_DPI, _layoutDpi);
    }
    int TextWidth(std::wstring_view text, HWND control = nullptr) const;
    int WrappedHeight(HWND label, int width) const;
    int ButtonWidth(HWND control) const;
    int ComboWidth(HWND control) const;
    int InputWidth(HWND control, int available) const;
    void Place(HWND control, int x, int y, int width, int height) const;
    void Text(HDC dc, std::wstring_view text, RECT rect, bool heading = false, bool muted = false,
              UINT options = DT_LEFT | DT_VCENTER | DT_SINGLELINE, COLORREF color = CLR_INVALID) const;
    void Fill(HDC dc, const RECT& rect, COLORREF color) const;
    void Rounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) const;
    void Icon(HDC dc, wchar_t glyph, RECT bounds, COLORREF color) const;
    RECT Rect(int left, int top, int right, int bottom) const;

protected:
    struct TabStop
    {
        HWND Control;
        int Top = -1;
        int Bottom = -1;
    };
    bool NextFormControl(std::span<const TabStop> stops, int& scroll, int viewport);
    HWND Button(int id, std::wstring_view label, bool primary = false, wchar_t glyph = 0);
    void ButtonGlyph(HWND button, wchar_t glyph) const;
    HWND Action(int id, std::wstring_view label, wchar_t glyph);
    HWND Label(std::wstring_view text, TextRole role = TextRole::Body, DWORD style = SS_LEFT);
    void UseFont(HWND control, TextRole role) const;
    void PlaceFooter(int width, int height, std::initializer_list<HWND> buttons) const;
    HWND OnPanel(HWND control);
    void DefaultHint(HWND control, std::wstring_view value, bool useWhenEmpty = true);
    void EditCue(HWND control, std::wstring_view text);
    HWND Edit(int id, std::wstring_view text = {}, DWORD extra = ES_AUTOHSCROLL);
    HWND Combo(int id);
    HWND PropertyList(int id);
    void PlacePropertyList(HWND list, int x, int y, int width, int height) const;
    HWND Child(std::wstring_view type, std::wstring_view text, int id, DWORD style, DWORD extended = 0);
    void Refresh();
    virtual void Create() = 0;
    virtual void LanguageChanged()
    {
    }
    virtual void Layout(int width, int height) = 0;
    virtual void Paint(HDC dc, const RECT& area) = 0;
    virtual LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam);
    virtual SIZE PreferredSize(SIZE available) const = 0;
    virtual POINT MinimumSize() const
    {
        return {_metrics.Character * 48, _metrics.Line * 15};
    }
    LayoutMetrics _metrics;
    Palette _colors;
    HWND _window{};
    Appearance _appearance;

private:
    static LRESULT CALLBACK Route(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK EditHint(HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                     DWORD_PTR owner);
    void ApplyTheme();
    void ApplyFont();
    HFONT ControlFont(HWND control) const;
    void DrawButton(const DRAWITEMSTRUCT& item) const;
    void DrawChoice(const DRAWITEMSTRUCT& item) const;
    LRESULT DrawProperties(NMLVCUSTOMDRAW& draw) const;
    void DrawFields(HDC dc) const;
    LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam);
    UINT _dpi{96};
    int _layoutDpi{USER_DEFAULT_SCREEN_DPI};
    HFONT _font{};
    HFONT _heading{};
    HFONT _section{};
    HFONT _monospace{};
    HFONT _icons{};
    HBRUSH _background{};
    HBRUSH _panel{};
    HBRUSH _field{};
    HWND _tips{};
    struct FieldHint
    {
        std::wstring Default, Cue;
    };
    std::map<HWND, FieldHint> _hints;
    bool _modal{};
    bool _creationFailed{};
    std::exception_ptr _createdError;
};

std::wstring Read(HWND control);
std::wstring SystemThemeName();
void Set(HWND control, std::wstring_view text);
void Enable(HWND control, bool enabled);
void Options(HWND combo, const std::vector<std::wstring>& items, std::wstring_view selected);
std::wstring Selection(HWND combo);
unsigned Number(HWND edit, std::wstring_view name, unsigned maximum, bool optional = false);
enum class FileChoice
{
    Open,
    CreateDisk,
    ExistingOrNew
};
Fs::path ChooseFile(HWND parent, std::wstring_view title, std::wstring_view filterName, std::wstring_view pattern,
                    FileChoice mode = FileChoice::Open, const Fs::path& initial = {});
void ShowError(HWND owner, std::wstring_view message);
void Busy(HWND owner, Appearance appearance, std::wstring_view message, std::function<void()> action);
} // namespace Ia64::Ui
