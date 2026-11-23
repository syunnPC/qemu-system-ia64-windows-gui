// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Localization.h"
#include "Devices.h"
#include "ProfileDetails.h"
#include "Updates.h"
#include "VmWindow.h"
#include "Wizard.h"

#include <objbase.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <algorithm>
#include <array>
#include <future>
#include <memory>

namespace Ia64::Ui
{
namespace
{
enum SetupId
{
    RuntimePath = 300,
    DiskToolPath,
    FirmwarePath,
    BrowseRuntime,
    BrowseDiskTool,
    BrowseFirmware,
    ChannelId,
    Fetch,
    Save,
    Cancel,
    Theme,
    DisplayLanguage,
    DefaultView,
    CloseOnShutdown
};
constexpr std::array Channels{Channel::Stable, Channel::BetaStable, Channel::Experimental};

struct SetupResult
{
    Engine Tools;
    std::optional<Capabilities> Detected;
};

class SetupWindow final : public Window
{
public:
    explicit SetupWindow(const State& state) : Window(state.Interface.Theme), _tools(state.Tools)
    {
    }
    std::optional<SetupResult> Result;

private:
    void Create() override
    {
        _captions[0] = Label(LText(IDS_Emulator), TextRole::Body, SS_CENTERIMAGE);
        _qemu = Edit(RuntimePath, _tools.Qemu.native());
        _pickQemu = Button(BrowseRuntime, LText(IDS_Browse));
        _captions[1] = Label(LText(IDS_ImageTool), TextRole::Body, SS_CENTERIMAGE);
        _image = Edit(DiskToolPath, _tools.ImageTool.native());
        _pickImage = Button(BrowseDiskTool, LText(IDS_Browse));
        _captions[2] = Label(LText(IDS_Firmware), TextRole::Body, SS_CENTERIMAGE);
        _firmware = Edit(FirmwarePath, _tools.Firmware.native());
        _pickFirmware = Button(BrowseFirmware, LText(IDS_Browse));
        _captions[3] = Label(LText(IDS_Channel), TextRole::Body, SS_CENTERIMAGE);
        _channel = Combo(ChannelId);
        std::vector<std::wstring> labels;
        for (const auto channel : Channels)
        {
            labels.emplace_back(ChannelName(channel));
        }
        const auto selected = _tools.ReleaseTag.substr(0, _tools.ReleaseTag.find(':'));
        Options(_channel, labels, Wide(selected));
        DefaultHint(_channel, labels.front());
        _fetch = Button(Fetch, LText(IDS_Update));
        _save = Button(Save, LText(IDS_Save), true);
        _cancel = Button(Cancel, LText(IDS_Cancel));
        _notice = Label(L"");
        if (!_tools.Qemu.empty() && _tools.ImageTool.empty())
        {
            Set(_notice, LText(IDS_ToolMissing));
        }
    }

    void Layout(int width, int height) override
    {
        _width = width;
        _height = height;
        const int gap = _metrics.Gap, margin = _metrics.Margin;
        int captions = 0;
        for (const auto label : _captions)
        {
            captions = std::max(captions, TextWidth(Read(label)));
        }
        const bool stacked = width - 2 * margin < captions + _metrics.Character * 28;
        const int start = stacked ? margin : margin + captions + gap;
        const std::array inputs{_qemu, _image, _firmware, _channel};
        const std::array actions{_pickQemu, _pickImage, _pickFirmware, _fetch};
        int y = margin;
        for (size_t index = 0; index < inputs.size(); ++index)
        {
            if (inputs[index] == _channel)
            {
                y += gap;
            }
            const int actionWidth = ButtonWidth(actions[index]);
            const int room = width - margin - actionWidth - gap - start;
            const int inputWidth = InputWidth(inputs[index], std::min(room, _metrics.PathWidth()));
            Place(_captions[index], margin, y, captions, stacked ? _metrics.Line : _metrics.Control);
            if (stacked)
            {
                y += _metrics.Line;
            }
            Place(inputs[index], start, y, inputWidth,
                  inputs[index] == _channel ? _metrics.Control * static_cast<int>(Channels.size() + 1)
                                            : _metrics.Control);
            Place(actions[index], start + inputWidth + gap, y, actionWidth, _metrics.Control);
            y += _metrics.Row;
            if (index == 2)
            {
                _divider = y;
            }
        }
        Place(_notice, margin, y, width - 2 * margin, height - _metrics.Footer() - y);
        PlaceFooter(width, height, {_cancel, _save});
    }

    void Paint(HDC dc, const RECT&) override
    {
        const int footer = _height - _metrics.Footer();
        Fill(dc, Rect(_metrics.Margin, _divider, _width - _metrics.Margin, _divider + 1), _colors.Border);
        Fill(dc, Rect(0, footer, _width, _height), _colors.Panel);
        Fill(dc, Rect(0, footer, _width, footer + 1), _colors.Border);
    }

    Engine Inputs() const
    {
        Engine engine{Trim(Read(_qemu)), Trim(Read(_image)), Trim(Read(_firmware)), _tools.ReleaseTag};
        if (engine.Qemu != _tools.Qemu || engine.Firmware != _tools.Firmware)
        {
            engine.ReleaseTag.clear();
        }
        return engine;
    }

    void Pick(int source)
    {
        const bool firmware = source == BrowseFirmware;
        const auto field = source == BrowseRuntime ? _qemu : source == BrowseDiskTool ? _image : _firmware;
        auto path =
            ChooseFile(_window,
                       LText(source == BrowseRuntime    ? IDS_Emulator
                             : source == BrowseDiskTool ? IDS_ImageTool
                                                        : IDS_Firmware),
                       LText(firmware ? IDS_FileFirmware : IDS_FileExe), firmware ? L"*.bin;*.elf;*.rom" : L"*.exe");
        if (path.empty())
        {
            return;
        }
        Set(field, path.native());
        if (source == BrowseRuntime)
        {
            const auto qemuParent = path.parent_path();
            const auto sibling = qemuParent / L"qemu-img.exe";
            const auto siblingFirmware = qemuParent / L"share" / L"ia64-firmware.bin";
            if (Read(_image).empty() && IsFile(sibling))
            {
                Set(_image, sibling.native());
            }
            if (Read(_firmware).empty() && IsFile(siblingFirmware))
            {
                Set(_firmware, siblingFirmware.native());
            }
        }
    }

    void Download()
    {
        const auto selected = SendMessageW(_channel, CB_GETCURSEL, 0, 0);
        if (selected < 0 || selected >= static_cast<LRESULT>(Channels.size()))
        {
            return;
        }
        auto current = Inputs();
        std::optional<Engine> updated;
        Busy(_window, _appearance, LText(IDS_EngineDownload),
             [&] { updated = UpdateEngine(Channels[static_cast<size_t>(selected)], current); });
        if (!updated)
        {
            Set(_notice, LText(IDS_StableEmpty));
            return;
        }
        const bool currentBuild = updated->Qemu == current.Qemu;
        _tools = std::move(*updated);
        Set(_qemu, _tools.Qemu.native());
        Set(_image, _tools.ImageTool.native());
        Set(_firmware, _tools.Firmware.native());
        Set(_notice, LText(_tools.ImageTool.empty() ? IDS_ToolMissing : currentBuild ? IDS_UpToDate : IDS_Downloaded));
    }

    void Commit()
    {
        auto engine = Inputs();
        std::optional<Capabilities> capabilities;
        if (!engine.Qemu.empty() || !engine.ImageTool.empty() || !engine.Firmware.empty())
        {
            ValidateEngine(engine);
            engine.Qemu = Fs::absolute(engine.Qemu);
            engine.ImageTool = Fs::absolute(engine.ImageTool);
            engine.Firmware = Fs::absolute(engine.Firmware);
            Busy(_window, _appearance, LText(IDS_EngineValidate), [&] { capabilities = ProbeEngine(engine); });
        }
        Result = SetupResult{std::move(engine), std::move(capabilities)};
        DestroyWindow(_window);
    }

    POINT MinimumSize() const override
    {
        return {_metrics.Character * 43, _metrics.Row * 7 + _metrics.Footer()};
    }
    SIZE PreferredSize(SIZE) const override
    {
        const int caption = std::max({TextWidth(LText(IDS_Emulator)), TextWidth(LText(IDS_ImageTool)),
                                      TextWidth(LText(IDS_Firmware)), TextWidth(LText(IDS_Channel))});
        return {_metrics.Margin * 2 + caption + _metrics.Gap * 4 + _metrics.Character * 36 +
                    TextWidth(LText(IDS_Browse)),
                MinimumSize().y};
    }

    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override
    {
        if (message == WM_COMMAND)
        {
            if (HIWORD(wparam) == BN_CLICKED)
            {
                switch (LOWORD(wparam))
                {
                case BrowseRuntime:
                case BrowseDiskTool:
                case BrowseFirmware:
                    Pick(LOWORD(wparam));
                    return 0;
                case Fetch:
                    Download();
                    return 0;
                case Save:
                    Commit();
                    return 0;
                case Cancel:
                case IDCANCEL:
                    DestroyWindow(_window);
                    return 0;
                default:
                    break;
                }
            }
        }
        return Window::Message(message, wparam, lparam);
    }

    Engine _tools;
    std::array<HWND, 4> _captions{};
    int _width{}, _height{}, _divider{};
    HWND _qemu{}, _image{}, _firmware{}, _pickQemu{}, _pickImage{}, _pickFirmware{};
    HWND _channel{}, _fetch{}, _save{}, _cancel{}, _notice{};
};

class InterfaceSettingsWindow final : public Window
{
public:
    explicit InterfaceSettingsWindow(InterfaceSettings value) : Window(value.Theme), _initial(value)
    {
    }
    std::optional<InterfaceSettings> Result;

private:
    static constexpr std::array Captions{IDS_Theme, IDS_DisplayLanguage, IDS_DefaultVmDisplay};
    static constexpr std::array Ids{Theme, DisplayLanguage, DefaultView};

    std::array<std::vector<std::wstring>, 3> Choices() const
    {
        return {{{SystemThemeName(), LText(IDS_ThemeLight), LText(IDS_ThemeDark)},
                 {LFmt(IDS_FollowSystem, LanguageInfoFor(WindowsUiLanguage()).NativeName),
                  LanguageInfoFor(UiLanguage::Japanese).NativeName, LanguageInfoFor(UiLanguage::English).NativeName},
                 {LText(IDS_FitDisplay), LText(IDS_NativeDisplay), LText(IDS_NativeAutoResize)}}};
    }

    int LabelWidth() const
    {
        int width{};
        for (const auto caption : Captions)
        {
            width = std::max(width, TextWidth(LText(caption)));
        }
        return width;
    }

    SIZE PreferredSize(SIZE available) const override
    {
        int widest{};
        for (const auto& row : Choices())
        {
            for (const auto& choice : row)
            {
                widest = std::max(widest, TextWidth(choice));
            }
        }
        const int fields = LabelWidth() + _metrics.Gap * 3 + widest + _metrics.Control;
        const int check = TextWidth(LText(IDS_CloseVmOnShutdown)) + _metrics.Control;
        const int rowsHeight = static_cast<int>(Captions.size()) * _metrics.Row;
        return {std::min<LONG>(available.cx, _metrics.Margin * 2 + std::max(fields, check)),
                _metrics.Margin * 2 + rowsHeight + _metrics.Control + _metrics.Footer()};
    }

    void Create() override
    {
        const auto choices = Choices();
        const auto& view = _initial.DefaultDisplay;
        const std::array selected{static_cast<int>(_appearance), static_cast<int>(_initial.Language),
                                  view.FitToWindow     ? 0
                                  : view.ResizeToGuest ? 2
                                                       : 1};
        for (size_t i = 0; i < Captions.size(); ++i)
        {
            _labels[i] = Label(LText(Captions[i]), TextRole::Body, SS_CENTERIMAGE);
            _choices[i] = Combo(Ids[i]);
            Options(_choices[i], choices[i], choices[i][selected[i]]);
        }
        DefaultHint(_choices[0], choices[0].front());
        DefaultHint(_choices[1], choices[1].front());
        DefaultHint(_choices[2], choices[2].front());
        _closeVm = Child(WC_BUTTONW, LText(IDS_CloseVmOnShutdown), CloseOnShutdown, WS_TABSTOP | BS_AUTOCHECKBOX);
        SendMessageW(_closeVm, BM_SETCHECK, _initial.CloseOnShutdown ? BST_CHECKED : BST_UNCHECKED, 0);
        DefaultHint(_closeVm, LText(IDS_Enabled));
        _save = Button(Save, LText(IDS_Save), true);
        _cancel = Button(Cancel, LText(IDS_Cancel));
    }

    void Layout(int width, int height) override
    {
        _width = width;
        _height = height;
        const auto margin = _metrics.Margin;
        const auto choices = Choices();
        for (size_t i = 0; i < 2; ++i)
        {
            const auto& first = choices[i].front();
            if (ComboBox_FindStringExact(_choices[i], -1, first.c_str()) == CB_ERR)
            {
                const auto selected = SendMessageW(_choices[i], CB_GETCURSEL, 0, 0);
                Options(_choices[i], choices[i], choices[i].at(static_cast<size_t>(selected)));
                DefaultHint(_choices[i], first);
            }
        }
        const auto label = LabelWidth();
        const auto left = margin + label + _metrics.Gap;
        const int available = width - left - margin;
        int y = margin;
        for (size_t i = 0; i < Captions.size(); ++i)
        {
            Place(_labels[i], margin, y, label, _metrics.Control);
            Place(_choices[i], left, y, InputWidth(_choices[i], available), _metrics.Control * 5);
            y += _metrics.Row;
        }
        Place(_closeVm, margin, y, width - margin * 2, _metrics.Control);
        PlaceFooter(width, height, {_cancel, _save});
    }

    void Paint(HDC dc, const RECT&) override
    {
        const int footer = _height - _metrics.Footer();
        Fill(dc, Rect(0, footer, _width, _height), _colors.Panel);
        Fill(dc, Rect(0, footer, _width, footer + 1), _colors.Border);
    }

    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override
    {
        if (message == WM_COMMAND)
        {
            const auto id = LOWORD(wparam);
            if (id == Theme && HIWORD(wparam) == CBN_SELCHANGE)
            {
                SetAppearance(static_cast<Appearance>(SendMessageW(_choices[0], CB_GETCURSEL, 0, 0)));
            }
            if (id == Save)
            {
                auto settings = _initial;
                settings.Theme = _appearance;
                settings.Language = static_cast<UiLanguage>(SendMessageW(_choices[1], CB_GETCURSEL, 0, 0));
                const auto mode = SendMessageW(_choices[2], CB_GETCURSEL, 0, 0);
                settings.DefaultDisplay = {mode == 0, mode == 2};
                settings.CloseOnShutdown = SendMessageW(_closeVm, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Result = settings;
            }
            if (id == Save || id == Cancel || id == IDCANCEL)
            {
                DestroyWindow(_window);
                return 0;
            }
        }
        return Window::Message(message, wparam, lparam);
    }

    InterfaceSettings _initial;
    std::array<HWND, 3> _labels{}, _choices{};
    HWND _closeVm{}, _save{}, _cancel{};
    int _width{}, _height{};
};

class SnapshotWindow final : public Window
{
public:
    SnapshotWindow(Appearance appearance, const std::vector<VmSnapshot>& snapshots) : Window(appearance)
    {
        _sources.push_back(LText(IDS_ColdBoot));
        for (const auto& snapshot : snapshots)
        {
            _sources.push_back(snapshot.Tag.empty() ? LFmt(IDS_UnnamedSnapshot, Wide(snapshot.Id))
                                                    : Wide(snapshot.Tag));
        }
    }

    std::optional<size_t> SelectedSource;
    bool SkipPrompt{};

private:
    enum Id
    {
        Source = 320,
        HidePrompt,
        Start
    };

    SIZE PreferredSize(SIZE available) const override
    {
        const int label = TextWidth(LText(IDS_BootSource)) + _metrics.Gap;
        int field = TextWidth(LText(IDS_HideSnapshotPrompt)) + _metrics.Control;
        for (const auto& source : _sources)
        {
            field = std::max(field, label + TextWidth(source) + _metrics.Control + _metrics.Gap);
        }
        return {std::min<int>(available.cx, std::min(field, label + _metrics.PathWidth()) + _metrics.Margin * 2),
                _metrics.Margin * 2 + _metrics.Control * 2 + _metrics.Gap + _metrics.Footer()};
    }

    void Create() override
    {
        _caption = Label(LText(IDS_BootSource), TextRole::Body, SS_CENTERIMAGE);
        _source = Combo(Source);
        Options(_source, _sources, _sources.front());
        SendMessageW(_source, CB_SETCURSEL, 1, 0);
        _hide = Child(WC_BUTTONW, LText(IDS_HideSnapshotPrompt), HidePrompt, WS_TABSTOP | BS_AUTOCHECKBOX);
        _start = Button(Start, LText(IDS_Start), true);
        _cancel = Button(IDCANCEL, LText(IDS_Cancel));
    }

    void Layout(int width, int height) override
    {
        const int margin = _metrics.Margin, control = _metrics.Control;
        const int label = TextWidth(Read(_caption));
        const int left = margin + label + _metrics.Gap;
        Place(_caption, margin, margin, label, control);
        Place(_source, left, margin, InputWidth(_source, width - left - margin), control * 8);
        Place(_hide, margin, margin + control + _metrics.Gap,
              std::min(width - 2 * margin, TextWidth(Read(_hide)) + control), control);
        PlaceFooter(width, height, {_cancel, _start});
    }

    void Paint(HDC dc, const RECT&) override
    {
        RECT rect{};
        GetClientRect(_window, &rect);
        rect.top = rect.bottom - Px(_metrics.Footer());
        Fill(dc, rect, _colors.Panel);
    }

    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override
    {
        if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED)
        {
            switch (LOWORD(wparam))
            {
            case HidePrompt:
                SkipPrompt = SendMessageW(_hide, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (SkipPrompt)
                {
                    _previousSource = SendMessageW(_source, CB_GETCURSEL, 0, 0);
                }
                SendMessageW(_source, CB_SETCURSEL, SkipPrompt ? 0 : _previousSource, 0);
                Enable(_source, !SkipPrompt);
                return 0;
            case Start:
                SelectedSource = SkipPrompt ? 0 : static_cast<size_t>(SendMessageW(_source, CB_GETCURSEL, 0, 0));
                [[fallthrough]];
            case IDCANCEL:
                DestroyWindow(_window);
                return 0;
            }
        }
        return Window::Message(message, wparam, lparam);
    }

    std::vector<std::wstring> _sources;
    LRESULT _previousSource = 1;
    HWND _caption{}, _source{}, _hide{}, _start{}, _cancel{};
};

enum ManagerId
{
    CreateVm = 400,
    EditVm,
    RemoveVm,
    StartVm,
    Setup,
    AppSettings,
    VmList,
    VmDetails,
    FirstAction
};
constexpr UINT_PTR DefaultsTimer = 1;

class Manager final : public Window
{
public:
    explicit Manager(State state) : Window(state.Interface.Theme), _state(std::move(state))
    {
    }

    bool TranslateKey(const MSG& message) override
    {
        if (message.message != WM_KEYDOWN || GetAncestor(message.hwnd, GA_ROOT) != _window ||
            !IsWindowEnabled(_window) || GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_MENU) < 0)
        {
            return false;
        }
        int command{};
        if (GetKeyState(VK_CONTROL) < 0)
        {
            if (message.wParam == 'N')
            {
                command = CreateVm;
            }
        }
        else if (message.hwnd == _list)
        {
            switch (message.wParam)
            {
            case VK_RETURN:
                command = StartVm;
                break;
            case VK_F2:
                command = EditVm;
                break;
            case VK_DELETE:
                command = RemoveVm;
                break;
            }
        }
        if (!command)
        {
            return false;
        }
        if (!(message.lParam & (1u << 30)))
        {
            SendMessageW(_window, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
        }
        return true;
    }

private:
    const Profile* Selected() const
    {
        const auto index = SendMessageW(_list, LB_GETCURSEL, 0, 0);
        return index < 0 || index >= static_cast<LRESULT>(_state.Profiles.size())
                   ? nullptr
                   : &_state.Profiles[static_cast<size_t>(index)];
    }

    auto FindWindow(std::wstring_view id)
    {
        return std::ranges::find_if(_vms, [id](const auto& vm) { return vm->Get() && vm->Id() == id; });
    }

    bool EngineReady() const
    {
        return IsFile(_state.Tools.Qemu) && IsFile(_state.Tools.ImageTool) && IsFile(_state.Tools.Firmware);
    }

    void Populate(std::wstring_view selected = {})
    {
        SendMessageW(_list, LB_RESETCONTENT, 0, 0);
        int index = 0;
        for (size_t row = 0; row < _state.Profiles.size(); ++row)
        {
            const auto& vm = _state.Profiles[row];
            InsertLibraryRow(row, vm, ProfileStatus(vm.Id));
            if (vm.Id == selected)
            {
                index = static_cast<int>(row);
            }
        }
        SendMessageW(_list, LB_SETCURSEL, index, 0);
        RefreshSelection();
    }

    VmStatus ProfileStatus(std::wstring_view id)
    {
        const auto found = FindWindow(id);
        return found != _vms.end() ? (*found)->Status() : VmStatus::Stopped;
    }

    void InsertLibraryRow(size_t row, const Profile& vm, VmStatus status)
    {
        // The native list-box item also supplies the name read by assistive technology.
        const auto text = vm.Name + L" — " + LText(StatusText(status));
        SendMessageW(_list, LB_INSERTSTRING, row, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(_list, LB_SETITEMDATA, row, static_cast<LPARAM>(status));
    }

    void RefreshLibraryStatus()
    {
        const auto selected = SendMessageW(_list, LB_GETCURSEL, 0, 0);
        const auto top = SendMessageW(_list, LB_GETTOPINDEX, 0, 0);
        bool changed = false;
        for (size_t row = 0; row < _state.Profiles.size(); ++row)
        {
            const auto& profile = _state.Profiles[row];
            const auto status = ProfileStatus(profile.Id);
            if (SendMessageW(_list, LB_GETITEMDATA, row, 0) == static_cast<LRESULT>(status))
            {
                continue;
            }
            SendMessageW(_list, LB_DELETESTRING, row, 0);
            InsertLibraryRow(row, profile, status);
            changed = true;
        }
        if (changed)
        {
            SendMessageW(_list, LB_SETCURSEL, selected, 0);
            SendMessageW(_list, LB_SETTOPINDEX, top, 0);
        }
    }

    void UpdateSessionActions()
    {
        const auto selected = Selected();
        const bool active = selected && FindWindow(selected->Id) != _vms.end();
        const auto status = selected ? ProfileStatus(selected->Id) : VmStatus::Stopped;
        const bool readOnly = status != VmStatus::Stopped && status != VmStatus::Failed;
        Enable(_edit, selected != nullptr);
        Set(_edit, LText(readOnly ? IDS_ViewVmSettings : IDS_RenameEdit));
        ButtonGlyph(_edit, readOnly ? 0xe756 : 0xe70f);
        Enable(_remove, selected && !active);
        Set(_start, LText(active ? IDS_Open : IDS_Start));
        ButtonGlyph(_start, active ? 0xe8a7 : 0xe768);
    }

    void RefreshSelection()
    {
        const auto selected = Selected();
        for (const auto control : {_library, _new, _list, _title, _start, _edit, _remove, _details})
        {
            ShowWindow(control, selected ? SW_SHOWNA : SW_HIDE);
        }
        for (const auto control : {_emptyTitle, _first})
        {
            ShowWindow(control, selected ? SW_HIDE : SW_SHOWNA);
        }
        ShowWindow(_setup, selected || _engineReady ? SW_SHOWNA : SW_HIDE);
        if (selected)
        {
            Set(_title, selected->Name);
            const int unavailable = !_engineReady    ? IDS_EngineMissing
                                    : _probe.valid() ? IDS_ReadingDefaults
                                                     : IDS_DefaultsUnavailable;
            ShowProfileDetails(_details, *selected, _capabilities ? &*_capabilities : nullptr, false, {}, unavailable);
        }
        else
        {
            Set(_emptyTitle, LText(_engineReady ? IDS_EmptyTitle : IDS_EngineMissing));
            Set(_first, LText(_engineReady ? IDS_CreateMachine : IDS_SetUpQemu));
            ButtonGlyph(_first, _engineReady ? 0xe710 : 0xe950);
        }
        UpdateSessionActions();
        Refresh();
    }

    void Create() override
    {
        _engineReady = EngineReady();
        _library = OnPanel(Label(LText(IDS_Manager), TextRole::Section, SS_CENTERIMAGE | SS_ENDELLIPSIS));
        _new = OnPanel(Action(CreateVm, LText(IDS_New), 0xe710));
        _list = OnPanel(
            Child(L"LISTBOX", LText(IDS_Manager), VmList,
                  WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS));
        _emptyTitle = Label(L"", TextRole::Title, SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS);
        _first = Button(FirstAction, L"", true);
        _title = Label(L"", TextRole::Title, SS_CENTERIMAGE | SS_ENDELLIPSIS);
        _start = Button(StartVm, LText(IDS_Start), true, 0xe768);
        _edit = Button(EditVm, LText(IDS_RenameEdit), false, 0xe70f);
        _remove = Action(RemoveVm, LText(IDS_Remove), 0xe74d);
        _details = PropertyList(VmDetails);
        _setup = OnPanel(Action(Setup, LText(IDS_QemuSettings), 0xe950));
        _settings = OnPanel(Action(AppSettings, LText(IDS_AppSettings), 0xe790));
        if (_engineReady && !_state.Profiles.empty())
        {
            BeginProbe();
        }
        Populate();
    }

    void LanguageChanged() override
    {
        const auto selected = Selected();
        const std::wstring id = selected ? selected->Id : L"";
        const auto top = SendMessageW(_list, LB_GETTOPINDEX, 0, 0);
        Set(_window, LText(IDS_App));
        Set(_library, LText(IDS_Manager));
        Set(_list, LText(IDS_Manager));
        Set(_new, LText(IDS_New));
        Set(_remove, LText(IDS_Remove));
        Set(_setup, LText(IDS_QemuSettings));
        Set(_settings, LText(IDS_AppSettings));
        Populate(id);
        SendMessageW(_list, LB_SETTOPINDEX, top, 0);
    }

    int LibraryRowHeight() const
    {
        return _metrics.Line * 2 + _metrics.Unit * 3;
    }

    void Layout(int width, int height) override
    {
        _width = width;
        _height = height;
        const int gap = _metrics.Gap, margin = _metrics.Margin, control = _metrics.Control;
        const int footer = height - _metrics.Footer();
        const int minSide = std::max({ButtonWidth(_setup), ButtonWidth(_settings),
                                      TextWidth(Read(_library), _library) + ButtonWidth(_new) + gap}) +
                            gap * 2;
        int names = 0;
        for (const auto& profile : _state.Profiles)
        {
            names = std::max(names, TextWidth(profile.Name) + control + 3 * gap);
        }
        _side = Selected() ? std::min(width / 3, std::max(minSide, std::min(names, _metrics.Character * 28))) : 0;
        if (!_side)
        {
            const int titleHeight = _metrics.Line * 2;
            const int y = std::max(gap, (footer - titleHeight - gap - control) / 2);
            const int firstWidth = ButtonWidth(_first);
            Place(_emptyTitle, gap, y, width - gap * 2, titleHeight);
            Place(_first, (width - firstWidth) / 2, y + titleHeight + gap, firstWidth, control);
            Place(_setup, gap, footer + gap, ButtonWidth(_setup), control);
            Place(_settings, width - gap - ButtonWidth(_settings), footer + gap, ButtonWidth(_settings), control);
            return;
        }
        const int newWidth = ButtonWidth(_new);
        Place(_library, gap, gap, _side - newWidth - gap * 3, control);
        Place(_new, _side - gap - newWidth, gap, newWidth, control);
        const int listTop = margin + control;
        _libraryRule = height - _metrics.Row * 2 - gap * 2;
        Place(_list, _metrics.Unit, listTop, _side - gap, _libraryRule - gap - listTop);
        SendMessageW(_list, LB_SETITEMHEIGHT, 0, Px(LibraryRowHeight()));
        Place(_setup, gap, _libraryRule + gap, std::min(ButtonWidth(_setup), _side - 2 * gap), control);
        Place(_settings, gap, _libraryRule + gap + _metrics.Row, std::min(ButtonWidth(_settings), _side - 2 * gap),
              control);

        const int left = _side + margin, right = width - margin;
        Place(_title, left, gap, right - left, _metrics.Line * 2);
        int x = left, y = _metrics.Header() + gap;
        for (const auto button : {_start, _edit, _remove})
        {
            const int size = std::min(right - left, ButtonWidth(button));
            if (x > left && x + size > right)
            {
                x = left;
                y += _metrics.Row;
            }
            Place(button, x, y, size, control);
            x += size + gap;
        }
        _detailRule = y + control + margin;
        PlacePropertyList(_details, left, _detailRule + _metrics.Unit, right - left, height - _detailRule - gap);
    }

    void Paint(HDC dc, const RECT&) override
    {
        if (_side)
        {
            Fill(dc, Rect(0, 0, _side, _height), _colors.Panel);
            Fill(dc, Rect(_side, 0, _side + 1, _height), _colors.Border);
            Fill(dc, Rect(_metrics.Gap, _libraryRule, _side - _metrics.Gap, _libraryRule + 1), _colors.Border);
            Fill(dc, Rect(_side + 1, _detailRule, _width, _detailRule + 1), _colors.Border);
        }
        else
        {
            const auto footer = _height - _metrics.Footer();
            Fill(dc, Rect(0, footer, _width, _height), _colors.Panel);
            Fill(dc, Rect(0, footer, _width, footer + 1), _colors.Border);
        }
    }

    void DrawProfile(const DRAWITEMSTRUCT& item)
    {
        if (item.itemID >= _state.Profiles.size())
        {
            return;
        }
        const auto& profile = _state.Profiles[item.itemID];
        const auto status = static_cast<VmStatus>(item.itemData);
        const auto& row = item.rcItem;
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        const auto ink = selected && _colors.HighContrast ? _colors.OnAccent : _colors.Text;
        const auto muted = selected && _colors.HighContrast ? ink : _colors.Muted;
        Fill(item.hDC, row, _colors.Panel);
        if (selected)
        {
            Rounded(item.hDC, {row.left, row.top + Px(2), row.right, row.bottom - Px(2)}, 4, _colors.Selection,
                    _colors.Selection);
            Fill(item.hDC, {row.left, row.top + Px(_metrics.Gap), row.left + Px(3), row.bottom - Px(_metrics.Gap)},
                 _colors.HighContrast ? ink : _colors.Accent);
        }
        Icon(item.hDC, 0xe977,
             {row.left + Px(_metrics.Gap), row.top, row.left + Px(_metrics.Gap + _metrics.Line), row.bottom}, ink);
        const auto x = row.left + Px(_metrics.Line + _metrics.Gap * 2);
        const auto middle = (row.top + row.bottom) / 2;
        const UINT flags = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        Text(item.hDC, profile.Name, {x, row.top + Px(_metrics.Unit), row.right - Px(_metrics.Gap), middle}, false,
             false, flags, ink);
        Text(item.hDC, LText(StatusText(status)),
             {x, middle, row.right - Px(_metrics.Gap), row.bottom - Px(_metrics.Unit)}, false, false, flags, muted);
        if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
        {
            auto focus = row;
            InflateRect(&focus, -Px(3), -Px(3));
            DrawFocusRect(item.hDC, &focus);
        }
    }

    void LibraryMenu(POINT screen)
    {
        if (screen.x == -1 && screen.y == -1)
        {
            const auto row = SendMessageW(_list, LB_GETCURSEL, 0, 0);
            RECT rect{};
            if (row == LB_ERR || SendMessageW(_list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect)) == LB_ERR)
            {
                return;
            }
            screen = {rect.left + Px(_metrics.Margin), rect.bottom};
            ClientToScreen(_list, &screen);
        }
        else
        {
            auto point = screen;
            ScreenToClient(_list, &point);
            const auto hit = SendMessageW(_list, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
            RECT rect{};
            if (HIWORD(hit) ||
                SendMessageW(_list, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&rect)) == LB_ERR ||
                !PtInRect(&rect, point))
            {
                return;
            }
            SendMessageW(_list, LB_SETCURSEL, LOWORD(hit), 0);
            RefreshSelection();
        }
        SetFocus(_list);
        std::unique_ptr<std::remove_pointer_t<HMENU>, decltype(&DestroyMenu)> menu(CreatePopupMenu(), DestroyMenu);
        if (!menu)
        {
            WinError(L"CreatePopupMenu");
        }
        const auto append = [&](int id, HWND control, std::wstring_view shortcut) {
            const auto caption = Read(control) + L"\t" + std::wstring(shortcut);
            AppendMenuW(menu.get(), MF_STRING | (IsWindowEnabled(control) ? MF_ENABLED : MF_GRAYED), id,
                        caption.c_str());
        };
        append(StartVm, _start, L"Enter");
        append(EditVm, _edit, L"F2");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        append(RemoveVm, _remove, L"Del");
        TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, _window, nullptr);
    }

    void ConfigureInterface()
    {
        InterfaceSettingsWindow settings(_state.Interface);
        settings.Open(LText(IDS_AppSettings), _window, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN);
        settings.Modal();
        if (settings.Result && *settings.Result != _state.Interface)
        {
            auto next = _state;
            const bool languageChanged = next.Interface.Language != settings.Result->Language;
            next.Interface = *settings.Result;
            SaveState(next);
            _state = std::move(next);
            if (languageChanged)
            {
                ApplyUiLanguage(_state.Interface.Language);
                RefreshLanguage();
            }
            SetAppearance(_state.Interface.Theme);
            for (const auto& vm : _vms)
            {
                if (vm->Get())
                {
                    vm->SetInterface(_state.Interface);
                    if (languageChanged)
                    {
                        vm->RefreshLanguage();
                    }
                }
            }
        }
    }

    void Configure()
    {
        SetupWindow setup(_state);
        setup.Open(LText(IDS_EngineSetup), _window,
                   WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN);
        setup.Modal();
        if (setup.Result)
        {
            auto next = _state;
            next.Tools = std::move(setup.Result->Tools);
            SaveState(next);
            _state = std::move(next);
            KillTimer(_window, DefaultsTimer);
            _probe = {};
            _capabilities = std::move(setup.Result->Detected);
            _engineReady = EngineReady();
            RefreshSelection();
        }
    }

    void BeginProbe()
    {
        _probe = std::async(std::launch::async, [engine = _state.Tools] { return ProbeEngine(engine); });
        SetTimer(_window, DefaultsTimer, 100, nullptr);
    }

    bool PrepareEngine()
    {
        if (!EngineReady())
        {
            Configure();
        }
        if (!EngineReady())
        {
            return false;
        }
        if (!_capabilities)
        {
            if (!_probe.valid())
            {
                BeginProbe();
            }
            KillTimer(_window, DefaultsTimer);
            auto probe = std::move(_probe);
            std::optional<Capabilities> capabilities;
            Busy(_window, _appearance, LText(IDS_EngineValidate), [&] { capabilities = probe.get(); });
            _capabilities = std::move(capabilities);
            RefreshSelection();
        }
        return true;
    }

    ProcessCommand NextCommand(const Profile& profile, const Machine& machine)
    {
        // Keep the bound loopback ports for this VM until launch. Previewing never starts a listener.
        const auto& connections = _connections[profile.Id];
        return LaunchCommand(_state.Tools, profile, machine, connections.Control, &connections.Video);
    }

    void EditProfile(const Profile* existing)
    {
        if (existing)
        {
            const auto open = FindWindow(existing->Id);
            if (open != _vms.end() && (*open)->Status() != VmStatus::Stopped && (*open)->Status() != VmStatus::Failed)
            {
                (*open)->ViewSettings(_window);
                return;
            }
        }
        // A modal settings change can replace the profile vector.
        const auto copy = existing ? std::optional(*existing) : std::nullopt;
        if (!PrepareEngine())
        {
            return;
        }
        auto result = ShowProfileWindow(
            _window, _appearance, _state.Tools, *_capabilities, copy ? &*copy : nullptr,
            [this](const Profile& profile, const Machine& machine) { return NextCommand(profile, machine).Text; });
        if (!result)
        {
            return;
        }
        auto next = _state;
        const auto id = result->Config.Id;
        const auto disk = result->Config.Primary.File;
        if (copy)
        {
            const auto profile = std::ranges::find(next.Profiles, id, &Profile::Id);
            *profile = std::move(result->Config);
        }
        else
        {
            next.Profiles.push_back(std::move(result->Config));
        }
        try
        {
            SaveState(next);
        }
        catch (...)
        {
            if (result->NewDisk)
            {
                std::error_code error;
                Fs::remove(disk, error);
            }
            throw;
        }
        _state = std::move(next);
        Populate(id);
    }

    void RemoveProfile()
    {
        const auto profile = Selected();
        if (!profile || FindWindow(profile->Id) != _vms.end())
        {
            return;
        }

        constexpr int RemoveOnly = 1000;
        constexpr int RemoveWithDisk = 1001;
        const auto diskExists =
            profile->Primary.Format == L"qcow2" && !profile->Primary.Protected() && IsFile(profile->Primary.File);
        const auto shared = diskExists && DiskUsedElsewhere(_state, *profile);
        const auto title = LText(IDS_App);
        const auto prompt = LFmt(IDS_RemoveConfirm, profile->Name);
        const auto detail = shared       ? LText(IDS_RemoveDiskShared)
                            : diskExists ? LFmt(IDS_RemoveDiskPath, Fs::absolute(profile->Primary.File).native())
                                         : std::wstring{};
        const auto profileLabel = LText(IDS_RemoveProfileOnly);
        const auto diskLabel = LText(IDS_RemoveWithDisk);
        const TASKDIALOG_BUTTON actions[]{{RemoveOnly, profileLabel.c_str()}, {RemoveWithDisk, diskLabel.c_str()}};
        TASKDIALOGCONFIG box{};
        box.cbSize = sizeof(box);
        box.hwndParent = _window;
        box.pszWindowTitle = title.c_str();
        box.pszMainInstruction = prompt.c_str();
        box.pszContent = detail.empty() ? nullptr : detail.c_str();
        box.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_COMMAND_LINKS;
        box.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        box.cButtons = diskExists && !shared ? 2u : 1u;
        box.pButtons = actions;
        box.nDefaultButton = IDCANCEL;
        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&box, &choice, nullptr, nullptr)))
        {
            Fail(LText(IDS_RemoveDialogError));
        }
        if (choice != RemoveOnly && choice != RemoveWithDisk)
        {
            return;
        }

        const auto index = static_cast<size_t>(profile - _state.Profiles.data());
        const auto id = profile->Id;
        if (choice == RemoveWithDisk && DiskUsedElsewhere(_state, *profile))
        {
            Fail(LText(IDS_RemoveDiskShared));
        }
        auto next = _state;
        std::erase_if(next.Profiles, [&](const Profile& candidate) { return candidate.Id == id; });
        SaveProfileRemoval(_state, next, choice == RemoveWithDisk ? profile->Primary.File : Fs::path{});
        _connections.erase(id);
        _state = std::move(next);
        Populate(_state.Profiles.empty() ? L"" : _state.Profiles[std::min(index, _state.Profiles.size() - 1)].Id);
        SetFocus(_state.Profiles.empty() ? _first : _list);
    }

    void SavePreference(const std::wstring& id, bool Profile::* setting, bool value)
    {
        auto profile = std::ranges::find(_state.Profiles, id, &Profile::Id);
        if (profile == _state.Profiles.end())
        {
            Fail(LText(IDS_InvalidVmId));
        }
        auto& preference = (*profile).*setting;
        const bool previous = std::exchange(preference, value);
        try
        {
            SaveState(_state);
        }
        catch (...)
        {
            preference = previous;
            throw;
        }
    }

    void StartProfile()
    {
        const auto selected = Selected();
        if (!selected)
        {
            return;
        }
        if (const auto existing = FindWindow(selected->Id); existing != _vms.end())
        {
            if (IsIconic((*existing)->Get()))
            {
                ShowWindow((*existing)->Get(), SW_RESTORE);
            }
            SetForegroundWindow((*existing)->Get());
            return;
        }
        auto profile = *selected;
        if (!PrepareEngine())
        {
            return;
        }
        const auto base = _capabilities->Find(profile.Machine);
        if (!base)
        {
            Fail(LFmt(IDS_MachineUnavailable, profile.Machine));
        }
        Machine machine;
        std::vector<VmSnapshot> snapshots;
        Busy(_window, _appearance, LText(IDS_HardwareLoading), [&] {
            machine = ProbeMachine(_state.Tools, *base);
            snapshots = InspectStorage(profile, _state.Tools);
            ValidateProfile(profile, machine, *_capabilities, _state.Tools);
            if (profile.SkipSnapshotPrompt)
            {
                snapshots.clear();
            }
        });
        if (machine.Properties.contains(L"nvram"))
        {
            const auto path = NvramPath(profile);
            if (std::ranges::any_of(_vms, [&](const auto& vm) { return vm->UsesNvram(path); }))
            {
                Fail(LText(IDS_NvramInUse));
            }
        }

        std::optional<VmSnapshot> resume;
        if (!snapshots.empty())
        {
            SnapshotWindow picker(_appearance, snapshots);
            picker.Open(LFmt(IDS_SnapshotBootTitle, profile.Name), _window,
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN);
            picker.Modal();
            if (!picker.SelectedSource)
            {
                return;
            }
            if (picker.SkipPrompt)
            {
                SavePreference(profile.Id, &Profile::SkipSnapshotPrompt, true);
                profile.SkipSnapshotPrompt = true;
            }
            else if (*picker.SelectedSource)
            {
                resume = snapshots.at(*picker.SelectedSource - 1);
            }
        }
        if (machine.Properties.contains(L"nvram") && profile.Nvram.empty())
        {
            Fs::create_directories(NvramPath(profile).parent_path());
        }
        auto command = NextCommand(profile, machine);
        auto connections = _connections.extract(profile.Id);
        auto vm = std::make_unique<VmWindow>(
            _window, _state.Interface, profile, machine, *_capabilities, std::move(command),
            std::move(connections.mapped()), std::move(resume), _state.Tools, [this](const Profile& changed) {
                auto next = _state;
                const auto target = std::ranges::find(next.Profiles, changed.Id, &Profile::Id);
                if (target == next.Profiles.end())
                {
                    Fail(LText(IDS_InvalidVmId));
                }
                target->DisplayOverride = changed.DisplayOverride;
                target->ForcePs2 = changed.ForcePs2;
                target->UsbMouse = changed.UsbMouse;
                target->MachineOptions = changed.MachineOptions;
                target->Primary = changed.Primary;
                target->Optical = changed.Optical;
                target->Drives = changed.Drives;
                SaveState(next);
                _state = std::move(next);
                RefreshSelection();
            });
        vm->Open(profile.Name, nullptr, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, WS_EX_APPWINDOW | WS_EX_CONTROLPARENT,
                 _window);
        ShowWindow(vm->Get(), SW_SHOW);
        _vms.push_back(std::move(vm));
        RefreshLibraryStatus();
        UpdateSessionActions();
        Refresh();
    }

    POINT MinimumSize() const override
    {
        return {_metrics.Character * 58, _metrics.Line * 16};
    }
    SIZE PreferredSize(SIZE available) const override
    {
        const bool empty = _state.Profiles.empty();
        return {std::min<LONG>(available.cx, _metrics.Character * (empty ? 60 : 96) + 2 * _metrics.Margin),
                std::min<LONG>(available.cy, _metrics.Line * (empty ? 16 : 25) + _metrics.Footer())};
    }

    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override
    {
        switch (message)
        {
        case WM_TIMER:
            if (wparam == DefaultsTimer && _probe.valid() &&
                _probe.wait_for(std::chrono::seconds::zero()) == std::future_status::ready)
            {
                KillTimer(_window, DefaultsTimer);
                try
                {
                    _capabilities = _probe.get();
                }
                catch (const std::exception&)
                {
                    _capabilities.reset();
                }
                RefreshSelection();
            }
            return 0;
        case WM_DRAWITEM:
            if (wparam == VmList)
            {
                DrawProfile(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
                return TRUE;
            }
            break;
        case ViewVmSettings:
            if (IsWindowEnabled(_window))
            {
                const auto found = std::ranges::find_if(
                    _vms, [lparam](const auto& vm) { return vm->Get() == reinterpret_cast<HWND>(lparam); });
                if (found != _vms.end())
                {
                    (*found)->ViewSettings(_window);
                }
            }
            return 0;
        case VmStateChanged:
            std::erase_if(_vms, [](const auto& vm) { return !vm->Get(); });
            RefreshLibraryStatus();
            UpdateSessionActions();
            Refresh();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE && _details)
            {
                const auto ready = EngineReady();
                if (_engineReady != ready)
                {
                    _engineReady = ready;
                    _capabilities.reset();
                    if (ready && !_state.Profiles.empty())
                    {
                        BeginProbe();
                    }
                    else
                    {
                        KillTimer(_window, DefaultsTimer);
                        _probe = {};
                    }
                    RefreshSelection();
                }
            }
            break;
        case WM_SETFOCUS:
            SetFocus(Selected() ? _list : _first);
            return 0;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wparam) == _list && Selected())
            {
                LibraryMenu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wparam) == VmList)
            {
                if (HIWORD(wparam) == LBN_SELCHANGE)
                {
                    RefreshSelection();
                }
                if (HIWORD(wparam) == LBN_DBLCLK)
                {
                    StartProfile();
                }
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED)
            {
                switch (LOWORD(wparam))
                {
                case FirstAction:
                    if (EngineReady())
                    {
                        EditProfile(nullptr);
                    }
                    else
                    {
                        Configure();
                    }
                    return 0;
                case CreateVm:
                    EditProfile(nullptr);
                    return 0;
                case EditVm:
                    if (const auto profile = Selected())
                    {
                        EditProfile(profile);
                    }
                    return 0;
                case RemoveVm:
                    RemoveProfile();
                    return 0;
                case StartVm:
                    StartProfile();
                    return 0;
                case Setup:
                    Configure();
                    return 0;
                case AppSettings:
                    ConfigureInterface();
                    return 0;
                default:
                    break;
                }
            }
            break;
        case WM_CLOSE: {
            // Copy HWNDs because nested message loops may remove closed windows.
            std::vector<HWND> windows;
            for (const auto& vm : _vms)
            {
                if (vm->Get())
                {
                    windows.push_back(vm->Get());
                }
            }
            for (const auto window : windows)
            {
                SendMessageW(window, WM_CLOSE, 0, 0);
                if (IsWindow(window))
                {
                    return 0;
                }
            }
            DestroyWindow(_window);
            return 0;
        }
        case WM_DESTROY:
            _vms.clear();
            PostQuitMessage(0);
            return 0;
        }
        return Window::Message(message, wparam, lparam);
    }

    State _state;
    std::optional<Capabilities> _capabilities;
    std::future<Capabilities> _probe;
    std::map<std::wstring, VmConnections> _connections;
    std::vector<std::unique_ptr<VmWindow>> _vms;
    int _width{}, _height{}, _side{};
    int _libraryRule{}, _detailRule{};
    bool _engineReady{};
    HWND _library{}, _title{}, _emptyTitle{}, _first{};
    HWND _new{}, _edit{}, _remove{}, _start{}, _setup{}, _settings{}, _list{}, _details{};
};
} // namespace
} // namespace Ia64::Ui

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show)
{
    using namespace Ia64;
    const auto apartment = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(apartment))
    {
        return 1;
    }
    struct Apartment
    {
        ~Apartment()
        {
            RoUninitialize();
        }
    } scope;
    BufferedPaintInit();
    struct Painting
    {
        ~Painting()
        {
            BufferedPaintUnInit();
        }
    } painting;
    try
    {
        auto state = LoadState();
        ApplyUiLanguage(state.Interface.Language);
        Handle instance(CreateMutexW(nullptr, FALSE, L"Local\\Qemu.Ia64.Windows.Manager"));
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            Fail(LText(IDS_AlreadyRunning));
        }
        if (!instance)
        {
            WinError(L"CreateMutexW");
        }
        Network network;
        const winrt::Windows::UI::ViewManagement::UISettings display;
        const auto notifyScale = [thread = GetCurrentThreadId()](const auto& sender, const auto&) {
            Ui::Window::NotifyTextScale(sender.TextScaleFactor(), thread);
        };
        const auto subscription = display.TextScaleFactorChanged(winrt::auto_revoke, notifyScale);
        notifyScale(display, nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&controls);
        Ui::Manager manager(std::move(state));
        manager.Open(LText(IDS_App), nullptr, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN);
        ShowWindow(manager.Get(), show);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (manager.TranslateKey(msg))
            {
                continue;
            }
            const auto root = msg.hwnd ? GetAncestor(msg.hwnd, GA_ROOT) : nullptr;
            if (root && msg.hwnd != root && IsDialogMessageW(root, &msg))
            {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }
    catch (const std::exception& error)
    {
        Ui::ShowError(nullptr, Wide(error.what()));
        return 1;
    }
}
