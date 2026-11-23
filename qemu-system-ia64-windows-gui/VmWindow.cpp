// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "VmWindow.h"
#include "Wizard.h"
#include "Devices.h"
#include <format>
#include <sstream>
#include "Localization.h"

#include <Windowsx.h>
#include <imm.h>
#include <ime.h>
#include <algorithm>
#include <array>
#include <memory>
#include <type_traits>

namespace Ia64::Ui
{
namespace
{
enum Id
{
    Pause = 200,
    Reset,
    Shutdown,
    Insert,
    Eject,
    Stop,
    Toolbar = 209,
    MachineMenu,
    DisplayMenu,
    DeviceMenu,
    FitDisplay,
    NativeDisplay,
    ShowMonitor,
    CtrlAltDelete,
    SaveAndClose,
    ShowSerial,
    ShowDebug,
    NextForcePs2,
    NextUsbMouse,
    UseDefaultDisplay,
    GuestAutoResize,
    ViewConfiguration
};
constexpr std::array Tools{std::pair{MachineMenu, IDS_MachineMenu}, std::pair{DisplayMenu, IDS_DisplayMenu},
                           std::pair{DeviceMenu, IDS_DeviceMenu}};
void MenuItem(HMENU menu, UINT_PTR command, int label, bool enabled = true, bool checked = false)
{
    AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED) | (checked ? MF_CHECKED : MF_UNCHECKED), command,
                LText(label).c_str());
}
constexpr auto Timer = 1u;
constexpr UINT RevealFailure = WM_APP + 3;
constexpr UINT GuestKey = WM_APP + 6;
thread_local VmWindow* KeyboardOwner{};

unsigned KeySym(WPARAM key, LPARAM flags)
{
    const bool extended = (flags & (1 << 24)) != 0;
    const auto scan = (flags >> 16) & 0xff;
    if (scan == 0x29 && (key == VK_KANJI || key == VK_DBE_DBCSCHAR || key == VK_DBE_SBCSCHAR ||
                         PRIMARYLANGID(LOWORD(reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)))) == LANG_JAPANESE))
    {
        return 0xff2a; // Zenkaku_Hankaku
    }
    if (key >= VK_F1 && key <= VK_F24)
    {
        return 0xffbe + static_cast<unsigned>(key - VK_F1);
    }
    if (key >= 'A' && key <= 'Z')
    {
        return static_cast<unsigned>(key - 'A' + 'a');
    }
    if (key >= '0' && key <= '9')
    {
        return static_cast<unsigned>(key);
    }

    switch (key)
    {
    case VK_BACK:
        return 0xff08;
    case VK_TAB:
        return 0xff09;
    case VK_RETURN:
        return extended ? 0xff8d : 0xff0d;
    case VK_ESCAPE:
        return 0xff1b;
    case VK_SPACE:
        return 0x20;
    case VK_HOME:
        return 0xff50;
    case VK_LEFT:
        return 0xff51;
    case VK_UP:
        return 0xff52;
    case VK_RIGHT:
        return 0xff53;
    case VK_DOWN:
        return 0xff54;
    case VK_PRIOR:
        return 0xff55;
    case VK_NEXT:
        return 0xff56;
    case VK_END:
        return 0xff57;
    case VK_INSERT:
        return 0xff63;
    case VK_DELETE:
        return 0xffff;
    case VK_KANJI:
        return 0xff21;
    case VK_NONCONVERT:
        return 0xff22;
    case VK_CONVERT:
        return 0xff23;
    case VK_KANA:
        return 0xff27;
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_SHIFT:
        return ((flags >> 16) & 0xff) == 0x36 ? 0xffe2 : 0xffe1;
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_CONTROL:
        return extended ? 0xffe4 : 0xffe3;
    case VK_LMENU:
    case VK_RMENU:
    case VK_MENU:
        return extended ? 0xffea : 0xffe9;
    case VK_CAPITAL:
        return 0xffe5;
    case VK_LWIN:
        return 0xffeb;
    case VK_RWIN:
        return 0xffec;
    case VK_PAUSE:
        return 0xff13;
    case VK_SNAPSHOT:
        return 0xff61;
    case VK_NUMLOCK:
        return 0xff7f;
    default:
        return 0;
    }
}
} // namespace

int StatusText(VmStatus status)
{
    switch (status)
    {
    case VmStatus::Starting:
        return IDS_VmStarting;
    case VmStatus::Restoring:
        return IDS_RestoringSnapshot;
    case VmStatus::Running:
        return IDS_VmRunning;
    case VmStatus::Paused:
        return IDS_VmPaused;
    case VmStatus::Saving:
        return IDS_SavingSnapshot;
    case VmStatus::Stopped:
        return IDS_VmStopped;
    case VmStatus::Failed:
        return IDS_VmFailed;
    }
    return IDS_VmStopped;
}

VmWindow::VmWindow(HWND manager, const InterfaceSettings& settings, Profile profile, const Machine& machine,
                   const Capabilities& capabilities, ProcessCommand command, VmConnections connections,
                   std::optional<VmSnapshot> resume, Engine engine, std::function<void(const Profile&)> saveProfile)
    : Window(settings.Theme), _manager(manager), _profile(std::move(profile)),
      _nvram(machine.Properties.contains(L"nvram") ? NvramPath(_profile) : Fs::path{}), _engine(std::move(engine)),
      _machine(machine), _capabilities(capabilities), _defaultDisplay(settings.DefaultDisplay),
      _closeOnShutdown(settings.CloseOnShutdown), _hasPs2(machine.Properties.contains(L"i8042")),
      _hasVideo(EffectiveVga(_profile, machine) != L"none"), _saveProfile(std::move(saveProfile)),
      _session(std::move(command), std::move(connections), std::move(resume), _profile, _hasVideo)
{
}

bool VmWindow::UsesNvram(const Fs::path& path) const
{
    const auto status = _session.Status();
    return status != VmStatus::Stopped && status != VmStatus::Failed && SameFilePath(_nvram, path);
}

void VmWindow::ViewSettings(HWND owner) const
{
    // All inputs are copied before entering the modal loop. The guest may shut down while it is open.
    ShowRunningProfile(owner, _appearance, _engine, _capabilities, _machine, _profile, _session.CommandLine());
}

void VmWindow::SetInterface(const InterfaceSettings& settings)
{
    const auto previous = Display();
    _defaultDisplay = settings.DefaultDisplay;
    _closeOnShutdown = settings.CloseOnShutdown;
    if (!_closeOnShutdown)
    {
        _pendingShutdownClose = false;
    }
    if (previous != Display())
    {
        _resizePending = true;
    }
    SetAppearance(settings.Theme);
}

void VmWindow::LanguageChanged()
{
    for (const auto& [command, label] : Tools)
    {
        auto text = LText(label);
        TBBUTTONINFOW info{sizeof(info), TBIF_TEXT};
        info.pszText = text.data();
        SendMessageW(_toolbar, TB_SETBUTTONINFOW, command, reinterpret_cast<LPARAM>(&info));
    }
    Set(_runStatus, LText(StatusText(_status)));
    Set(_captureHint, LText(IDS_CaptureHelp));
    if (_frame)
    {
        Set(_resolution, LFmt(IDS_ResolutionValue, _frame->Width, _frame->Height));
    }
    for (const auto window : {_monitor.get(), _serial.get(), _debug.get()})
    {
        if (!window || !window->Get())
        {
            continue;
        }
        const auto title = window == _monitor.get()
                               ? LFmt(IDS_MonitorTitle, _profile.Name)
                               : _profile.Name + L" – " + LText(window == _serial.get() ? IDS_Serial : IDS_DebugPort);
        Set(window->Get(), title);
        window->RefreshLanguage();
    }
}

void VmWindow::Create()
{
    // The guest receives physical keys. Host text fields retain their own input contexts.
    ImmAssociateContext(_window, nullptr);
    _toolbar = Child(TOOLBARCLASSNAMEW, L"", Toolbar,
                     WS_TABSTOP | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER | TBSTYLE_FLAT | TBSTYLE_LIST |
                         TBSTYLE_TOOLTIPS);
    SendMessageW(_toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(_toolbar, TB_SETEXTENDEDSTYLE, 0,
                 TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DOUBLEBUFFER | TBSTYLE_EX_DRAWDDARROWS);
    SendMessageW(_toolbar, TB_SETMAXTEXTROWS, 1, 0);
    SendMessageW(_toolbar, TB_SETBITMAPSIZE, 0, MAKELPARAM(0, 0));
    for (const auto& [command, label] : Tools)
    {
        const auto text = LText(label);
        TBBUTTON tool{};
        tool.iBitmap = I_IMAGENONE;
        tool.idCommand = command;
        tool.fsState = TBSTATE_ENABLED;
        tool.fsStyle = BTNS_WHOLEDROPDOWN | BTNS_SHOWTEXT;
        tool.iString = reinterpret_cast<INT_PTR>(text.c_str());
        if (!SendMessageW(_toolbar, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&tool)))
        {
            WinError(L"TB_ADDBUTTONS");
        }
    }
    _runStatus = OnPanel(Label(LText(StatusText(_status)), TextRole::Body, SS_CENTERIMAGE));
    _resolution = OnPanel(Label(L"", TextRole::Secondary, SS_RIGHT | SS_CENTERIMAGE));
    _captureHint = OnPanel(Label(LText(IDS_CaptureHelp), TextRole::Body, SS_CENTERIMAGE | SS_ENDELLIPSIS));
    ShowWindow(_captureHint, SW_HIDE);
    SetTimer(_window, Timer, 30, nullptr);
}

int VmWindow::LayoutToolbar(int width) const
{
    const auto dc = GetDC(_toolbar);
    const auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(_toolbar, WM_GETFONT, 0, 0)));
    TEXTMETRICW font{};
    GetTextMetricsW(dc, &font);
    SendMessageW(_toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(Px(_metrics.Line), font.tmHeight + Px(_metrics.Unit)));
    for (const auto& [command, label] : Tools)
    {
        const auto text = LText(label);
        SIZE extent{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
        TBBUTTONINFOW info{sizeof(info), TBIF_SIZE};
        info.cx = static_cast<WORD>(extent.cx + Px(_metrics.Line + _metrics.Gap));
        SendMessageW(_toolbar, TB_SETBUTTONINFOW, command, reinterpret_cast<LPARAM>(&info));
    }
    SelectObject(dc, oldFont);
    ReleaseDC(_toolbar, dc);
    SendMessageW(_toolbar, TB_AUTOSIZE, 0, 0);
    RECT button{};
    SendMessageW(_toolbar, TB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&button));
    SetWindowPos(_toolbar, nullptr, Px(_metrics.Unit), 0, Px(width - _metrics.Gap), button.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return button.bottom + Px(1);
}

SIZE VmWindow::PreferredSize(SIZE available) const
{
    const int guestWidth = _profile.Width ? _profile.Width : DefaultDisplayWidth;
    const int guestHeight = _profile.Height ? _profile.Height : DefaultDisplayHeight;
    const int chrome = _metrics.Control * 2;
    const int height = std::min<int>(Dip(guestHeight), available.cy * 4 / 5 - chrome);
    const int width = std::min<int>(available.cx * 4 / 5, MulDiv(height, guestWidth, guestHeight));
    return {width, MulDiv(width, guestHeight, guestWidth) + chrome};
}

void VmWindow::Layout(int width, int height)
{
    _width = width;
    _height = height;
    const int statusTop = height - _metrics.Control;
    const int gap = _metrics.Gap;
    const int stateWidth = TextWidth(Read(_runStatus)) + gap;
    const int resolutionWidth = TextWidth(Read(_resolution)) + gap;
    Place(_runStatus, gap, statusTop + 1, stateWidth, _metrics.Control - 1);
    Place(_captureHint, stateWidth + gap * 2, statusTop + 1, width - stateWidth - resolutionWidth - gap * 4,
          _metrics.Control - 1);
    Place(_resolution, width - resolutionWidth - gap, statusTop + 1, resolutionWidth, _metrics.Control - 1);
    GetClientRect(_window, &_view);
    _view.top = LayoutToolbar(width);
    const auto statusHeight = Px(_metrics.Control);
    _view.bottom = std::max<LONG>(_view.top, _view.bottom - statusHeight);
    const int chrome = _view.top + statusHeight;
    if (std::exchange(_chromeHeight, chrome) != chrome)
    {
        _resizePending = true;
    }
    RefreshHostCursor();
}

void VmWindow::ResizeToGuest()
{
    const auto display = Display();
    if (!_resizePending || !_frame || display.FitToWindow || !display.ResizeToGuest || IsIconic(_window) ||
        IsZoomed(_window))
    {
        return;
    }
    RECT outer{}, client{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetWindowRect(_window, &outer) || !GetClientRect(_window, &client) ||
        !GetMonitorInfoW(MonitorFromWindow(_window, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        return;
    }

    const auto& work = monitor.rcWork;
    const int workWidth = work.right - work.left, workHeight = work.bottom - work.top;
    const int chromeX = outer.right - outer.left - client.right;
    const int chromeY = outer.bottom - outer.top - client.bottom + _chromeHeight;
    const int availableX = std::max(1, workWidth - chromeX), availableY = std::max(1, workHeight - chromeY);
    int width = std::min<int>(_frame->Width, availableX);
    int height = MulDiv(width, _frame->Height, _frame->Width);
    if (height > availableY)
    {
        height = availableY;
        width = MulDiv(height, _frame->Width, _frame->Height);
    }
    const auto minimum = MinimumSize();
    width = std::min(workWidth, std::max(Px(minimum.x), width + chromeX));
    height = std::min(workHeight, std::max(Px(minimum.y), height + chromeY));
    const auto x = std::clamp(outer.left, work.left, work.right - width);
    const auto y = std::clamp(outer.top, work.top, work.bottom - height);
    _resizePending = false;
    if (x == outer.left && y == outer.top && width == outer.right - outer.left && height == outer.bottom - outer.top)
    {
        return;
    }
    ReleaseInput();
    SetWindowPos(_window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

RECT VmWindow::Letterbox() const
{
    if (!_frame)
    {
        return _view;
    }
    auto area = _view;
    const auto width = _view.right - _view.left;
    const auto height = _view.bottom - _view.top;
    if (!Display().FitToWindow && _frame->Width <= static_cast<unsigned>(width) &&
        _frame->Height <= static_cast<unsigned>(height))
    {
        area.left += (width - static_cast<int>(_frame->Width)) / 2;
        area.top += (height - static_cast<int>(_frame->Height)) / 2;
        area.right = area.left + static_cast<LONG>(_frame->Width);
        area.bottom = area.top + static_cast<LONG>(_frame->Height);
        return area;
    }
    const auto fittedWidth = MulDiv(height, _frame->Width, _frame->Height);
    if (fittedWidth <= width)
    {
        area.left += (width - fittedWidth) / 2;
        area.right = area.left + fittedWidth;
    }
    else
    {
        const auto fittedHeight = MulDiv(width, _frame->Height, _frame->Width);
        area.top += (height - fittedHeight) / 2;
        area.bottom = area.top + fittedHeight;
    }
    return area;
}

bool VmWindow::CursorOverGuest(POINT location) const
{
    if (!_frame || (_status != VmStatus::Running && _status != VmStatus::Paused))
    {
        return false;
    }
    if (!ScreenToClient(_window, &location))
    {
        return false;
    }
    const auto bounds = Letterbox();
    return PtInRect(&bounds, location) != FALSE;
}

void VmWindow::RefreshHostCursor() const
{
    POINT location{};
    if (_captured || !GetCursorPos(&location) || WindowFromPoint(location) != _window)
    {
        return;
    }
    auto clientLocation = location;
    RECT client{};
    if (GetClientRect(_window, &client) && ScreenToClient(_window, &clientLocation) &&
        PtInRect(&client, clientLocation))
    {
        SetCursor(CursorOverGuest(location) ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
    }
}

void VmWindow::Paint(HDC dc, const RECT& area)
{
    Fill(dc, Rect(0, 0, _width, Dip(_view.top)), _colors.Panel);
    const int statusTop = _height - _metrics.Control;
    Fill(dc, Rect(0, statusTop, _width, _height), _colors.Panel);
    Fill(dc, Rect(0, statusTop, _width, statusTop + 1), _colors.Border);
    Fill(dc, Rect(0, Dip(_view.top) - 1, _width, Dip(_view.top)), _colors.Border);
    RECT overlap{};
    if (!IntersectRect(&overlap, &area, &_view))
    {
        return;
    }
    Fill(dc, _view, RGB(15, 15, 16));
    if (!_frame)
    {
        if (_status == VmStatus::Starting || _status == VmStatus::Running)
        {
            Text(dc, LText(!_hasVideo ? IDS_NoVideo : IDS_DisplayWait), _view, false, false,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(225, 225, 225));
        }
        return;
    }
    const auto rect = Letterbox();
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = static_cast<LONG>(_frame->Width);
    bitmap.bmiHeader.biHeight = -static_cast<LONG>(_frame->Height);
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, 0, 0, _frame->Width,
                  _frame->Height, _frame->Pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
}

void VmWindow::Tick()
{
    if (const auto status = _session.Status(); status != _status)
    {
        _status = status;
        _pendingShutdownClose = status == VmStatus::Stopped && _closeOnShutdown;
        Set(_runStatus, LText(StatusText(status)));
        PostMessageW(_manager, VmStateChanged, 0, 0);
        if (status != VmStatus::Running)
        {
            ReleaseInput();
        }
        RefreshHostCursor();
        if (status == VmStatus::Failed)
        {
            _pendingFailure = true;
            if (GetActiveWindow() == _window)
            {
                OpenMonitor();
            }
        }
        Refresh();
    }
    if (_pendingShutdownClose && !_closing && !_inAction && IsWindowEnabled(_window))
    {
        DestroyWindow(_window);
        return;
    }
    auto frame = _session.CurrentFrame();
    if (frame != _frame)
    {
        const bool boundsChanged =
            !_frame || !frame || frame->Width != _frame->Width || frame->Height != _frame->Height;
        _frame = std::move(frame);
        if (boundsChanged)
        {
            _resizePending = true;
            Set(_resolution, _frame ? LFmt(IDS_ResolutionValue, _frame->Width, _frame->Height) : L"");
            Refresh();
            RefreshHostCursor();
        }
        InvalidateRect(_window, &_view, FALSE);
    }
    UpdateKeyboardCapture();
    ResizeToGuest();
    if (_captured && _session.AbsolutePointer())
    {
        ReleaseInput();
    }
    for (auto window : {_monitor.get(), _serial.get(), _debug.get()})
    {
        if (window)
        {
            window->Update();
        }
    }
}

void VmWindow::OpenMonitor()
{
    _pendingFailure = false;
    ReleaseInput();
    if (!_monitor)
    {
        auto monitor = std::make_unique<MonitorWindow>(_appearance, _session);
        monitor->Open(LFmt(IDS_MonitorTitle, _profile.Name), nullptr, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                      WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, _window);
        _monitor = std::move(monitor);
    }
    _monitor->Activate();
}

void VmWindow::SetAppearance(Appearance appearance)
{
    Window::SetAppearance(appearance);
    for (auto window : {_monitor.get(), _serial.get(), _debug.get()})
    {
        if (window)
        {
            window->SetAppearance(appearance);
        }
    }
}

void VmWindow::OpenPort(Console console)
{
    ReleaseInput();
    auto& window = console == Console::Serial ? _serial : _debug;
    if (!window)
    {
        window = std::make_unique<MonitorWindow>(_appearance, _session, console);
        const auto label = LText(console == Console::Serial ? IDS_Serial : IDS_DebugPort);
        try
        {
            window->Open(_profile.Name + L" – " + label, nullptr, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                         WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, _window);
        }
        catch (...)
        {
            window.reset();
            throw;
        }
    }
    window->Activate();
}

void VmWindow::Commit(Profile next)
{
    _saveProfile(next);
    _profile = std::move(next);
}

void VmWindow::SetInput(bool ps2)
{
    auto next = _profile;
    auto& value = ps2 ? next.ForcePs2 : next.UsbMouse;
    value = !value;
    if (ps2 && value && _hasPs2)
    {
        next.MachineOptions[L"i8042"] = true;
    }
    Commit(std::move(next));
}

namespace
{
void ConnectDrive(Session& session, const Profile& profile, const Drive& drive)
{
    const auto id = Utf8(drive.Id);
    Json block{{"node-name", id},
               {"driver", Utf8(drive.Format)},
               {"read-only", drive.Protected()},
               {"file", {{"driver", "file"}, {"filename", Utf8(Fs::absolute(drive.File).native())}}}};
    if (!drive.Discard.empty())
    {
        block["discard"] = Utf8(drive.Discard);
    }
    if (!drive.DetectZeroes.empty())
    {
        block["detect-zeroes"] = Utf8(drive.DetectZeroes);
    }
    if (!drive.Cache.empty())
    {
        block["cache"] = {{"direct", drive.Cache == L"none" || drive.Cache == L"directsync"},
                          {"no-flush", drive.Cache == L"unsafe"}};
    }
    session.DeviceCall("blockdev-add", std::move(block));
    try
    {
        session.DeviceCall("device_add", DriveDevice(profile, drive));
    }
    catch (...)
    {
        session.DeviceCall("blockdev-del", {{"node-name", id}});
        throw;
    }
}

void DisconnectDrive(Session& session, const Drive& drive)
{
    const auto id = Utf8(drive.Id), device = "dev-" + id;
    session.DeviceCall("device_del", {{"id", device}});
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    for (;;)
    {
        const auto entries = session.DeviceCall("qom-list", {{"path", "/machine/peripheral"}});
        if (!std::ranges::any_of(entries, [&](const auto& e) { return e.at("name") == device; }))
        {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            Fail(LText(IDS_LiveError));
        }
        std::this_thread::sleep_for(30ms);
    }
    const auto nodes = session.DeviceCall("query-named-block-nodes", {{"flat", true}});
    if (std::ranges::any_of(nodes, [&](const auto& n) { return n.at("node-name") == id; }))
    {
        session.DeviceCall("blockdev-del", {{"node-name", id}});
    }
}

void SwapMedium(Session& session, const Drive& drive, bool eject)
{
    const auto blocks = session.DeviceCall("query-block");
    const auto found = std::ranges::find_if(blocks, [&](const Json& b) {
        return b.value("device", "") == Utf8(drive.Id) || b.value("qdev", "").ends_with(Utf8(L"dev-" + drive.Id));
    });
    if (found == blocks.end() || !found->contains("qdev"))
    {
        Fail(LText(IDS_NoOptical));
    }
    const auto id = found->at("qdev");
    if (!eject)
    {
        session.DeviceCall("blockdev-change-medium", {{"id", id},
                                                      {"filename", Utf8(Fs::absolute(drive.File).native())},
                                                      {"format", Utf8(drive.Format)},
                                                      {"read-only-mode", "read-only"}});
    }
    else if (found->contains("inserted"))
    {
        session.DeviceCall("blockdev-open-tray", {{"id", id}});
        session.DeviceCall("blockdev-remove-medium", {{"id", id}});
    }
}
} // namespace

void VmWindow::ChangeMedia(const Drive& previous, bool eject)
{
    Drive changed = previous;
    if (eject)
    {
        changed.File.clear();
        changed.Format.clear();
    }
    else
    {
        changed.File = ChooseFile(_window, LText(IDS_Image), LText(IDS_StorageFile), L"*.*");
        if (changed.File.empty())
        {
            return;
        }
        changed.Format = InspectDisk(_engine, changed.File).Format;
        if (changed.Format != L"raw" && changed.Format != L"qcow2")
        {
            Fail(LText(IDS_BadFormat));
        }
    }
    auto next = _profile;
    for (auto d : AllDrives(next))
    {
        if (d->Id == changed.Id)
        {
            *d = changed;
        }
    }
    Busy(_window, _appearance, LText(IDS_OpticalDrive), [&] { SwapMedium(_session, changed, eject); });
    try
    {
        Commit(std::move(next));
    }
    catch (...)
    {
        SwapMedium(_session, previous, previous.File.empty());
        throw;
    }
}

void VmWindow::AttachDisk(std::wstring controller)
{
    const auto file = ChooseFile(_window, LText(IDS_SelectDisk), LText(IDS_StorageFile), L"*.*");
    if (file.empty())
    {
        return;
    }
    Drive added{L"disk-" + NewId().substr(0, 24), std::move(controller)};
    added.SelectImage(file);
    added.Format = InspectDisk(_engine, file).Format;
    if (added.Format != L"qcow2" && added.Format != L"raw")
    {
        Fail(LText(IDS_BadFormat));
    }
    const auto drives = AllDrives(_profile);
    for (const auto d : drives)
    {
        if (d->Connected() && SameFilePath(file, d->File) && (!d->Protected() || !added.Protected()))
        {
            Fail(LText(IDS_DuplicateDisk));
        }
    }
    if (SameFilePath(file, NvramPath(_profile)) || SameFilePath(file, _engine.Firmware) ||
        SameFilePath(file, _profile.Serial.File) || SameFilePath(file, _profile.DebugPort.File))
    {
        Fail(LText(IDS_BadDrive));
    }
    const auto count = PortCount(ControllerModel(_profile, added.Controller));
    while (added.Slot < count &&
           (added.Slot == 7 ||
            std::ranges::any_of(
                drives,
                [&](auto d) { return d->Connected() && d->Controller == added.Controller && d->Slot == added.Slot; })))
    {
        ++added.Slot;
    }
    if (added.Slot == count)
    {
        Fail(LText(IDS_BadDrive));
    }
    auto next = _profile;
    next.Drives.push_back(added);
    Busy(_window, _appearance, LText(IDS_AddDrive), [&] { ConnectDrive(_session, next, added); });
    try
    {
        Commit(std::move(next));
    }
    catch (...)
    {
        DisconnectDrive(_session, added);
        throw;
    }
}

void VmWindow::DetachDisk(const Drive& drive)
{
    auto next = _profile;
    if (drive.Id == next.Primary.Id)
    {
        next.Primary.Attached = false;
    }
    else if (drive.Id == next.Optical.Id)
    {
        next.Optical.Attached = false;
    }
    else
    {
        std::erase_if(next.Drives, [&](const Drive& d) { return d.Id == drive.Id; });
    }
    Busy(_window, _appearance, LText(IDS_Unplug), [&] { DisconnectDrive(_session, drive); });
    try
    {
        Commit(std::move(next));
    }
    catch (...)
    {
        ConnectDrive(_session, _profile, drive);
        throw;
    }
}

void VmWindow::DevicesMenu(HMENU menu, std::vector<std::function<void()>>& actions, bool active)
{
    const auto submenu = [](HMENU parent, const std::wstring& name) {
        auto sub = CreatePopupMenu();
        if (!sub || !AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), name.c_str()))
        {
            WinError(L"CreatePopupMenu");
        }
        return sub;
    };
    const auto item = [&](HMENU parent, int caption, std::function<void()> fn, bool enabled = true,
                          bool checked = false) {
        actions.push_back(std::move(fn));
        MenuItem(parent, 999 + actions.size(), caption, enabled, checked);
    };
    auto input = submenu(menu, LText(IDS_Input));
    MenuItem(input, CtrlAltDelete, IDS_SendCtrlAltDelete, _status == VmStatus::Running);
    auto startup = submenu(input, LText(IDS_NextStart));
    MenuItem(startup, NextForcePs2, IDS_ForcePs2, true, _profile.ForcePs2);
    MenuItem(startup, NextUsbMouse, IDS_UsbMouse, true, _profile.UsbMouse);
    std::vector<StorageController> controllers{{L"onboard", _profile.Bus}};
    controllers.insert(controllers.end(), _profile.Controllers.begin(), _profile.Controllers.end());
    for (size_t i = 0; i < controllers.size(); ++i)
    {
        const auto& c = controllers[i];
        auto sub = submenu(menu, std::format(L"{} · {}", i + 1, c.Model));
        for (const auto d : AllDrives(_profile))
        {
            if (d->Connected() && d->Controller == c.Id)
            {
                const auto drive = *d;
                const auto name = drive.File.empty() ? LText(IDS_OpticalDrive) : drive.File.filename().native();
                auto entry = submenu(sub, LFmt(IDS_AttachmentPath, name, drive.Slot));
                if (drive.Media == L"cdrom")
                {
                    item(entry, IDS_Insert, [this, drive] { ChangeMedia(drive, false); }, active);
                    item(entry, IDS_Eject, [this, drive] { ChangeMedia(drive, true); }, active);
                }
                else
                {
                    item(entry, IDS_Unplug, [this, drive] { DetachDisk(drive); }, active && IsScsi(c.Model));
                }
            }
        }
        if (IsScsi(c.Model))
        {
            item(sub, IDS_HotDisk, [this, id = c.Id] { AttachDisk(id); }, active);
        }
    }
    if (_profile.Network.Backend != L"none" && active)
    {
        const auto info =
            _session.DeviceCall("human-monitor-command", {{"command-line", "info network"}}).get<std::string>();
        std::istringstream lines(info);
        for (std::string line; std::getline(lines, line);)
        {
            auto end = line.find(": index=");
            if (end == line.npos || !line.contains("type=nic,"))
            {
                continue;
            }
            auto name = line.substr(0, end);
            if (auto pos = name.rfind(": "); pos != name.npos)
            {
                name.erase(0, pos + 2);
            }
            auto sub = submenu(menu, Wide(name));
            if (!_links.contains(name))
            {
                _links[name] = true;
            }
            item(
                sub, IDS_LinkUp,
                [this, name] {
                    const auto up = !_links.at(name);
                    _session.DeviceCall("set_link", {{"name", name}, {"up", up}});
                    _links[name] = up;
                },
                true, _links.at(name));
        }
    }
    if (_profile.Serial.Mode == L"window")
    {
        MenuItem(menu, ShowSerial, IDS_Serial);
    }
    if (_profile.DebugPort.Mode == L"window")
    {
        MenuItem(menu, ShowDebug, IDS_DebugPort);
    }
}

void VmWindow::HandleAction(int command)
{
    if (_inAction)
    {
        return;
    }
    _inAction = true;
    DropKeyboardCapture();
    ReleaseInput();
    try
    {
        if (command == MachineMenu || command == DisplayMenu || command == DeviceMenu)
        {
            OpenMenu(command);
        }
        else
        {
            RunCommand(command);
        }
    }
    catch (...)
    {
        _inAction = false;
        UpdateKeyboardCapture();
        throw;
    }
    _inAction = false;
    if (_window)
    {
        UpdateKeyboardCapture();
    }
}

void VmWindow::OpenMenu(int group)
{
    std::unique_ptr<std::remove_pointer_t<HMENU>, decltype(&DestroyMenu)> menu(CreatePopupMenu(), DestroyMenu);
    if (!menu)
    {
        WinError(L"CreatePopupMenu");
    }
    const bool active = _status == VmStatus::Running || _status == VmStatus::Paused;
    const auto add = [&](int command, int label, bool enabled = true, bool checked = false) {
        MenuItem(menu.get(), command, label, enabled, checked);
    };
    const auto separator = [&] { AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr); };
    std::vector<std::function<void()>> deviceActions;

    switch (group)
    {
    case MachineMenu:
        add(Pause, _status == VmStatus::Paused ? IDS_Resume : IDS_Pause, active);
        separator();
        add(Reset, IDS_Reset, active);
        add(Shutdown, IDS_Power, active);
        add(SaveAndClose, IDS_SaveAndClose, active && SnapshotDrive(_profile) != nullptr);
        add(Stop, IDS_Stop, active);
        separator();
        add(ViewConfiguration, IDS_ViewVmSettings);
        break;
    case DisplayMenu: {
        const auto settings = Display();
        const auto defaultLabel = LText(_defaultDisplay.FitToWindow     ? IDS_FitDisplay
                                        : _defaultDisplay.ResizeToGuest ? IDS_NativeAutoResize
                                                                        : IDS_NativeDisplay);
        const auto inherit = LFmt(IDS_UseUiDisplay, defaultLabel);
        AppendMenuW(menu.get(),
                    MF_STRING | (_hasVideo ? MF_ENABLED : MF_GRAYED) |
                        (_profile.DisplayOverride ? MF_UNCHECKED : MF_CHECKED),
                    UseDefaultDisplay, inherit.c_str());
        separator();
        add(FitDisplay, IDS_FitDisplay, _hasVideo, settings.FitToWindow);
        add(NativeDisplay, IDS_NativeDisplay, _hasVideo, !settings.FitToWindow);
        add(GuestAutoResize, IDS_ResizeToGuest, _hasVideo && !settings.FitToWindow,
            !settings.FitToWindow && settings.ResizeToGuest);
        separator();
        add(ShowMonitor, IDS_Monitor);
        break;
    }
    case DeviceMenu:
        DevicesMenu(menu.get(), deviceActions, active);
        break;
    default:
        return;
    }

    ReleaseInput();
    RECT button{};
    SendMessageW(_toolbar, TB_GETRECT, group, reinterpret_cast<LPARAM>(&button));
    MapWindowPoints(_toolbar, HWND_DESKTOP, reinterpret_cast<POINT*>(&button), 2);
    TPMPARAMS placement{sizeof(placement), button};
    const auto command =
        TrackPopupMenuEx(menu.get(), TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON,
                         button.left, button.bottom, _window, &placement);
    if (command >= 1000 && command - 1000 < deviceActions.size())
    {
        deviceActions[command - 1000]();
    }
    else if (command)
    {
        RunCommand(command);
    }
}

void VmWindow::RunCommand(int command)
{
    const bool active = _status == VmStatus::Running || _status == VmStatus::Paused;
    if (command == ShowSerial || command == ShowDebug)
    {
        const auto& port = command == ShowSerial ? _profile.Serial : _profile.DebugPort;
        if (port.Mode == L"window")
        {
            OpenPort(command == ShowSerial ? Console::Serial : Console::Debug);
        }
        return;
    }
    if (command == NextForcePs2 || command == NextUsbMouse)
    {
        SetInput(command == NextForcePs2);
        return;
    }
    if (command == ShowMonitor)
    {
        OpenMonitor();
        return;
    }
    if (command == ViewConfiguration)
    {
        PostMessageW(_manager, ViewVmSettings, 0, reinterpret_cast<LPARAM>(_window));
        return;
    }
    if (command == UseDefaultDisplay || command == FitDisplay || command == NativeDisplay || command == GuestAutoResize)
    {
        auto view = Display();
        if (!_hasVideo || (command == GuestAutoResize && view.FitToWindow))
        {
            return;
        }
        auto next = _profile;
        if (command == UseDefaultDisplay)
        {
            next.DisplayOverride.reset();
        }
        else
        {
            if (command == GuestAutoResize)
            {
                view.ResizeToGuest = !view.ResizeToGuest;
            }
            else
            {
                view.FitToWindow = command == FitDisplay;
            }
            next.DisplayOverride = view;
        }
        if (next.DisplayOverride != _profile.DisplayOverride)
        {
            Commit(std::move(next));
            ReleaseInput();
            _resizePending = true;
            ResizeToGuest();
            Refresh();
        }
        return;
    }
    if (!active)
    {
        return;
    }

    switch (command)
    {
    case CtrlAltDelete:
        if (_session.Status() == VmStatus::Running)
        {
            ReleaseInput();
            _session.SendCtrlAltDelete();
        }
        break;
    case SaveAndClose:
        if (SnapshotDrive(_profile) != nullptr)
        {
            Close(true);
        }
        break;
    case Pause:
        _session.Enqueue(_status == VmStatus::Paused ? "cont" : "stop");
        break;
    case Reset:
    case Stop: {
        const auto title = LText(command == Reset ? IDS_ResetPrompt : IDS_StopPrompt);
        const auto label = LText(command == Reset ? IDS_Reset : IDS_Stop);
        TASKDIALOG_BUTTON action{IDOK, label.c_str()};
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = _window;
        dialog.pszWindowTitle = _profile.Name.c_str();
        dialog.pszMainInstruction = title.c_str();
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        dialog.cButtons = 1;
        dialog.pButtons = &action;
        dialog.nDefaultButton = IDCANCEL;
        int selected{};
        if (SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)) && selected == IDOK)
        {
            _session.Enqueue(command == Reset ? "system_reset" : "quit");
        }
        break;
    }
    case Shutdown:
        _session.Enqueue("system_powerdown");
        break;
    }
}

void VmWindow::Close(bool saveSnapshot)
{
    if (_closing || !IsWindowEnabled(_window))
    {
        return;
    }
    _closing = true;
    bool close = true;
    try
    {
        ReleaseInput();
        const auto status = _session.Status();
        if (!saveSnapshot && status != VmStatus::Failed && status != VmStatus::Stopped)
        {
            if (SnapshotDrive(_profile) != nullptr && (status == VmStatus::Running || status == VmStatus::Paused))
            {
                const auto prompt = LFmt(IDS_CloseChoice, _profile.Name);
                const auto save = LText(IDS_SaveAndClose), power = LText(IDS_Stop);
                const TASKDIALOG_BUTTON actions[]{{IDYES, save.c_str()}, {IDNO, power.c_str()}};
                TASKDIALOGCONFIG dialog{sizeof(dialog)};
                dialog.hwndParent = _window;
                dialog.pszWindowTitle = _profile.Name.c_str();
                dialog.pszMainInstruction = prompt.c_str();
                dialog.dwFlags =
                    TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_COMMAND_LINKS;
                dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
                dialog.cButtons = static_cast<UINT>(std::size(actions));
                dialog.pButtons = actions;
                dialog.nDefaultButton = IDCANCEL;
                int choice{};
                const auto result = TaskDialogIndirect(&dialog, &choice, nullptr, nullptr);
                if (FAILED(result))
                {
                    WinError(L"TaskDialogIndirect", result);
                }
                saveSnapshot = choice == IDYES;
                close = saveSnapshot || choice == IDNO;
            }
            else
            {
                close = MessageBoxW(_window, LFmt(IDS_CloseVm, _profile.Name).c_str(), LText(IDS_App).c_str(),
                                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
            }
        }
        if (saveSnapshot)
        {
            Busy(_window, _appearance, LText(IDS_SavingSnapshot),
                 [this] { _session.SaveAndQuit(SnapshotDrive(_profile)->Id); });
        }
        else if (close && status == VmStatus::Restoring)
        {
            Busy(_window, _appearance, LText(IDS_RestoringSnapshot), [this] { _session.Stop(); });
        }
    }
    catch (...)
    {
        _closing = false;
        throw;
    }
    _closing = false;
    if (close)
    {
        DestroyWindow(_window);
    }
}

void VmWindow::SendPointer(int x, int y, bool movement, unsigned wheel)
{
    if (!_frame || (_status != VmStatus::Running && _status != VmStatus::Paused))
    {
        return;
    }
    const auto bounds = Letterbox();
    const int width = std::max<LONG>(1, bounds.right - bounds.left);
    const int height = std::max<LONG>(1, bounds.bottom - bounds.top);
    if (GetCapture() != _window && (x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom))
    {
        return;
    }
    PointerInput input{MulDiv(x - bounds.left, _frame->Width, width),
                       MulDiv(y - bounds.top, _frame->Height, height),
                       0,
                       0,
                       _buttons | wheel,
                       movement};
    if (_captured && movement)
    {
        input.Dx = x - _captureCenter.x;
        input.Dy = y - _captureCenter.y;
        if (!input.Dx && !input.Dy)
        {
            return;
        }
        POINT screen = _captureCenter;
        ClientToScreen(_window, &screen);
        SetCursorPos(screen.x, screen.y);
    }
    _session.Pointer(input);
    if (wheel)
    {
        input.Buttons = _buttons;
        _session.Pointer(input);
    }
}

LRESULT CALLBACK VmWindow::KeyboardEvent(int code, WPARAM message, LPARAM data)
{
    // This hook runs on the UI thread, before shell hotkeys and the host IME.
    // Queue input to the focused guest and return before doing any RFB I/O.
    auto vm = KeyboardOwner;
    GUITHREADINFO gui{sizeof(gui)};
    if (code == HC_ACTION && vm && GetForegroundWindow() == vm->_window &&
        GetGUIThreadInfo(GetCurrentThreadId(), &gui) && gui.hwndFocus == vm->_window &&
        !(gui.flags & (GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE)) && !vm->_closing &&
        !vm->_inAction)
    {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool extended = (event.flags & LLKHF_EXTENDED) != 0;
        const unsigned scan = event.scanCode & 0xff;
        const auto index = scan + (extended ? 256u : 0u);
        if (scan && (down || vm->_intercepted.test(index)))
        {
            const LPARAM flags = vm->_keyboardEpoch | (scan << 16) | (extended ? 1u << 24 : 0u) | (down ? 0 : 1u << 31);
            if (PostMessageW(vm->_window, GuestKey, event.vkCode, flags))
            {
                vm->_intercepted.set(index, down);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

void VmWindow::DropKeyboardCapture()
{
    if (_keyboardHook)
    {
        UnhookWindowsHookEx(_keyboardHook);
        _keyboardHook = nullptr;
        KeyboardOwner = nullptr;
        _intercepted.reset();
        ++_keyboardEpoch;
        ShowWindow(_captureHint, SW_HIDE);
    }
}

void VmWindow::UpdateKeyboardCapture()
{
    const bool focused = _window && GetForegroundWindow() == _window && GetFocus() == _window &&
                         _status == VmStatus::Running && _frame && !_inAction && !_closing;
    if (!focused)
    {
        if (_keyboardHook)
        {
            DropKeyboardCapture();
            ReleaseInput();
        }
        return;
    }
    if (!_keyboardHook)
    {
        _keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardEvent, GetModuleHandleW(nullptr), 0);
        if (!_keyboardHook)
        {
            WinError(L"SetWindowsHookExW");
        }
        KeyboardOwner = this;
        ShowWindow(_captureHint, SW_SHOWNA);
    }
}

void VmWindow::SendKey(WPARAM key, LPARAM data, bool down)
{
    if ((_keyboardHook || _captured) && key == VK_F12)
    {
        if (!down)
        {
            ReleaseInput();
            SetFocus(_toolbar);
        }
        return;
    }
    const auto scan = static_cast<unsigned>((data >> 16) & 0x7f) | ((data & (1 << 24)) ? 0x80u : 0u);
    const KeyInput input{scan, KeySym(key, data), down};
    if (down)
    {
        if ((data & (1u << 30)) || _keys.contains(scan))
        {
            return;
        }
        _keys.emplace(scan, input);
    }
    else if (!_keys.erase(scan))
    {
        return;
    }
    _session.Key(input);
}

void VmWindow::CapturePointer()
{
    if (_captured || !_frame)
    {
        return;
    }
    _captured = true;
    const auto bounds = Letterbox();
    _captureCenter = {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    GetCursorPos(&_savedPointer);
    SetCapture(_window);
    SetCursor(nullptr);
    auto screen = _captureCenter;
    ClientToScreen(_window, &screen);
    SetCursorPos(screen.x, screen.y);
    ShowWindow(_captureHint, SW_SHOWNA);
}

void VmWindow::ReleaseInput()
{
    for (const auto& [scan, key] : _keys)
    {
        _session.Key({scan, key.KeySym, false});
    }
    _keys.clear();
    if (_buttons)
    {
        _buttons = 0;
        _session.Pointer({0, 0, 0, 0, 0, false});
    }
    const bool relative = _captured;
    _captured = false;
    if (GetCapture() == _window)
    {
        ReleaseCapture();
    }
    if (relative)
    {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        SetCursorPos(_savedPointer.x, _savedPointer.y);
        RefreshHostCursor();
        if (!_keyboardHook)
        {
            ShowWindow(_captureHint, SW_HIDE);
        }
    }
}

LRESULT VmWindow::Message(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE && _pendingFailure)
        {
            PostMessageW(_window, RevealFailure, 0, 0);
        }
        break;
    case RevealFailure:
        if (_pendingFailure && GetActiveWindow() == _window)
        {
            OpenMonitor();
        }
        return 0;
    case WM_NOTIFY: {
        const auto& header = *reinterpret_cast<const NMHDR*>(lparam);
        if (header.hwndFrom != _toolbar)
        {
            break;
        }
        if (header.code == TBN_DROPDOWN)
        {
            HandleAction(reinterpret_cast<const NMTOOLBARW*>(lparam)->iItem);
            return TBDDRET_DEFAULT;
        }
        if (header.code != NM_CUSTOMDRAW)
        {
            break;
        }
        auto& draw = *reinterpret_cast<NMTBCUSTOMDRAW*>(lparam);
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT)
        {
            Fill(draw.nmcd.hdc, draw.nmcd.rc, _colors.Panel);
            return CDRF_NOTIFYITEMDRAW;
        }
        if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
        {
            draw.clrText = _colors.Text;
            draw.clrBtnFace = _colors.Panel;
            draw.clrBtnHighlight = _colors.Field;
            draw.clrHighlightHotTrack = _colors.Field;
            draw.clrMark = _colors.Accent;
            return TBCDRF_HILITEHOTTRACK | TBCDRF_USECDCOLORS;
        }
        break;
    }
    case WM_TIMER:
        Tick();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED)
        {
            HandleAction(LOWORD(wparam));
            return 0;
        }
        break;
    case GuestKey:
        if (_keyboardHook && LOWORD(lparam) == _keyboardEpoch && GetFocus() == _window)
        {
            SendKey(wparam, lparam, !(lparam & (1u << 31)));
        }
        return 0;
    case WM_SETFOCUS:
        UpdateKeyboardCapture();
        return 0;
    case WM_KILLFOCUS:
        DropKeyboardCapture();
        ReleaseInput();
        return 0;
    case WM_CAPTURECHANGED:
        if (_captured || _buttons)
        {
            ReleaseInput();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (_status == VmStatus::Running && _frame)
        {
            SendKey(wparam, lparam, message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
            return 0;
        }
        break;
    case WM_SETCURSOR: {
        POINT location{};
        if (_captured || (reinterpret_cast<HWND>(wparam) == _window && LOWORD(lparam) == HTCLIENT &&
                          GetCursorPos(&location) && CursorOverGuest(location)))
        {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE:
        SendPointer(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const auto box = Letterbox();
        if (!_captured && !_buttons && (!PtInRect(&box, point) || !_frame))
        {
            return 0;
        }
        const auto mask = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP   ? 1u
                          : message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ? 2u
                                                                                 : 4u;
        const bool down = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN;
        if (down)
        {
            SetFocus(_window);
            if (!_session.AbsolutePointer() && !_captured)
            {
                CapturePointer();
            }
            else if (!_buttons && !_captured)
            {
                SetCapture(_window);
            }
            _buttons |= mask;
        }
        else
        {
            _buttons &= ~mask;
        }
        SendPointer(point.x, point.y, false);
        if (!_buttons && !_captured && GetCapture() == _window)
        {
            ReleaseCapture();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(_window, &point);
        SendPointer(point.x, point.y, false, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 8u : 16u);
        return 0;
    }
    case WM_CLOSE:
        Close(false);
        return 0;
    case WM_DESTROY:
        KillTimer(_window, Timer);
        DropKeyboardCapture();
        for (const auto window : {_monitor.get(), _serial.get(), _debug.get()})
        {
            if (window && window->Get())
            {
                DestroyWindow(window->Get());
            }
        }
        ReleaseInput();
        PostMessageW(_manager, VmStateChanged, 0, 0);
        return 0;
    }
    return Window::Message(message, wparam, lparam);
}
} // namespace Ia64::Ui
