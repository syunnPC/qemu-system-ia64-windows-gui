// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "UI.h"
#include "Localization.h"

#include <Dwmapi.h>
#include <ShObjIdl.h>
#include <Uxtheme.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <format>
#include <limits>
#include <memory>
#include <thread>

namespace Ia64::Ui
{
namespace
{
constexpr wchar_t WindowClass[] = L"Qemu.Ia64.Window";
constexpr UINT WorkDone = WM_APP + 1;
constexpr UINT TextScaleChanged = WM_APP + 4;
std::atomic<int> TextScaleDpi{USER_DEFAULT_SCREEN_DPI};
constexpr wchar_t FieldFrame[] = L"Ia64.FieldFrame";
constexpr wchar_t Hovered[] = L"Ia64.Hovered";
constexpr wchar_t PanelSurface[] = L"Ia64.Panel";
constexpr wchar_t FontRole[] = L"Ia64.FontRole";
constexpr wchar_t FlatButton[] = L"Ia64.FlatButton";
constexpr wchar_t Properties[] = L"Ia64.PropertyList";

void ThemeControl(HWND control, const Palette& colors)
{
    wchar_t type[32]{};
    GetClassNameW(control, type, static_cast<int>(std::size(type)));
    const bool tree = _wcsicmp(type, WC_TREEVIEWW) == 0;
    const bool combo = _wcsicmp(type, WC_COMBOBOXW) == 0;
    const auto theme = colors.Dark                    ? (combo ? L"DarkMode_CFD" : L"DarkMode_Explorer")
                       : tree && !colors.HighContrast ? L"Explorer"
                                                      : nullptr;
    SetWindowTheme(control, theme, nullptr);
    if (tree)
    {
        TreeView_SetBkColor(control, colors.HighContrast ? GetSysColor(COLOR_WINDOW) : colors.Panel);
        TreeView_SetTextColor(control, colors.Text);
        TreeView_SetLineColor(control, colors.Border);
    }
}

COLORREF Blend(COLORREF base, COLORREF ink, unsigned percent)
{
    const auto channel = [percent](unsigned a, unsigned b) { return (a * (100 - percent) + b * percent) / 100; };
    return RGB(channel(GetRValue(base), GetRValue(ink)), channel(GetGValue(base), GetGValue(ink)),
               channel(GetBValue(base), GetBValue(ink)));
}

LRESULT CALLBACK TrackButton(HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    // Dialog navigation promotes the focused button to a default push button.
    // Keep the drawing contract while allowing the dialog to route Enter normally.
    if (message == BM_SETSTYLE)
    {
        wparam = (wparam & ~static_cast<WPARAM>(BS_TYPEMASK)) | BS_OWNERDRAW;
    }
    if (message == WM_GETDLGCODE)
    {
        return DefSubclassProc(control, message, wparam, lparam) |
               (GetFocus() == control || GetPropW(control, L"Primary") ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
    }
    if (message == WM_MOUSEMOVE && !GetPropW(control, Hovered))
    {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
        if (TrackMouseEvent(&tracking))
        {
            SetPropW(control, Hovered, reinterpret_cast<HANDLE>(1));
        }
        InvalidateRect(control, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE)
    {
        RemovePropW(control, Hovered);
        InvalidateRect(control, nullptr, FALSE);
    }
    if (message == WM_NCDESTROY)
    {
        RemovePropW(control, Hovered);
        RemoveWindowSubclass(control, TrackButton, id);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

template <typename T> struct ReleaseCom
{
    void operator()(T* value) const
    {
        value->Release();
    }
};

bool DarkSystem()
{
    DWORD light = 1;
    DWORD length = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &length);
    return light == 0;
}

Palette Colors(Appearance appearance)
{
    HIGHCONTRASTW contrast{sizeof(contrast), 0, nullptr};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    if (contrast.dwFlags & HCF_HIGHCONTRASTON)
    {
        return {GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_BTNFACE),
                GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(COLOR_HIGHLIGHTTEXT),
                GetSysColor(COLOR_HIGHLIGHT),
                false,
                true};
    }
    const bool dark = appearance == Appearance::Dark || (appearance == Appearance::System && DarkSystem());
    DWORD colorization{};
    BOOL opaque{};
    auto accent = GetSysColor(COLOR_HIGHLIGHT);
    if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque)))
    {
        accent = RGB((colorization >> 16) & 0xff, (colorization >> 8) & 0xff, colorization & 0xff);
    }
    const auto shade = [dark](unsigned component) {
        return static_cast<BYTE>(dark ? (component * 3 + 255 * 2) / 5 : component * 4 / 5);
    };
    accent = RGB(shade(GetRValue(accent)), shade(GetGValue(accent)), shade(GetBValue(accent)));
    const auto linear = [](unsigned c) {
        const double s = static_cast<double>(c) / 255;
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    const auto luminance =
        0.2126 * linear(GetRValue(accent)) + 0.7152 * linear(GetGValue(accent)) + 0.0722 * linear(GetBValue(accent));
    const auto onAccent = luminance > 0.179 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    if (dark)
    {
        return {RGB(30, 30, 30),
                RGB(37, 37, 37),
                RGB(47, 47, 47),
                RGB(245, 245, 245),
                RGB(184, 184, 184),
                RGB(67, 67, 67),
                accent,
                onAccent,
                Blend(RGB(37, 37, 37), accent, 20),
                true,
                false};
    }
    return {RGB(255, 255, 255),
            RGB(245, 245, 245),
            RGB(255, 255, 255),
            RGB(28, 28, 28),
            RGB(89, 89, 89),
            RGB(215, 215, 215),
            accent,
            onAccent,
            Blend(RGB(245, 245, 245), accent, 12),
            false,
            false};
}

class WorkWindow final : public Window
{
public:
    WorkWindow(Appearance appearance, std::wstring label) : Window(appearance), _label(std::move(label))
    {
    }
    void Start(std::function<void()> action)
    {
        _worker = std::jthread([this, destination = _window, action = std::move(action)] {
            try
            {
                action();
            }
            catch (...)
            {
                _error = std::current_exception();
            }
            while (IsWindow(destination) && !PostMessageW(destination, WorkDone, 0, 0))
            {
                std::this_thread::sleep_for(20ms);
            }
        });
    }

    void Result()
    {
        _worker.join();
        if (_error)
        {
            std::rethrow_exception(_error);
        }
    }

private:
    SIZE PreferredSize(SIZE available) const override
    {
        return {
            std::min<LONG>(available.cx, std::max(_metrics.Character * 26, TextWidth(_label)) + _metrics.Margin * 2),
            _metrics.Margin * 2 + _metrics.Line * 2 + _metrics.Gap * 2};
    }
    void Create() override
    {
        _progress = Child(PROGRESS_CLASSW, L"", 1, PBS_MARQUEE);
        SendMessageW(_progress, PBM_SETMARQUEE, TRUE, 25);
    }

    void Layout(int width, int height) override
    {
        Place(_progress, _metrics.Margin, height - _metrics.Margin - _metrics.Gap, width - _metrics.Margin * 2,
              _metrics.Gap);
    }

    void Paint(HDC dc, const RECT&) override
    {
        RECT client{};
        GetClientRect(_window, &client);
        Text(dc, _label,
             {Px(_metrics.Margin), Px(_metrics.Gap), client.right - Px(_metrics.Margin),
              client.bottom - Px(_metrics.Margin + _metrics.Gap * 2)},
             false, false, DT_LEFT | DT_WORDBREAK);
    }

    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override
    {
        if (message == WM_CLOSE)
        {
            return 0;
        }
        if (message == WorkDone)
        {
            DestroyWindow(_window);
            return 0;
        }
        return Window::Message(message, wparam, lparam);
    }

    std::wstring _label;
    HWND _progress{};
    std::exception_ptr _error;
    std::jthread _worker;
};
} // namespace

Window::~Window()
{
    if (_window)
    {
        DestroyWindow(_window);
    }
    DeleteObject(_font);
    DeleteObject(_heading);
    DeleteObject(_section);
    DeleteObject(_monospace);
    DeleteObject(_icons);
    DeleteObject(_background);
    DeleteObject(_panel);
    DeleteObject(_field);
}

void Window::Open(std::wstring_view title, HWND owner, DWORD style, DWORD extended, HWND positionRelativeTo)
{
    static const auto registered = [] {
        WNDCLASSEXW window{};
        window.cbSize = sizeof(window);
        window.lpfnWndProc = Route;
        window.hInstance = GetModuleHandleW(nullptr);
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        const auto icon = MAKEINTRESOURCEW(IDI_Ia64Manager);
        window.hIcon = LoadIconW(window.hInstance, icon);
        window.hIconSm =
            static_cast<HICON>(LoadImageW(window.hInstance, icon, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        window.lpszClassName = WindowClass;
        return RegisterClassExW(&window);
    }();
    if (!registered)
    {
        WinError(LText(IDS_WindowClass));
    }

    RECT parent{};
    const auto anchor = positionRelativeTo ? positionRelativeTo : owner;
    if (!anchor || !GetWindowRect(anchor, &parent))
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO info{sizeof(info), {}, {}, 0};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info);
        parent = info.rcWork;
    }
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    GetMonitorInfoW(MonitorFromRect(&parent, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto work = monitor.rcWork;
    _dpi = anchor ? GetDpiForWindow(anchor) : GetDpiForSystem();
    ApplyFont();
    const auto size = PreferredSize({Dip(work.right - work.left), Dip(work.bottom - work.top)});
    RECT rect{0, 0, Px(size.cx), Px(size.cy)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, extended, _dpi);
    const auto w = std::min(rect.right - rect.left, work.right - work.left);
    const auto h = std::min(rect.bottom - rect.top, work.bottom - work.top);
    const auto left = std::clamp((parent.left + parent.right - w) / 2, work.left, work.right - w);
    const auto top = std::clamp((parent.top + parent.bottom - h) / 2, work.top, work.bottom - h);
    const auto name = std::wstring(title);
    if (!CreateWindowExW(extended, WindowClass, name.c_str(), style, left, top, w, h, owner, nullptr,
                         GetModuleHandleW(nullptr), this))
    {
        if (_createdError)
        {
            std::rethrow_exception(_createdError);
        }
        WinError(LText(IDS_WindowCreate));
    }
}

void Window::NotifyTextScale(double factor, DWORD uiThread)
{
    const auto scale = static_cast<int>(std::lround(factor * USER_DEFAULT_SCREEN_DPI));
    if (TextScaleDpi.exchange(scale) == scale)
    {
        return;
    }
    EnumThreadWindows(
        uiThread,
        [](HWND hwnd, LPARAM) -> BOOL {
            wchar_t name[32]{};
            if (GetClassNameW(hwnd, name, static_cast<int>(std::size(name))) && std::wstring_view(name) == WindowClass)
            {
                PostMessageW(hwnd, TextScaleChanged, 0, 0);
            }
            return TRUE;
        },
        0);
}

void Window::Modal()
{
    const auto owner = GetWindow(_window, GW_OWNER);
    const auto previousFocus = GetFocus();
    const bool wasEnabled = owner && IsWindowEnabled(owner);
    if (wasEnabled)
    {
        EnableWindow(owner, FALSE);
    }
    _modal = true;
    ShowWindow(_window, SW_SHOWNORMAL);
    if (!IsChild(_window, GetFocus()))
    {
        SetFocus(GetNextDlgTabItem(_window, nullptr, FALSE));
    }
    MSG message{};
    while (_window)
    {
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0)
        {
            if (result == 0)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            DestroyWindow(_window);
            break;
        }
        if (_window && TranslateKey(message))
        {
            continue;
        }
        const auto root = message.hwnd ? GetAncestor(message.hwnd, GA_ROOT) : nullptr;
        if (!root || (root != _window && message.hwnd == root) || !IsDialogMessageW(root, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    _modal = false;
    if (wasEnabled)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        if (IsChild(owner, previousFocus) && IsWindowVisible(previousFocus) && IsWindowEnabled(previousFocus))
        {
            SetFocus(previousFocus);
        }
    }
}

void Window::SetAppearance(Appearance appearance)
{
    _appearance = appearance;
    ApplyTheme();
    Refresh();
}

void Window::RefreshLanguage()
{
    ApplyFont();
    for (auto child = GetWindow(_window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        if (GetPropW(child, Properties))
        {
            Set(child, LText(IDS_Details));
            for (const auto& [index, label] : {std::pair{0, IDS_Property}, std::pair{1, IDS_Value}})
            {
                auto text = LText(label);
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT;
                column.pszText = text.data();
                ListView_SetColumn(child, index, &column);
            }
        }
    }
    LanguageChanged();
    Refresh();
}

bool Window::NextFormControl(std::span<const TabStop> stops, int& scroll, int viewport)
{
    const auto current = std::ranges::find(stops, GetFocus(), &TabStop::Control);
    if (current == stops.end())
    {
        return false;
    }
    const auto step = GetKeyState(VK_SHIFT) < 0 ? stops.size() - 1 : 1;
    auto index = static_cast<size_t>(current - stops.begin());
    do
    {
        index = (index + step) % stops.size();
    } while (!IsWindowEnabled(stops[index].Control) && stops[index].Control != current->Control);
    const auto next = stops[index];
    if (next.Top >= 0 && viewport > 0)
    {
        const int position = std::min(next.Top, std::max(scroll, next.Bottom - viewport));
        if (position != scroll)
        {
            scroll = position;
            Refresh();
        }
    }
    SendMessageW(_window, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(next.Control), TRUE);
    return true;
}

void Window::Place(HWND control, int x, int y, int width, int height) const
{
    if (GetPropW(control, FieldFrame))
    {
        x += _metrics.Unit;
        y += _metrics.Unit;
        width -= _metrics.Gap;
        height -= _metrics.Gap;
    }
    SetWindowPos(control, nullptr, Px(x), Px(y), Px(std::max(0, width)), Px(std::max(0, height)),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

int Window::TextWidth(std::wstring_view text, HWND control) const
{
    const auto dc = GetDC(_window);
    const auto old = SelectObject(dc, control ? ControlFont(control) : _font);
    SIZE extent{};
    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
    SelectObject(dc, old);
    ReleaseDC(_window, dc);
    return Dip(extent.cx);
}

int Window::ButtonWidth(HWND control) const
{
    const auto icon = GetPropW(control, L"Glyph") ? _metrics.Line + _metrics.Gap : 0;
    return std::max(_metrics.Control, TextWidth(Read(control), control) + icon + _metrics.Gap * 2);
}

int Window::ComboWidth(HWND control) const
{
    int widest = 0;
    const auto count = SendMessageW(control, CB_GETCOUNT, 0, 0);
    for (int index = 0; index < count; ++index)
    {
        const auto length = SendMessageW(control, CB_GETLBTEXTLEN, index, 0);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        SendMessageW(control, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        text.resize(length);
        widest = std::max(widest, TextWidth(text, control));
    }
    return widest + _metrics.Control + _metrics.Gap;
}

int Window::InputWidth(HWND control, int available) const
{
    wchar_t type[32]{};
    GetClassNameW(control, type, static_cast<int>(std::size(type)));
    const int compact = _metrics.Character * 9 + _metrics.Gap * 2;
    int wanted = available;
    if (_wcsicmp(type, WC_BUTTONW) == 0)
    {
        wanted = ButtonWidth(control);
    }
    else if (_wcsicmp(type, WC_COMBOBOXW) == 0)
    {
        wanted = std::max(compact, ComboWidth(control));
        SendMessageW(control, CB_SETDROPPEDWIDTH, Px(wanted), 0);
    }
    else if (_wcsicmp(type, WC_EDITW) == 0 && (GetWindowLongPtrW(control, GWL_STYLE) & ES_NUMBER))
    {
        const auto hint = _hints.find(control);
        wanted =
            hint == _hints.end() ? compact : std::max(compact, TextWidth(hint->second.Cue, control) + _metrics.Gap * 2);
    }
    return std::max(0, std::min(wanted, available));
}

LRESULT CALLBACK Window::EditHint(HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                  DWORD_PTR owner)
{
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(control, EditHint, id);
    }
    if ((message != WM_PAINT && message != WM_PRINTCLIENT) || GetWindowTextLengthW(control) != 0)
    {
        return DefSubclassProc(control, message, wparam, lparam);
    }
    auto& window = *reinterpret_cast<Window*>(owner);
    const auto hint = window._hints.find(control);
    if (hint == window._hints.end() || hint->second.Cue.empty())
    {
        return DefSubclassProc(control, message, wparam, lparam);
    }
    PAINTSTRUCT paint{};
    const auto dc = wparam ? reinterpret_cast<HDC>(wparam) : BeginPaint(control, &paint);
    RECT rect{};
    GetClientRect(control, &rect);
    FillRect(dc, &rect, window._field);
    const auto margins = SendMessageW(control, EM_GETMARGINS, 0, 0);
    rect.left += LOWORD(margins);
    rect.right -= HIWORD(margins);
    const auto previous = SelectObject(dc, window.ControlFont(control));
    SetTextColor(dc, window._colors.Muted);
    SetBkMode(dc, TRANSPARENT);
    const auto& text = hint->second.Cue;
    const auto ellipsis = text.contains(L'\\') ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS;
    DrawTextW(dc, text.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | ellipsis);
    SelectObject(dc, previous);
    if (!wparam)
    {
        EndPaint(control, &paint);
    }
    return 0;
}

void Window::DefaultHint(HWND control, std::wstring_view value, bool useWhenEmpty)
{
    if (!_tips)
    {
        _tips = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, _window, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        ThemeControl(_tips, _colors);
        SendMessageW(_tips, WM_SETFONT, reinterpret_cast<WPARAM>(_font), FALSE);
        SendMessageW(_tips, TTM_SETMAXTIPWIDTH, 0, Px(_metrics.PathWidth()));
    }
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = _window;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    SendMessageW(_tips, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    auto& hint = _hints[control].Default;
    hint = LFmt(IDS_DefaultValue, value);
    tool.lpszText = hint.data();
    SendMessageW(_tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    wchar_t type[32]{};
    GetClassNameW(control, type, static_cast<int>(std::size(type)));
    if (useWhenEmpty && _wcsicmp(type, WC_EDITW) == 0)
    {
        EditCue(control, hint);
    }
}

void Window::EditCue(HWND control, std::wstring_view text)
{
    auto& cue = _hints[control].Cue;
    cue = text;
    SendMessageW(control, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    SetWindowSubclass(control, EditHint, 1, reinterpret_cast<DWORD_PTR>(this));
}

std::wstring SystemThemeName()
{
    const auto system = Colors(Appearance::System);
    return LFmt(IDS_FollowSystem, LText(system.HighContrast ? IDS_HighContrast
                                        : system.Dark       ? IDS_ThemeDark
                                                            : IDS_ThemeLight));
}

void Window::PlaceFooter(int width, int height, std::initializer_list<HWND> buttons) const
{
    int edge = width - _metrics.Margin;
    for (const auto button : buttons)
    {
        if (button)
        {
            const int preferred = std::max(_metrics.Character * 8, ButtonWidth(button));
            edge -= preferred;
            Place(button, edge, height - _metrics.Gap - _metrics.Control, preferred, _metrics.Control);
            edge -= _metrics.Gap;
        }
    }
}

int Window::WrappedHeight(HWND label, int width) const
{
    const auto text = Read(label);
    const auto dc = GetDC(_window);
    const auto old = SelectObject(dc, ControlFont(label));
    RECT area{0, 0, Px(std::max(1, width)), 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &area, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(_window, dc);
    return std::max(_metrics.Line, Dip(area.bottom));
}

void Window::Text(HDC dc, std::wstring_view text, RECT rect, bool heading, bool muted, UINT options,
                  COLORREF color) const
{
    const auto previous = SelectObject(dc, heading ? _heading : _font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color != CLR_INVALID ? color : muted ? _colors.Muted : _colors.Text);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect, options | DT_NOPREFIX);
    SelectObject(dc, previous);
}

void Window::Fill(HDC dc, const RECT& rect, COLORREF color) const
{
    const auto brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Window::Rounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) const
{
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }
    const auto brush = CreateSolidBrush(fill);
    const auto pen = CreatePen(PS_SOLID, 1, border);
    const auto previousBrush = SelectObject(dc, brush);
    const auto previousPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Px(radius * 2), Px(radius * 2));
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void Window::Icon(HDC dc, wchar_t glyph, RECT bounds, COLORREF color) const
{
    const auto previous = SelectObject(dc, _icons);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, &glyph, 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous);
}

RECT Window::Rect(int left, int top, int right, int bottom) const
{
    return {Px(left), Px(top), Px(right), Px(bottom)};
}

HWND Window::Child(std::wstring_view type, std::wstring_view text, int id, DWORD style, DWORD extended)
{
    const auto control = CreateWindowExW(
        extended, std::wstring(type).c_str(), std::wstring(text).c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!control)
    {
        WinError(LText(IDS_ControlCreate));
    }
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(_font), FALSE);
    if (_colors.Dark || type == WC_TREEVIEWW)
    {
        ThemeControl(control, _colors);
    }
    return control;
}

HWND Window::Button(int id, std::wstring_view label, bool primary, wchar_t glyph)
{
    const auto button = Child(L"BUTTON", label, id, WS_TABSTOP | BS_OWNERDRAW);
    SetWindowSubclass(button, TrackButton, 1, 0);
    if (primary)
    {
        SetPropW(button, L"Primary", reinterpret_cast<HANDLE>(1));
    }
    if (glyph)
    {
        ButtonGlyph(button, glyph);
    }
    return button;
}

void Window::ButtonGlyph(HWND button, wchar_t glyph) const
{
    SetPropW(button, L"Glyph", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(glyph)));
    InvalidateRect(button, nullptr, FALSE);
}

HWND Window::Action(int id, std::wstring_view label, wchar_t glyph)
{
    const auto button = Button(id, label, false, glyph);
    SetPropW(button, FlatButton, reinterpret_cast<HANDLE>(1));
    return button;
}

HWND Window::OnPanel(HWND control)
{
    SetPropW(control, PanelSurface, reinterpret_cast<HANDLE>(1));
    return control;
}

HWND Window::Label(std::wstring_view text, TextRole role, DWORD style)
{
    const auto label = Child(L"STATIC", text, -1, style | SS_NOPREFIX);
    UseFont(label, role);
    return label;
}

void Window::UseFont(HWND control, TextRole role) const
{
    SetPropW(control, FontRole, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(role)));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ControlFont(control)), FALSE);
}

HWND Window::Edit(int id, std::wstring_view text, DWORD extra)
{
    const auto edit = Child(L"EDIT", text, id, WS_TABSTOP | extra);
    SetPropW(edit, FieldFrame, reinterpret_cast<HANDLE>(1));
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, EC_USEFONTINFO);
    return edit;
}

HWND Window::Combo(int id)
{
    const auto combo =
        Child(WC_COMBOBOXW, L"", id, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS);
    SendMessageW(combo, CB_SETMINVISIBLE, 9, 0);
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, Px(_metrics.Line + _metrics.Unit));
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(_metrics.Line + _metrics.Unit));
    return combo;
}

HWND Window::PropertyList(int id)
{
    const auto list = Child(WC_LISTVIEWW, LText(IDS_Details), id,
                            WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS);
    SetPropW(list, Properties, reinterpret_cast<HANDLE>(1));
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = Px(_metrics.Character * 14);
    for (const auto label : {IDS_Property, IDS_Value})
    {
        auto name = LText(label);
        column.pszText = name.data();
        ListView_InsertColumn(list, label == IDS_Property ? 0 : 1, &column);
    }
    ListView_EnableGroupView(list, TRUE);
    return list;
}

void Window::PlacePropertyList(HWND list, int x, int y, int width, int height) const
{
    Place(list, x, y, width, height);
    ListView_SetBkColor(list, _colors.Background);
    ListView_SetTextBkColor(list, _colors.Background);
    ListView_SetTextColor(list, _colors.Text);
    LVGROUPMETRICS metrics{};
    metrics.cbSize = sizeof(metrics);
    metrics.mask = LVGMF_BORDERSIZE;
    metrics.Top = Px(_metrics.Gap);
    metrics.Bottom = Px(_metrics.Gap);
    ListView_SetGroupMetrics(list, &metrics);
    RECT client{};
    GetClientRect(list, &client);
    int preferred = 0;
    for (int index = 0, count = ListView_GetItemCount(list); index < count; ++index)
    {
        wchar_t label[256]{};
        ListView_GetItemText(list, index, 0, label, static_cast<int>(std::size(label)));
        preferred = std::max(preferred, TextWidth(label));
    }
    const int key = std::min(Px(preferred + _metrics.Gap * 3), static_cast<int>(client.right) * 46 / 100);
    ListView_SetColumnWidth(list, 0, key);
    ListView_SetColumnWidth(list, 1, std::max(0L, client.right - key));
}

void Window::Refresh()
{
    if (!_window)
    {
        return;
    }
    RECT client{};
    GetClientRect(_window, &client);
    Layout(Dip(client.right), Dip(client.bottom));
    RedrawWindow(_window, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void Window::ApplyTheme()
{
    _colors = Colors(_appearance);
    DeleteObject(_background);
    DeleteObject(_panel);
    DeleteObject(_field);
    _background = CreateSolidBrush(_colors.Background);
    _panel = CreateSolidBrush(_colors.Panel);
    _field = CreateSolidBrush(_colors.Field);
    const BOOL dark = _colors.Dark;
    DwmSetWindowAttribute(_window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const auto corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    for (auto child = GetWindow(_window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        ThemeControl(child, _colors);
    }
    if (_tips)
    {
        ThemeControl(_tips, _colors);
    }
}

HFONT Window::ControlFont(HWND control) const
{
    switch (static_cast<TextRole>(reinterpret_cast<UINT_PTR>(GetPropW(control, FontRole))))
    {
    case TextRole::Title:
        return _heading;
    case TextRole::Section:
        return _section;
    case TextRole::Monospace:
        return _monospace;
    default:
        return _font;
    }
}

void Window::ApplyFont()
{
    _layoutDpi = MulDiv(_dpi, TextScaleDpi.load(), USER_DEFAULT_SCREEN_DPI);
    const HFONT previous[]{_font, _heading, _section, _monospace, _icons};
    const auto font = [this](int size, int weight, LPCWSTR face) {
        return CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    const bool japanese = PRIMARYLANGID(GetThreadUILanguage()) == LANG_JAPANESE;
    const auto body = japanese ? L"Yu Gothic UI" : L"Segoe UI Variable Text";
    _font = font(14, FW_NORMAL, body);
    _section = font(16, FW_SEMIBOLD, body);
    _monospace = font(14, FW_NORMAL, L"Consolas");
    _heading = font(24, FW_SEMIBOLD, japanese ? body : L"Segoe UI Variable Display");
    _icons = font(16, FW_NORMAL, L"Segoe Fluent Icons");
    const auto dc = GetDC(_window);
    const auto oldFont = SelectObject(dc, _font);
    TEXTMETRICW text{};
    GetTextMetricsW(dc, &text);
    const auto line = std::max(1, Dip(text.tmHeight));
    const auto unit = std::max(1, (line + 2) / 4);
    _metrics = {line,           std::max(1, Dip(text.tmAveCharWidth)), unit, unit * 2, unit * 4, line + unit * 2,
                line + unit * 4};
    SelectObject(dc, oldFont);
    ReleaseDC(_window, dc);
    for (auto child = _window ? GetWindow(_window, GW_CHILD) : nullptr; child; child = GetWindow(child, GW_HWNDNEXT))
    {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(ControlFont(child)), TRUE);
        wchar_t type[32]{};
        GetClassNameW(child, type, static_cast<int>(std::size(type)));
        if (_wcsicmp(type, WC_COMBOBOXW) == 0)
        {
            SendMessageW(child, CB_SETITEMHEIGHT, 0, Px(_metrics.Line + _metrics.Unit));
            SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(_metrics.Line + _metrics.Unit));
        }
    }
    for (const auto old : previous)
    {
        DeleteObject(old);
    }
    if (_tips)
    {
        SendMessageW(_tips, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
        SendMessageW(_tips, TTM_SETMAXTIPWIDTH, 0, Px(_metrics.PathWidth()));
    }
}

void Window::DrawButton(const DRAWITEMSTRUCT& item) const
{
    const auto bound = item.rcItem;
    const bool primary = GetPropW(item.hwndItem, L"Primary") != nullptr;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = !disabled && GetPropW(item.hwndItem, Hovered);
    const auto glyph = static_cast<wchar_t>(reinterpret_cast<UINT_PTR>(GetPropW(item.hwndItem, L"Glyph")));
    const bool flat = GetPropW(item.hwndItem, FlatButton) != nullptr;
    const auto background = GetPropW(item.hwndItem, PanelSurface) ? _colors.Panel : _colors.Background;
    const auto foreground = disabled  ? (_colors.HighContrast ? GetSysColor(COLOR_GRAYTEXT) : _colors.Muted)
                            : primary ? _colors.OnAccent
                                      : _colors.Text;
    Fill(item.hDC, bound, background);
    if (flat)
    {
        if (hovered || pressed)
        {
            const auto color = pressed && !_colors.HighContrast ? _colors.Border : _colors.Selection;
            Rounded(item.hDC, bound, 4, color, color);
        }
    }
    else
    {
        const auto base = primary && !disabled ? _colors.Accent : _colors.Field;
        const auto surface = _colors.HighContrast || disabled ? base
                                                              : Blend(base, foreground,
                                                                      pressed   ? 16
                                                                      : hovered ? 7
                                                                                : 0);
        Rounded(item.hDC, bound, 4, surface, primary && !disabled ? surface : _colors.Border);
    }
    const auto ink = flat && _colors.HighContrast && (hovered || pressed) ? _colors.OnAccent : foreground;
    const auto label = Read(item.hwndItem);
    const auto previous = SelectObject(item.hDC, _font);
    SetTextColor(item.hDC, ink);
    SetBkMode(item.hDC, TRANSPARENT);
    auto labelArea = bound;
    InflateRect(&labelArea, -Px(_metrics.Unit), 0);
    if (glyph)
    {
        SIZE size{};
        GetTextExtentPoint32W(item.hDC, label.c_str(), static_cast<int>(label.size()), &size);
        const auto glyphArea = Px(_metrics.Line + _metrics.Gap);
        const auto left = flat ? bound.left + Px(_metrics.Gap)
                               : std::max(labelArea.left, (bound.left + bound.right - size.cx - glyphArea) / 2);
        Icon(item.hDC, glyph, {left, bound.top, left + Px(_metrics.Line), bound.bottom}, ink);
        labelArea.left = left + glyphArea;
    }
    DrawTextW(item.hDC, label.c_str(), static_cast<int>(label.size()), &labelArea,
              (glyph ? DT_LEFT : DT_CENTER) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, previous);
    if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
    {
        auto inset = bound;
        InflateRect(&inset, -Px(3), -Px(3));
        DrawFocusRect(item.hDC, &inset);
    }
}

void Window::DrawFields(HDC dc) const
{
    for (auto child = GetWindow(_window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        if (!GetPropW(child, FieldFrame) || !IsWindowVisible(child))
        {
            continue;
        }
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(HWND_DESKTOP, _window, reinterpret_cast<POINT*>(&bounds), 2);
        InflateRect(&bounds, Px(_metrics.Unit), Px(_metrics.Unit));
        Rounded(dc, bounds, 4, _colors.Field, GetFocus() == child ? _colors.Accent : _colors.Border);
    }
}

void Window::DrawChoice(const DRAWITEMSTRUCT& item) const
{
    const bool selection = (item.itemState & ODS_SELECTED) && !(item.itemState & ODS_COMBOBOXEDIT);
    Fill(item.hDC, item.rcItem, selection ? _colors.Accent : _colors.Field);
    const auto length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
    if (length != CB_ERR)
    {
        std::wstring value(length + 1, L'\0');
        SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(value.data()));
        const auto previous = SelectObject(item.hDC, _font);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, selection                         ? _colors.OnAccent
                               : (item.itemState & ODS_DISABLED) ? _colors.Muted
                                                                 : _colors.Text);
        auto textArea = item.rcItem;
        textArea.left += Px(_metrics.Unit);
        textArea.right -= Px(_metrics.Unit);
        DrawTextW(item.hDC, value.c_str(), static_cast<int>(length), &textArea,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item.hDC, previous);
    }
    if (item.itemState & ODS_FOCUS)
    {
        DrawFocusRect(item.hDC, &item.rcItem);
    }
}

LRESULT Window::DrawProperties(NMLVCUSTOMDRAW& draw) const
{
    const auto list = draw.nmcd.hdr.hwndFrom;
    if (draw.dwItemType == LVCDI_GROUP && draw.nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        const int id = static_cast<int>(draw.nmcd.dwItemSpec);
        wchar_t label[128]{};
        LVGROUP group{};
        group.cbSize = sizeof(group);
        group.mask = LVGF_HEADER | LVGF_STATE;
        group.stateMask = LVGS_FOCUSED;
        group.pszHeader = label;
        group.cchHeader = static_cast<int>(std::size(label));
        RECT rect{};
        if (ListView_GetGroupInfo(list, id, &group) == -1 || !ListView_GetGroupRect(list, id, LVGGR_HEADER, &rect))
        {
            return CDRF_DODEFAULT;
        }
        const auto dc = draw.nmcd.hdc;
        Fill(dc, rect, _colors.Background);
        rect.left += Px(3);
        const auto font = SelectObject(dc, _section);
        SetTextColor(dc, _colors.Text);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, group.pszHeader, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, font);
        if ((group.state & LVGS_FOCUSED) && GetFocus() == list &&
            !(SendMessageW(list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        {
            DrawFocusRect(dc, &rect);
        }
        return CDRF_SKIPDEFAULT;
    }
    switch (draw.nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
        if (!(draw.nmcd.uItemState & CDIS_SELECTED))
        {
            draw.clrText = draw.iSubItem == 0 ? _colors.Muted : _colors.Text;
        }
        break;
    }
    return CDRF_DODEFAULT;
}

LRESULT CALLBACK Window::Route(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->_window = hwnd;
        self->_dpi = GetDpiForWindow(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    try
    {
        const auto result = self->Dispatch(message, wparam, lparam);
        if (message == WM_NCDESTROY)
        {
            self->_window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return result;
    }
    catch (const std::exception& error)
    {
        if (message == WM_CREATE)
        {
            self->_creationFailed = true;
            self->_createdError = std::current_exception();
            return -1;
        }
        ShowError(hwnd, Wide(error.what()));
        return 0;
    }
}

LRESULT Window::Dispatch(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_PARENTNOTIFY:
        if (LOWORD(wparam) == WM_DESTROY)
        {
            const auto child = reinterpret_cast<HWND>(lparam);
            if (_hints.contains(child))
            {
                TOOLINFOW tool{sizeof(tool)};
                tool.hwnd = _window;
                tool.uFlags = TTF_IDISHWND;
                tool.uId = reinterpret_cast<UINT_PTR>(child);
                SendMessageW(_tips, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
                _hints.erase(child);
            }
        }
        break;
    case WM_NEXTDLGCTL: {
        const auto target =
            lparam ? reinterpret_cast<HWND>(wparam) : GetNextDlgTabItem(_window, GetFocus(), wparam != 0);
        if (target && IsChild(_window, target))
        {
            SetFocus(target);
            if (SendMessageW(target, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL)
            {
                SendMessageW(target, EM_SETSEL, 0, -1);
            }
        }
        return 0;
    }
    case DM_GETDEFID:
        if (_modal)
        {
            for (auto child = GetWindow(_window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                if (GetPropW(child, L"Primary") && IsWindowVisible(child) && IsWindowEnabled(child))
                {
                    return MAKELRESULT(GetDlgCtrlID(child), DC_HASDEFID);
                }
            }
        }
        break;
    case WM_CREATE:
        ApplyFont();
        ApplyTheme();
        Create();
        return 0;
    case WM_SIZE:
        if (!_creationFailed && wparam != SIZE_MINIMIZED)
        {
            Refresh();
        }
        return 0;
    case WM_DPICHANGED: {
        _dpi = HIWORD(wparam);
        ApplyFont();
        const auto& rect = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(_window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Refresh();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        const auto min = MinimumSize();
        const auto target = reinterpret_cast<MINMAXINFO*>(lparam);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(_window, MONITOR_DEFAULTTONEAREST), &monitor);
        target->ptMinTrackSize = {std::min<LONG>(Px(min.x), monitor.rcWork.right - monitor.rcWork.left),
                                  std::min<LONG>(Px(min.y), monitor.rcWork.bottom - monitor.rcWork.top)};
        return 0;
    }
    case TextScaleChanged: {
        const auto previous = _layoutDpi;
        ApplyFont();
        if (_layoutDpi != previous && !IsZoomed(_window) && !IsIconic(_window))
        {
            RECT rect{};
            GetWindowRect(_window, &rect);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(_window, MONITOR_DEFAULTTONEAREST), &monitor);
            const auto area = monitor.rcWork;
            const LONG w = std::min<LONG>(area.right - area.left, MulDiv(rect.right - rect.left, _layoutDpi, previous));
            const LONG h = std::min<LONG>(area.bottom - area.top, MulDiv(rect.bottom - rect.top, _layoutDpi, previous));
            SetWindowPos(_window, nullptr, std::clamp(rect.left, area.left, area.right - w),
                         std::clamp(rect.top, area.top, area.bottom - h), w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Refresh();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyTheme();
        Refresh();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(_window, &paint);
        HDC buffer{};
        const auto buffered = BeginBufferedPaint(dc, &paint.rcPaint, BPBF_COMPATIBLEBITMAP, nullptr, &buffer);
        const auto target = buffered ? buffer : dc;
        FillRect(target, &paint.rcPaint, _background);
        Paint(target, paint.rcPaint);
        DrawFields(target);
        if (buffered)
        {
            EndBufferedPaint(buffered, TRUE);
        }
        EndPaint(_window, &paint);
        return 0;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item.CtlType == ODT_BUTTON)
        {
            DrawButton(item);
            return TRUE;
        }
        if (item.CtlType == ODT_COMBOBOX)
        {
            DrawChoice(item);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto header = reinterpret_cast<NMHDR*>(lparam);
        if (header->code == NM_CUSTOMDRAW && GetPropW(header->hwndFrom, Properties))
        {
            return DrawProperties(*reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
        }
        break;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS)
        {
            const auto edit = reinterpret_cast<HWND>(lparam);
            if (edit && GetPropW(edit, FieldFrame))
            {
                RECT bounds{};
                GetWindowRect(edit, &bounds);
                MapWindowPoints(HWND_DESKTOP, _window, reinterpret_cast<POINT*>(&bounds), 2);
                InflateRect(&bounds, Px(_metrics.Unit), Px(_metrics.Unit));
                InvalidateRect(_window, &bounds, FALSE);
            }
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const auto control = reinterpret_cast<HWND>(lparam);
        const bool panel = GetPropW(control, PanelSurface) != nullptr;
        const bool field = message != WM_CTLCOLORSTATIC || GetPropW(control, FieldFrame);
        const auto role = static_cast<TextRole>(reinterpret_cast<UINT_PTR>(GetPropW(control, FontRole)));
        SetTextColor(dc, role == TextRole::Secondary ? _colors.Muted : _colors.Text);
        SetBkColor(dc, panel ? _colors.Panel : field ? _colors.Field : _colors.Background);
        return reinterpret_cast<LRESULT>(panel ? _panel : field ? _field : _background);
    }
    }
    return Message(message, wparam, lparam);
}

LRESULT Window::Message(UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(_window, message, wparam, lparam);
}

std::wstring Read(HWND control)
{
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const auto count = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(count);
    return text;
}

void Set(HWND control, std::wstring_view text)
{
    SetWindowTextW(control, std::wstring(text).c_str());
}

void Enable(HWND control, bool enabled)
{
    EnableWindow(control, enabled);
}

void Options(HWND combo, const std::vector<std::wstring>& items, std::wstring_view selected)
{
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int index = 0;
    for (const auto& item : items)
    {
        const auto row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        if (item == selected && row >= 0)
        {
            index = static_cast<int>(row);
        }
    }
    SendMessageW(combo, CB_SETCURSEL, index, 0);
}

std::wstring Selection(HWND combo)
{
    const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR)
    {
        return {};
    }
    const auto length = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
    if (length == CB_ERR)
    {
        return {};
    }
    std::wstring value(length + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(value.data()));
    value.resize(length);
    return value;
}

unsigned Number(HWND edit, std::wstring_view name, unsigned maximum, bool optional)
{
    const auto text = Trim(Read(edit));
    if (text.empty() && optional)
    {
        return 0;
    }
    if (text.empty() || text.size() > 10 || text.find_first_not_of(L"0123456789") != std::wstring::npos)
    {
        Fail(LFmt(IDS_NumberRequired, name));
    }
    const auto value = std::stoull(text);
    if (value > maximum || (!optional && !value))
    {
        Fail(LFmt(IDS_NumberRange, name, optional ? 0 : 1, maximum));
    }
    return static_cast<unsigned>(value);
}

Fs::path ChooseFile(HWND parent, std::wstring_view title, std::wstring_view filterName, std::wstring_view pattern,
                    FileChoice mode, const Fs::path& initial)
{
    const bool save = mode == FileChoice::CreateDisk;
    IFileDialog* raw = nullptr;
    const auto created = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw));
    if (FAILED(created))
    {
        Fail(LText(IDS_PickerOpen));
    }
    std::unique_ptr<IFileDialog, ReleaseCom<IFileDialog>> dialog(raw);
    const std::wstring filter(filterName), extension(pattern), name(title);
    const auto allFiles = LText(IDS_AllFiles);
    const COMDLG_FILTERSPEC types[]{{filter.c_str(), extension.c_str()}, {allFiles.c_str(), L"*.*"}};
    dialog->SetTitle(name.c_str());
    dialog->SetFileTypes(static_cast<UINT>(std::size(types)), types);
    if (save)
    {
        dialog->SetDefaultExtension(L"qcow2");
    }
    if (!initial.empty())
    {
        if (initial.has_parent_path())
        {
            IShellItem* rawFolder{};
            if (SUCCEEDED(
                    SHCreateItemFromParsingName(initial.parent_path().c_str(), nullptr, IID_PPV_ARGS(&rawFolder))))
            {
                std::unique_ptr<IShellItem, ReleaseCom<IShellItem>> folder(rawFolder);
                dialog->SetFolder(folder.get());
            }
        }
        dialog->SetFileName(initial.filename().c_str());
    }
    DWORD flags{};
    dialog->GetOptions(&flags);
    flags |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (mode == FileChoice::Open)
    {
        flags |= FOS_FILEMUSTEXIST;
    }
    else
    {
        flags = (flags & ~FOS_FILEMUSTEXIST) | FOS_NOREADONLYRETURN;
    }
    dialog->SetOptions(flags);
    const auto result = dialog->Show(parent);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        return {};
    }
    if (FAILED(result))
    {
        Fail(LText(IDS_PickerSelect));
    }
    IShellItem* rawItem = nullptr;
    if (FAILED(dialog->GetResult(&rawItem)))
    {
        Fail(LText(IDS_PickerResult));
    }
    std::unique_ptr<IShellItem, ReleaseCom<IShellItem>> item(rawItem);
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
    {
        Fail(LText(IDS_PickerPath));
    }
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> pathOwner(path, CoTaskMemFree);
    return Fs::path(path);
}

void ShowError(HWND owner, std::wstring_view message)
{
    const auto newline = message.find(L'\n');
    const auto summary = Trim(message.substr(0, newline));
    const auto details = newline == std::wstring_view::npos ? L"" : Trim(message.substr(newline + 1));
    const auto title = LText(IDS_App);
    const auto detailsLabel = LText(IDS_Details);
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = owner;
    dialog.pszWindowTitle = title.c_str();
    dialog.pszMainIcon = TD_ERROR_ICON;
    dialog.pszContent = summary.c_str();
    dialog.dwFlags =
        TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_EXPAND_FOOTER_AREA | TDF_SIZE_TO_CONTENT;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    if (!details.empty())
    {
        dialog.pszExpandedInformation = details.c_str();
        dialog.pszExpandedControlText = detailsLabel.c_str();
        dialog.pszCollapsedControlText = detailsLabel.c_str();
    }
    if (FAILED(TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr)))
    {
        MessageBoxW(owner, std::wstring(message).c_str(), title.c_str(), MB_OK | MB_ICONERROR);
    }
}

void Busy(HWND owner, Appearance appearance, std::wstring_view message, std::function<void()> action)
{
    WorkWindow progress(appearance, std::wstring(message));
    progress.Open(LText(IDS_App), owner, WS_CAPTION | WS_POPUP);
    progress.Start(std::move(action));
    progress.Modal();
    progress.Result();
}
} // namespace Ia64::Ui
