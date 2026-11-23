// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Wizard.h"
#include "Localization.h"
#include "Media.h"
#include "Devices.h"
#include "StorageWindow.h"
#include "ProfileDetails.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cwctype>
#include <cmath>
#include <format>
#include <utility>

namespace Ia64::Ui
{
namespace
{
enum class Page
{
    Identity,
    Hardware,
    Storage,
    Display,
    Advanced,
    Review,
    System,
    Processor,
    Memory,
    Controller,
    Disk,
    Optical,
    Ps2,
    Usb,
    Pcie,
    Dma,
    CommandLine,
    Input,
    Network,
    Ports,
    MachineOptions
};

struct PageInfo
{
    Page Id;
    int Title;
    std::optional<Page> Parent{};
    std::optional<size_t> Option{};
};

constexpr std::array CreationPages{PageInfo{Page::Identity, IDS_Identity}, PageInfo{Page::Hardware, IDS_Hardware},
                                   PageInfo{Page::Storage, IDS_Storage},   PageInfo{Page::Display, IDS_Display},
                                   PageInfo{Page::Input, IDS_Input},       PageInfo{Page::Network, IDS_NetworkAdapter},
                                   PageInfo{Page::Ports, IDS_Ports},       PageInfo{Page::Advanced, IDS_Advanced},
                                   PageInfo{Page::Review, IDS_Review}};
constexpr std::array SettingsPages{PageInfo{Page::System, IDS_Hardware},
                                   PageInfo{Page::Processor, IDS_Processor, Page::System},
                                   PageInfo{Page::Memory, IDS_Memory, Page::System},
                                   PageInfo{Page::Input, IDS_Input, Page::System},
                                   PageInfo{Page::Ps2, IDS_Ps2Device, Page::Input, 0},
                                   PageInfo{Page::Usb, IDS_UsbDevice, Page::Input, 1},
                                   PageInfo{Page::Network, IDS_NetworkAdapter, Page::System},
                                   PageInfo{Page::Ports, IDS_Ports, Page::System},
                                   PageInfo{Page::Controller, IDS_Storage},
                                   PageInfo{Page::Disk, IDS_DiskFile, Page::Controller},
                                   PageInfo{Page::Optical, IDS_OpticalDrive, Page::Controller},
                                   PageInfo{Page::Display, IDS_Display},
                                   PageInfo{Page::Advanced, IDS_Advanced},
                                   PageInfo{Page::MachineOptions, IDS_MachineOptions, Page::Advanced},
                                   PageInfo{Page::Pcie, IDS_Pcie, Page::MachineOptions, 2},
                                   PageInfo{Page::Dma, IDS_Dma, Page::MachineOptions, 3},
                                   PageInfo{Page::CommandLine, IDS_LaunchCommand, Page::Advanced}};

enum Id
{
    Back = 100,
    Next,
    Cancel,
    Browse,
    MachineId = 105,
    Cpu,
    Ram,
    DiskModeId,
    DiskSize,
    SizeSlider,
    Vga,
    Llvm,
    Name,
    Iso,
    Sockets,
    Cores,
    Threads,
    Bus,
    DiskPath,
    Ati,
    Width,
    Height,
    MaxWidth,
    MaxHeight,
    Vram,
    Cache,
    RamSlider,
    VramSlider,
    InitialMode,
    MaximumMode,
    ReviewList,
    SettingsTree,
    BrowseDisk,
    NvramFile,
    BrowseNvram,
    SnapshotStartup,
    LaunchText,
    CopyLaunch,
    OptionStart = 150,
    StorageEdit = 170,
    ForcePs2,
    UsbMouse,
    Alat,
    FirmwareBase,
    LlvmThreads,
    NoReboot,
    Backend,
    NicModel,
    Mac,
    Tap,
    NetHost,
    NetPort,
    Forward,
    NetRom,
    SerialMode,
    SerialFile,
    SerialBrowse,
    SerialHost,
    SerialPort,
    DebugMode,
    DebugFile,
    DebugBrowse,
    DebugHost,
    DebugTcpPort,
    AttachmentInfo,
    DiskOptions,
    OpticalOptions
};

constexpr DWORD CommandTextStyle = ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL;

void CopyCommandText(HWND edit)
{
    DWORD start{}, end{};
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SendMessageW(edit, WM_COPY, 0, 0);
    SendMessageW(edit, EM_SETSEL, start, end);
}

enum class DiskMode
{
    None,
    Existing,
    Create
};

bool IsTestModel(std::wstring_view name)
{
    return !std::ranges::search(name, std::wstring_view(L"test"), [](wchar_t a, wchar_t b) {
                return std::towlower(a) == b;
            }).empty();
}

constexpr unsigned MiBPerGiB = 1024;
constexpr unsigned MaxDiskMiB = 64u * MiBPerGiB * 1024;
constexpr unsigned NewDiskMiB = 32 * MiBPerGiB;
constexpr int DiskSliderLast = 512;
constexpr std::array DiskMarks{16u, 32u, 64u, 128u, 256u, 512u, 1024u};

struct CapacityTick
{
    unsigned MiB;
    int Position;
};

unsigned DiskMiB(int position)
{
    if (position <= 64)
    {
        return std::max(1u, static_cast<unsigned>(position) * 256);
    }
    if (position <= 448)
    {
        const auto octave = (position - 64) / 64;
        const auto start = 16384u << octave;
        return start + start * static_cast<unsigned>((position - 64) % 64) / 64;
    }
    return 1024u * 1024 + static_cast<unsigned>(position - 448) * ((MaxDiskMiB - 1024u * 1024) / 64);
}

int DiskPosition(unsigned value)
{
    if (value <= 16384)
    {
        return static_cast<int>(value / 256);
    }
    if (value <= 1024u * 1024)
    {
        const auto start = std::bit_floor(value);
        const auto octave = std::bit_width(start) - std::bit_width(16384u);
        return 64 + static_cast<int>(64 * octave + ((value - start) * 64u / start));
    }
    return 448 + static_cast<int>((value - 1024u * 1024) / ((MaxDiskMiB - 1024u * 1024) / 64));
}

struct RamSliderScale
{
    static constexpr int Last = 4096;
    static constexpr int CommonEnd = 3456;
    static constexpr unsigned CommonMaximum = 32 * 1024;

    unsigned Minimum{};
    unsigned Maximum{};

    int Position(unsigned memory) const
    {
        if (Minimum >= Maximum)
        {
            return 0;
        }
        memory = std::clamp(memory, Minimum, Maximum);
        if (Minimum >= CommonMaximum)
        {
            return MulDiv(static_cast<int>(memory - Minimum), Last, static_cast<int>(Maximum - Minimum));
        }

        const unsigned upper = std::min(Maximum, CommonMaximum);
        if (memory > upper)
        {
            return CommonEnd +
                   MulDiv(static_cast<int>(memory - upper), Last - CommonEnd, static_cast<int>(Maximum - upper));
        }

        const int end = Maximum > upper ? CommonEnd : Last;
        const auto part =
            std::log(static_cast<double>(memory) / Minimum) / std::log(static_cast<double>(upper) / Minimum);
        return static_cast<int>(std::llround(part * end));
    }

    unsigned Memory(int position) const
    {
        if (position <= 0 || Minimum >= Maximum)
        {
            return Minimum;
        }
        if (position >= Last)
        {
            return Maximum;
        }

        const unsigned upper = std::min(Maximum, CommonMaximum);
        if (Minimum >= CommonMaximum || (Maximum > upper && position > CommonEnd))
        {
            const bool entireRange = Minimum >= CommonMaximum;
            const auto start = entireRange ? Minimum : upper;
            const auto offset = entireRange ? 0 : CommonEnd;
            return start +
                   static_cast<unsigned>(MulDiv(static_cast<int>(Maximum - start), position - offset, Last - offset));
        }

        const auto end = Maximum > upper ? CommonEnd : Last;
        const auto ratio = static_cast<double>(position) / end;
        const auto amount = Minimum * std::pow(static_cast<double>(upper) / Minimum, ratio);
        return std::clamp(static_cast<unsigned>(std::llround(amount / 32) * 32), Minimum, upper);
    }

    void ForEachVisibleTick(int width, int majorWidth, const auto& visit) const
    {
        const auto first = (uint64_t{Minimum} + MiBPerGiB - 1) / MiBPerGiB;
        const auto last = Maximum / MiBPerGiB;
        int previousTick = -1;
        for (auto gib = first; gib <= last; ++gib)
        {
            const bool major = gib >= 2 && gib <= 32 && std::has_single_bit(gib);
            const auto mib = static_cast<unsigned>(gib * MiBPerGiB);
            const auto position = Position(mib);
            const int x = MulDiv(width, position, Last);
            if (!major && x <= previousTick)
            {
                continue;
            }
            previousTick = x + (major ? majorWidth - 1 : 0);
            visit(CapacityTick{mib, position}, x, major);
        }
    }
};

struct Resolution
{
    unsigned Width{};
    unsigned Height{};
    friend bool operator==(const Resolution&, const Resolution&) = default;
};

constexpr std::array Presets{Resolution{640, 480},   Resolution{800, 600},   Resolution{1024, 768},
                             Resolution{1280, 960},  Resolution{1600, 1200}, Resolution{2048, 1536},
                             Resolution{1280, 720},  Resolution{1600, 900},  Resolution{1920, 1080},
                             Resolution{2560, 1440}, Resolution{3840, 2160}, Resolution{7680, 4320}};

std::wstring ResolutionText(Resolution resolution)
{
    return LFmt(IDS_ResolutionValue, resolution.Width, resolution.Height);
}

class ProfileWindow final : public Window
{
public:
    ProfileWindow(Appearance appearance, const Engine& engine, const Capabilities& capabilities,
                  const Profile* existing, Machine machine, CommandPreview preview,
                  std::optional<std::wstring> activeCommand = std::nullopt)
        : Window(appearance), _engine(engine), _capabilities(capabilities), _editing(existing != nullptr),
          _preview(std::move(preview)), _activeCommand(std::move(activeCommand)), _selected(std::move(machine)),
          _page(existing ? Page::System : Page::Identity)
    {
        if (existing)
        {
            _draft = *existing;
            if (!_draft.Primary.File.empty())
            {
                _diskMode = DiskMode::Existing;
                _existingPath = _draft.Primary.File;
            }
        }
        else
        {
            _draft.Id = NewId();
        }
    }

    std::optional<ProfileResult> Result;

private:
    enum class RowKind
    {
        Input,
        Note,
        Scale
    };
    struct Row
    {
        HWND Caption{};
        HWND Value{};
        HWND Action{};
        RowKind Kind{};
        bool Shown{true};
        int Height{};
    };

    void Create() override
    {
        _title = Label(L"", TextRole::Title, SS_CENTERIMAGE | SS_ENDELLIPSIS);
        if (_editing)
        {
            _tree = Child(WC_TREEVIEWW, LText(IDS_SettingsTree), SettingsTree,
                          WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT | TVS_SHOWSELALWAYS |
                              TVS_NONEVENHEIGHT);
            UpdateTree();
            StorageTree();
            TreeView_Expand(_tree, _nodes.at(Page::System), TVE_EXPAND);
            TreeView_Expand(_tree, _nodes.at(Page::Controller), TVE_EXPAND);
            TreeView_Expand(_tree, _nodes.at(Page::Advanced), TVE_EXPAND);
        }
        else
        {
            _step = Label(L"", TextRole::Secondary, SS_RIGHT | SS_CENTERIMAGE);
            _back = Button(Back, LText(IDS_Back));
        }
        if (_activeCommand)
        {
            _cancel = Button(IDCANCEL, LText(IDS_CloseCommand), true);
        }
        else
        {
            _next = Button(Next, LText(_editing ? IDS_Save : IDS_Next), true);
            _cancel = Button(Cancel, LText(IDS_Cancel));
        }
        Build(_page);
        if (_editing)
        {
            TreeView_SelectItem(_tree, _nodes.at(_page));
            SetFocus(_tree);
        }
    }

    void UpdateTree()
    {
        std::map<HTREEITEM, HTREEITEM> lastChild;
        for (const auto& entry : SettingsPages)
        {
            if (entry.Id == Page::Disk || entry.Id == Page::Optical)
            {
                continue;
            }
            const auto found = _nodes.find(entry.Id);
            if (entry.Option && !_selected.Properties.contains(DeviceOptions[*entry.Option].first))
            {
                if (found != _nodes.end())
                {
                    TreeView_DeleteItem(_tree, found->second);
                    _nodes.erase(found);
                }
                continue;
            }
            const auto parent = entry.Parent ? _nodes.at(*entry.Parent) : TVI_ROOT;
            if (found == _nodes.end())
            {
                auto name = LText(entry.Title);
                TVINSERTSTRUCTW item{};
                item.hParent = parent;
                item.hInsertAfter = lastChild.contains(parent) ? lastChild.at(parent) : TVI_FIRST;
                item.item.mask = TVIF_TEXT | TVIF_PARAM;
                item.item.pszText = name.data();
                item.item.lParam = static_cast<LPARAM>(entry.Id);
                _nodes[entry.Id] = TreeView_InsertItem(_tree, &item);
            }
            lastChild[parent] = _nodes.at(entry.Id);
        }
    }

    void StorageTree()
    {
        if (!_tree)
        {
            return;
        }
        const bool building = std::exchange(_building, true);
        auto root = _nodes.at(Page::Controller);
        _connections.clear();
        for (auto child = TreeView_GetChild(_tree, root); child; child = TreeView_GetChild(_tree, root))
        {
            TreeView_DeleteItem(_tree, child);
        }
        const auto insert = [&](HTREEITEM parent, std::wstring name, Page page, const std::wstring& id) {
            TVINSERTSTRUCTW item{};
            item.hParent = parent;
            item.hInsertAfter = TVI_LAST;
            item.item.mask = TVIF_TEXT | TVIF_PARAM;
            item.item.lParam = static_cast<LPARAM>(page);
            item.item.pszText = name.data();
            const auto node = TreeView_InsertItem(_tree, &item);
            _connections.emplace(node, id);
            if (page != Page::Controller)
            {
                _nodes[page] = node;
            }
            return node;
        };
        std::vector<StorageController> controllers{{L"onboard", _draft.Bus}};
        controllers.insert(controllers.end(), _draft.Controllers.begin(), _draft.Controllers.end());
        HTREEITEM select = root;
        for (const auto& c : controllers)
        {
            auto parent = insert(root, LFmt(IDS_ControllerName, c.Model), Page::Controller, c.Id);
            if (_storageSelection == c.Id)
            {
                select = parent;
            }
            for (const auto d : AllDrives(_draft))
            {
                if (d->Controller == c.Id)
                {
                    auto page = d == &_draft.Primary   ? Page::Disk
                                : d == &_draft.Optical ? Page::Optical
                                                       : Page::Controller;
                    auto name = page == Page::Disk      ? LText(IDS_DiskFile)
                                : page == Page::Optical ? LText(IDS_OpticalDrive)
                                : d->File.empty()       ? d->Media
                                                        : d->File.filename().native();
                    auto item = insert(parent, name, page, d->Id);
                    if (_storageSelection == d->Id)
                    {
                        select = item;
                    }
                }
            }
            TreeView_Expand(_tree, parent, TVE_EXPAND);
        }
        if (_page == Page::Controller || _page == Page::Disk || _page == Page::Optical)
        {
            TreeView_SelectItem(_tree, select);
        }
        _building = building;
    }

    void ConnectionSummary()
    {
        if (!_editing || !_forms.contains(Page::Controller) || !Find(AttachmentInfo))
        {
            return;
        }
        const bool root = _storageSelection.empty() || _storageSelection == L"onboard";
        ShowRow(Find(Bus), root);
        ShowRow(Find(AttachmentInfo), !root);
        if (!root)
        {
            auto drives = AllDrives(_draft);
            auto d = std::ranges::find_if(drives, [&](const Drive* drive) { return drive->Id == _storageSelection; });
            if (d != drives.end())
            {
                Set(Find(AttachmentInfo), std::format(L"{} · {} · {}", (*d)->Media, (*d)->Slot, (*d)->File.native()));
            }
            else if (auto c = std::ranges::find(_draft.Controllers, _storageSelection, &StorageController::Id);
                     c != _draft.Controllers.end())
            {
                Set(Find(AttachmentInfo), c->Model);
            }
        }
    }

    HWND Field(int label, HWND value, HWND action = nullptr, RowKind kind = RowKind::Input)
    {
        if (_activeCommand && action)
        {
            DestroyWindow(action);
            action = nullptr;
        }
        Rows().push_back({label ? Label(LText(label), TextRole::Body, SS_CENTERIMAGE | SS_ENDELLIPSIS) : nullptr, value,
                          action, kind});
        return value;
    }

    HWND Find(int id) const
    {
        return GetDlgItem(_window, id);
    }
    std::vector<Row>& Rows()
    {
        return _forms[_page];
    }

    HWND Choice(int id, int label, const std::vector<std::wstring>& choices, std::wstring_view selection)
    {
        const auto combo = Field(label, Combo(id));
        Options(combo, choices, selection);
        return combo;
    }

    HWND Numeric(int id, int label, unsigned value)
    {
        return Field(label, Edit(id, value ? std::to_wstring(value) : L"", ES_NUMBER));
    }

    HWND Note(std::wstring_view message)
    {
        return Field(0, Label(message, TextRole::Secondary), nullptr, RowKind::Note);
    }

    void ShowRow(HWND value, bool show)
    {
        for (auto& [page, rows] : _forms)
        {
            if (const auto row = std::ranges::find(rows, value, &Row::Value); row != rows.end())
            {
                row->Shown = show;
                return;
            }
        }
    }

    void Build(Page page)
    {
        _building = true;
        _draggingCapacity = false;
        for (const auto& row : Rows())
        {
            for (auto item : {row.Caption, row.Value, row.Action})
            {
                if (item)
                {
                    if (_editing)
                    {
                        ShowWindow(item, SW_HIDE);
                    }
                    else
                    {
                        DestroyWindow(item);
                    }
                }
            }
        }
        if (!_editing)
        {
            Rows().clear();
        }
        _page = page;
        _scroll = 0;
        const auto pages =
            _editing ? std::span<const PageInfo>(SettingsPages) : std::span<const PageInfo>(CreationPages);
        const auto entry = std::ranges::find(pages, page, &PageInfo::Id);
        Set(_title, LText(entry->Title));
        if (!_editing)
        {
            Set(_step, std::format(L"{} / {}", entry - pages.begin() + 1, pages.size()));
        }

        if (Rows().empty())
        {
            switch (page)
            {
            case Page::Identity:
                NameField();
                Optical();
                break;
            case Page::Hardware:
                MachineField();
                if (!_recommendation.Description.empty())
                {
                    Note(_recommendation.Description);
                }
                Processor();
                Memory();
                break;
            case Page::Storage:
                Disk();
                Controller();
                break;
            case Page::Input:
                BoolChoice(ForcePs2, IDS_ForcePs2, _draft.ForcePs2);
                BoolChoice(UsbMouse, IDS_UsbMouse, _draft.UsbMouse);
                break;
            case Page::MachineOptions:
                MachineOptions();
                break;
            case Page::Ports:
                PortFields(SerialMode, _draft.Serial);
                PortFields(DebugMode, _draft.DebugPort);
                break;
            case Page::Network:
                NetworkFields();
                break;
            case Page::Display:
                Display();
                break;
            case Page::Advanced:
                Advanced();
                break;
            case Page::Review:
                Review();
                break;
            case Page::System:
                NameField();
                MachineField();
                break;
            case Page::Processor:
                Processor();
                break;
            case Page::Memory:
                Memory();
                break;
            case Page::Controller:
                Controller();
                break;
            case Page::Disk:
                Disk();
                Field(0, Button(DiskOptions, LText(_activeCommand ? IDS_ViewConnections : IDS_ConfigureDevices)));
                break;
            case Page::Optical:
                Optical();
                Field(0, Button(OpticalOptions, LText(_activeCommand ? IDS_ViewConnections : IDS_ConfigureDevices)));
                break;
            case Page::CommandLine:
                UseFont(Field(0, Edit(LaunchText, L"", CommandTextStyle)), TextRole::Monospace);
                Field(0, Button(CopyLaunch, LText(IDS_CopyCommand)));
                break;
            case Page::Ps2:
            case Page::Usb:
            case Page::Pcie:
            case Page::Dma:
                OptionField(*entry->Option, IDS_DeviceState);
                break;
            }
        }

        if (page == Page::Controller)
        {
            ConnectionSummary();
        }
        if (page == Page::CommandLine)
        {
            UpdateCommandLine();
        }
        if (_activeCommand)
        {
            for (const auto& row : Rows())
            {
                wchar_t type[32]{};
                GetClassNameW(row.Value, type, static_cast<int>(std::size(type)));
                if (_wcsicmp(type, WC_EDITW) == 0)
                {
                    SendMessageW(row.Value, EM_SETREADONLY, TRUE, 0);
                }
                else if (_wcsicmp(type, WC_COMBOBOXW) == 0 || _wcsicmp(type, TRACKBAR_CLASSW) == 0)
                {
                    Enable(row.Value, false);
                }
            }
        }
        _building = false;
        HWND previous = _editing ? _tree : _step;
        const auto order = [&](HWND control) {
            if (control)
            {
                SetWindowPos(control, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                previous = control;
            }
        };
        for (const auto& row : Rows())
        {
            for (const auto control : {row.Caption, row.Value, row.Action})
            {
                order(control);
            }
        }
        for (const auto control : {_back, _next, _cancel})
        {
            order(control);
        }
        if (!_editing)
        {
            Enable(_back, page != Page::Identity);
            Set(_next, LText(page == Page::Review ? IDS_Finish : IDS_Next));
        }
        const auto slider = page == Page::Memory || page == Page::Hardware ? Find(RamSlider) : Find(SizeSlider);
        if (slider)
        {
            _capacityPosition = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        }
        Refresh();
        if (!_editing)
        {
            SetFocus(!Rows().empty() && (GetWindowLongPtrW(Rows().front().Value, GWL_STYLE) & WS_TABSTOP)
                         ? Rows().front().Value
                         : _next);
        }
    }

    void NameField()
    {
        Field(IDS_Name, Edit(Name, _draft.Name));
        SendMessageW(Find(Name), EM_SETLIMITTEXT, 100, 0);
    }

    void Optical()
    {
        const auto iso = Field(IDS_Iso, Edit(Iso, _draft.Optical.File.native()), Button(Browse, LText(IDS_Browse)));
        DefaultHint(iso, LText(IDS_None), _activeCommand.has_value());
        if (!_activeCommand)
        {
            EditCue(iso, LText(IDS_SelectIso));
        }
        _hint = Note(L"");
        ShowRecommendation();
    }

    void MachineField()
    {
        const auto field = Field(IDS_Machine, Combo(MachineId));
        DefaultHint(field, Profile{}.Machine);
        MachineChoices();
    }

    void MachineChoices()
    {
        std::vector<std::wstring> machines;
        for (const auto& item : _capabilities.Machines)
        {
            if (!IsTestModel(item.Name))
            {
                machines.push_back(item.Name);
            }
        }
        auto selected = _draft.Machine;
        if (IsTestModel(selected))
        {
            selected = LText(IDS_KeepMachine);
            machines.insert(machines.begin(), selected);
        }
        else if (!_capabilities.Find(selected))
        {
            machines.insert(machines.begin(), _draft.Machine);
        }
        Options(Find(MachineId), machines, selected);
    }

    void CpuChoices(const std::wstring& selected)
    {
        std::vector cpus{LFmt(IDS_DefaultValue, MachineInfo().DefaultCpu)};
        cpus.insert(cpus.end(), _capabilities.Cpus.begin(), _capabilities.Cpus.end());
        Options(Find(Cpu), cpus, selected);
    }

    void Processor()
    {
        Field(IDS_Cpu, Combo(Cpu));
        CpuChoices(_draft.Cpu);
        DefaultHint(Numeric(Sockets, IDS_Sockets, _draft.Sockets), L"1", false);
        DefaultHint(Numeric(Cores, IDS_Cores, _draft.Cores), L"1", false);
        DefaultHint(Numeric(Threads, IDS_Threads, _draft.Threads), L"1", false);
        if (_editing)
        {
            _cpuLimit = Note(LFmt(IDS_ProcessorLimit, MachineInfo().MaxCpus));
        }
    }

    void Memory()
    {
        Numeric(Ram, IDS_Ram, _draft.RamMiB);
        Slider(RamSlider);
        _memoryLimit = Note(L"");
        MemoryRange(_draft.RamMiB);
    }

    void MemoryRange(unsigned memory)
    {
        const auto& current = MachineInfo();
        DefaultHint(Find(Ram), std::to_wstring(DefaultRam(current.Name)), false);
        const auto [minimum, machineMaximum] = RamRange(current.Name);
        _ramScale = {minimum, std::min(InstalledRamMiB(), machineMaximum)};
        const auto slider = Find(RamSlider);
        SetSlider(slider, 0, RamSliderScale::Last, _ramScale.Position(memory));
        Enable(slider, _ramScale.Minimum < _ramScale.Maximum);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, RamSliderScale::Last / 16);
        Set(_memoryLimit, _editing ? LFmt(IDS_MemoryLimits, minimum, _ramScale.Maximum)
                                   : LFmt(IDS_MachineLimits, current.MaxCpus, minimum, _ramScale.Maximum));
    }

    HWND Slider(int id, bool ticks = false)
    {
        return Field(0, Child(TRACKBAR_CLASSW, L"", id, WS_TABSTOP | (ticks ? TBS_BOTTOM : TBS_NOTICKS)), nullptr,
                     id == VramSlider ? RowKind::Input : RowKind::Scale);
    }

    static void SetSlider(HWND slider, int minimum, int maximum, int position)
    {
        SendMessageW(slider, TBM_SETRANGEMIN, FALSE, minimum);
        SendMessageW(slider, TBM_SETRANGEMAX, TRUE, maximum);
        SendMessageW(slider, TBM_SETPOS, TRUE, std::clamp(position, minimum, maximum));
    }

    Fs::path DefaultDisk() const
    {
        auto name = _draft.Name;
        std::ranges::replace_if(
            name, [](wchar_t c) { return c < 32 || std::wstring_view(L"<>:\"/\\|?*").contains(c); }, L'_');
        while (!name.empty() && (name.back() == L' ' || name.back() == L'.'))
        {
            name.pop_back();
        }
        auto stem = name.substr(0, name.find(L'.'));
        std::ranges::transform(stem, stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        if (stem.empty() || stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
             std::wstring_view(L"123456789¹²³").contains(stem.back())))
        {
            name.insert(name.begin(), L'_');
        }
        return DataDirectory() / L"VMs" / _draft.Id / (name + L".qcow2");
    }

    void Disk()
    {
        const std::vector modes{LFmt(IDS_DefaultValue, LText(IDS_NoDisk)), LText(IDS_Existing), LText(IDS_CreateDisk)};
        Choice(DiskModeId, IDS_DiskMode, modes, modes[static_cast<size_t>(_diskMode)]);
        auto path = _diskMode == DiskMode::Create     ? (_newDiskPath.empty() ? DefaultDisk() : _newDiskPath)
                    : _diskMode == DiskMode::Existing ? _existingPath
                                                      : Fs::path{};
        Field(IDS_DiskPath, Edit(DiskPath, path.native()), Button(BrowseDisk, LText(IDS_Browse)));
        DefaultHint(Numeric(DiskSize, IDS_DiskSize, _size), std::to_wstring(NewDiskMiB), false);
        const auto slider = Slider(SizeSlider, true);
        _capacityPosition = DiskPosition(_size);
        SetSlider(slider, 0, DiskSliderLast, _capacityPosition);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 16);
        for (const auto gib : DiskMarks)
        {
            SendMessageW(slider, TBM_SETTIC, 0, DiskPosition(gib * MiBPerGiB));
        }
        UpdateStorage();
    }

    void Controller()
    {
        const auto buses = Buses(_draft.Machine);
        DefaultHint(Choice(Bus, IDS_Bus, buses, _draft.Bus), buses.front());
        if (_editing)
        {
            Field(0, Child(WC_STATICW, L"", AttachmentInfo, SS_LEFT), nullptr, RowKind::Note);
        }
        Field(0, Button(StorageEdit, LText(_activeCommand ? IDS_ViewConnections : IDS_ConfigureDevices)));
    }

    std::optional<RECT> TickArea(int id) const
    {
        const auto slider = Find(id);
        if (!slider || !IsWindowVisible(slider))
        {
            return std::nullopt;
        }
        RECT sliderRect{}, channel{};
        GetWindowRect(slider, &sliderRect);
        MapWindowPoints(HWND_DESKTOP, _window, reinterpret_cast<POINT*>(&sliderRect), 2);
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        return RECT{sliderRect.left + channel.left, sliderRect.bottom, sliderRect.left + channel.right,
                    sliderRect.bottom + Px(_metrics.Row)};
    }

    void DrawTickLabel(HDC dc, const RECT& track, int center, int top, unsigned gib, LONG& nextLeft) const
    {
        const auto label = LFmt(IDS_GibMarker, gib);
        const auto font =
            SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(_tree ? _tree : _step, WM_GETFONT, 0, 0)));
        SIZE size{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
        SelectObject(dc, font);
        const auto left = std::clamp(center - size.cx / 2, track.left, std::max(track.left, track.right - size.cx));
        if (left < nextLeft)
        {
            return;
        }
        Text(dc, label, {left, top, left + size.cx + 1, top + size.cy}, false, true, DT_LEFT | DT_SINGLELINE);
        nextLeft = left + size.cx + Px(_metrics.Gap);
    }

    void DrawStorageMarks(HDC dc) const
    {
        const auto marks = TickArea(SizeSlider);
        if (!marks)
        {
            return;
        }
        LONG nextLeft = marks->left;
        for (const auto mark : DiskMarks)
        {
            const auto position = DiskPosition(mark * MiBPerGiB);
            const int center = marks->left + MulDiv(marks->right - marks->left, position, DiskSliderLast);
            DrawTickLabel(dc, *marks, center, marks->top, mark, nextLeft);
        }
    }

    void DrawRamMarks(HDC dc) const
    {
        const auto marks = TickArea(RamSlider);
        if (!marks)
        {
            return;
        }

        LONG nextLeft = marks->left;
        _ramScale.ForEachVisibleTick(marks->right - marks->left, Px(2), [&](CapacityTick tick, int offset, bool major) {
            const int x = marks->left + offset;
            Fill(dc, {x, marks->top, x + (major ? Px(2) : 1), marks->top + Px(major ? _metrics.Gap : _metrics.Unit)},
                 major ? _colors.Accent : _colors.Border);
            if (major)
            {
                DrawTickLabel(dc, *marks, x, marks->top + Px(_metrics.Gap), tick.MiB / MiBPerGiB, nextLeft);
            }
        });
    }

    CapacityTick CapacityAt(int slider, int position) const
    {
        const bool ram = slider == RamSlider;
        const int last = ram ? RamSliderScale::Last : DiskSliderLast;
        CapacityTick selected{ram ? _ramScale.Memory(position) : DiskMiB(position), position};
        const auto area = TickArea(slider);
        if (!area || position == 0 || position == last || area->right <= area->left)
        {
            return selected;
        }

        const int width = area->right - area->left;
        constexpr int snapDistanceDip = 6;
        // Compare in scaled pixels to avoid rounding the distance to a whole pixel.
        auto closest = int64_t{Px(snapDistanceDip)} * last + 1;
        const auto consider = [&](CapacityTick tick) {
            const auto distance = int64_t{width} * std::abs(tick.Position - position);
            if (distance < closest)
            {
                closest = distance;
                selected = tick;
            }
        };
        if (ram)
        {
            _ramScale.ForEachVisibleTick(width, Px(2), [&](CapacityTick tick, int, bool) { consider(tick); });
        }
        else
        {
            consider({1, 0});
            for (const auto gib : DiskMarks)
            {
                consider({gib * MiBPerGiB, DiskPosition(gib * MiBPerGiB)});
            }
            consider({MaxDiskMiB, last});
        }
        return selected;
    }

    void SetCapacityPosition(HWND slider, int position)
    {
        _capacityPosition = position;
        _draggingCapacity = false;
        SendMessageW(slider, TBM_SETPOS, TRUE, position);
    }

    void MoveCapacitySlider(HWND slider, int id, int position, UINT action)
    {
        const bool released = action == TB_THUMBPOSITION;
        if (action == TB_ENDTRACK || (position == _capacityPosition && !(released && _draggingCapacity)))
        {
            return;
        }

        _capacityPosition = position;
        _draggingCapacity = action == TB_THUMBTRACK;
        const auto value = CapacityAt(id, position);
        Set(Find(id == RamSlider ? Ram : DiskSize), std::to_wstring(value.MiB));
        // Let repeated keyboard steps and continued dragging advance the native position.
        if (released && value.Position != position)
        {
            SetCapacityPosition(slider, value.Position);
        }
    }

    void ChangeDiskMode()
    {
        const Fs::path previous(Trim(Read(Find(DiskPath))));
        if (_diskMode == DiskMode::Existing)
        {
            _existingPath = previous;
        }
        if (_diskMode == DiskMode::Create)
        {
            _newDiskPath = previous == DefaultDisk() ? Fs::path{} : previous;
        }
        _diskMode = static_cast<DiskMode>(SendMessageW(Find(DiskModeId), CB_GETCURSEL, 0, 0));
        _draft.Primary.Attached = _diskMode != DiskMode::None;
        const auto path = _diskMode == DiskMode::Create     ? (_newDiskPath.empty() ? DefaultDisk() : _newDiskPath)
                          : _diskMode == DiskMode::Existing ? _existingPath
                                                            : Fs::path{};
        Set(Find(DiskPath), path.native());
        UpdateStorage();
        Refresh();
    }

    HWND ResolutionChoice(int id, int label, Resolution dimensions, std::wstring_view defaultChoice)
    {
        std::vector<std::wstring> choices{std::wstring(defaultChoice)};
        for (const auto preset : Presets)
        {
            choices.push_back(ResolutionText(preset));
        }
        choices.push_back(LText(IDS_Custom));
        const auto selected = dimensions == Resolution{}                                ? choices.front()
                              : std::ranges::find(Presets, dimensions) != Presets.end() ? ResolutionText(dimensions)
                                                                                        : choices.back();
        return Choice(id, label, choices, selected);
    }

    void AdapterChoices(std::wstring vga, std::wstring ati)
    {
        const auto& machine = MachineInfo();
        const auto defaultAdapter = machine.DefaultVga.empty() ? LText(IDS_DefaultsUnavailable)
                                                               : LFmt(IDS_DefaultValue, VgaName(machine.DefaultVga));
        std::vector values{defaultAdapter};
        for (const auto& adapter : machine.VgaTypes)
        {
            values.push_back(VgaName(adapter));
        }
        Options(Find(Vga), values, vga == L"none" ? LText(IDS_NoVideo) : vga);
        Options(Find(Ati), {LFmt(IDS_DefaultValue, DefaultAti(machine.Name)), L"rage128p", L"rv100", L"es1000"}, ati);
    }

    void Display()
    {
        const auto& machine = MachineInfo();
        Field(IDS_Vga, Combo(Vga));
        Field(IDS_Ati, Combo(Ati));
        AdapterChoices(_draft.Vga, _draft.AtiModel);

        ResolutionChoice(InitialMode, IDS_InitialResolution, {_draft.Width, _draft.Height},
                         LFmt(IDS_DefaultValue, ResolutionText({DefaultDisplayWidth, DefaultDisplayHeight})));
        Numeric(Width, IDS_Width, _draft.Width);
        Numeric(Height, IDS_Height, _draft.Height);
        ResolutionChoice(
            MaximumMode, IDS_MaxResolution, {_draft.MaxWidth, _draft.MaxHeight},
            LFmt(IDS_DefaultValue, ResolutionText({_draft.Width ? _draft.Width : DefaultDisplayWidth,
                                                   _draft.Height ? _draft.Height : DefaultDisplayHeight})));
        Numeric(MaxWidth, IDS_MaxWidth, _draft.MaxWidth);
        Numeric(MaxHeight, IDS_MaxHeight, _draft.MaxHeight);
        const auto value = Numeric(Vram, IDS_Vram, _draft.VramMiB);
        Slider(VramSlider);
        if (!_draft.VramMiB)
        {
            Set(value, std::to_wstring(_draft.Width || _draft.MaxWidth ? MinimumVram(_draft, machine)
                                                                       : DefaultVram(EffectiveVga(_draft, machine))));
        }
        UpdateVideo(false);
    }

    void BoolChoice(int id, int label, bool enabled)
    {
        const std::vector values{LFmt(IDS_DefaultValue, LText(IDS_Disabled)), LText(IDS_Enabled)};
        Choice(id, label, values, values[enabled]);
    }

    static const std::vector<std::wstring>& PortModes()
    {
        static const std::vector<std::wstring> modes{L"none", L"window", L"file", L"listen", L"connect"};
        return modes;
    }

    void PortFields(int id, const CharPort& port)
    {
        const std::vector names{LText(IDS_None), LText(IDS_ConsoleWindow), LText(IDS_FileOutput), LText(IDS_TcpListen),
                                LText(IDS_TcpConnect)};
        const auto found = std::ranges::find(PortModes(), port.Mode);
        if (found == PortModes().end())
        {
            Fail(LText(IDS_BadEndpoint));
        }
        DefaultHint(
            Choice(id, id == SerialMode ? IDS_Serial : IDS_DebugPort, names, names[found - PortModes().begin()]),
            names.front());
        Field(IDS_LogFile, Edit(id + 1, port.File.native()), Button(id + 2, LText(IDS_Browse)));
        DefaultHint(Field(IDS_Host, Edit(id + 3, port.Host)), CharPort{}.Host, false);
        Numeric(id + 4, IDS_Port, port.Port);
        if (id == DebugMode && !IsVpc(_draft.Machine))
        {
            Options(Find(id), {LText(IDS_NoMachineSupport)}, LText(IDS_NoMachineSupport));
            Enable(Find(id), false);
        }
        UpdatePort(id);
    }

    void UpdatePort(int id)
    {
        const auto selected = CurrentIndex(Find(id));
        ShowRow(Find(id + 1), selected == 2);
        ShowRow(Find(id + 3), selected >= 3);
        ShowRow(Find(id + 4), selected >= 3);
    }

    void NetworkFields()
    {
        Field(IDS_NetBackend, Combo(Backend));
        Field(IDS_NicModel, Combo(NicModel));
        DefaultHint(Field(IDS_Mac, Edit(Mac, _draft.Network.Mac)), L"52:54:00:12:34:56");
        const auto tap = Field(IDS_TapInterface,
                               Child(WC_COMBOBOXW, L"", Tap, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL));
        Options(tap, _capabilities.TapAdapters, _draft.Network.Interface);
        if (!_draft.Network.Interface.empty())
        {
            Set(tap, _draft.Network.Interface);
        }
        if (_capabilities.TapAdapters.empty())
        {
            SendMessageW(tap, CB_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(LText(IDS_NoTap).c_str()));
        }
        DefaultHint(Field(IDS_Host, Edit(NetHost, _draft.Network.Host)), Nic{}.Host, false);
        Numeric(NetPort, IDS_Port, _draft.Network.Port);
        DefaultHint(Field(IDS_PortForward, Edit(Forward, _draft.Network.Forward)), LText(IDS_None));
        BoolChoice(NetRom, IDS_BootRom, _draft.Network.BootRom);
        NetworkChoices();
    }

    void NetworkChoices()
    {
        _backends = {L"", L"none"};
        std::vector names{LFmt(IDS_DefaultValue, NetworkDefault(_draft.Machine, _capabilities)),
                          LText(IDS_NetDisconnected)};
        for (const auto& backend : _capabilities.NetBackends)
        {
            if (MachineInfo().NicModels.empty())
            {
                break;
            }
            if (backend == L"socket")
            {
                _backends.insert(_backends.end(), {L"listen", L"connect"});
                names.insert(names.end(), {LText(IDS_TcpListen), LText(IDS_TcpConnect)});
            }
            else
            {
                _backends.push_back(backend);
                names.push_back(backend == L"user" ? L"NAT (libslirp)" : L"TAP");
            }
        }
        const auto idx = std::ranges::find(_backends, _draft.Network.Backend);
        Options(Find(Backend), names, idx == _backends.end() ? names.front() : names[idx - _backends.begin()]);
        Options(Find(NicModel), MachineInfo().NicModels,
                _draft.Network.Model.empty() ? DefaultNicModel(_draft.Machine) : _draft.Network.Model);
        UpdateNetwork();
    }

    void UpdateNetwork()
    {
        const auto& b = _backends.at(CurrentIndex(Find(Backend)));
        const bool active = !b.empty() && b != L"none";
        ShowRow(Find(NicModel), active);
        ShowRow(Find(Mac), active);
        ShowRow(Find(NetRom), active && IsVpc(_draft.Machine));
        ShowRow(Find(Forward), b == L"user");
        ShowRow(Find(Tap), b == L"tap");
        ShowRow(Find(NetHost), b == L"listen" || b == L"connect");
        ShowRow(Find(NetPort), b == L"listen" || b == L"connect");
    }

    void StorageConnections()
    {
        const auto selection = _storageSelection.empty() ? L"onboard" : _storageSelection;
        if (_activeCommand)
        {
            EditStorage(_window, _appearance, _draft, _engine, _selected, _capabilities, {}, selection, true);
            return;
        }
        if (_editing)
        {
            for (const auto page : {Page::Disk, Page::Optical, Page::Controller})
            {
                if (_changed.contains(page))
                {
                    Capture(page, true);
                }
            }
        }
        else
        {
            Capture(Page::Storage, true);
        }
        const auto pending = _diskMode == DiskMode::Create ? _draft.Primary.File : Fs::path{};
        if (!EditStorage(_window, _appearance, _draft, _engine, MachineInfo(), _capabilities, pending, selection))
        {
            return;
        }
        _diskMode = _draft.Primary.File.empty()                  ? DiskMode::None
                    : SameFilePath(pending, _draft.Primary.File) ? DiskMode::Create
                                                                 : DiskMode::Existing;
        _existingPath = _draft.Primary.File;
        if (auto mode = Find(DiskModeId))
        {
            SendMessageW(mode, CB_SETCURSEL, static_cast<WPARAM>(_diskMode), 0);
            Set(Find(DiskPath), _draft.Primary.File.native());
            UpdateStorage();
        }
        if (auto path = Find(Iso))
        {
            Set(path, _draft.Optical.File.native());
        }
        SetBus(_draft.Bus);
        ConnectionSummary();
        Refresh();
    }

    void Advanced()
    {
        const auto nvram =
            Field(IDS_NvramFile, Edit(NvramFile, _draft.Nvram.native()), Button(BrowseNvram, LText(IDS_Browse)));
        const auto defaultNvram = DataDirectory() / L"VMs" / _draft.Id / L"nvram";
        DefaultHint(nvram, defaultNvram.native());
        ShowRow(nvram, MachineInfo().Properties.contains(L"nvram"));
        const std::vector startup{LFmt(IDS_DefaultValue, LText(IDS_ChooseSnapshot)), LText(IDS_ColdBoot)};
        Choice(SnapshotStartup, IDS_SnapshotStartup, startup, startup[_draft.SkipSnapshotPrompt ? 1 : 0]);
        std::vector choices{LFmt(IDS_DefaultValue, L"TCG")};
        if (_capabilities.Llvm || _draft.Llvm)
        {
            choices.push_back(L"TCG-LLVM");
        }
        Choice(Llvm, IDS_Accel, choices, _draft.Llvm ? L"TCG-LLVM" : choices.front());
        DefaultHint(Numeric(Cache, IDS_Cache, _draft.CacheMiB), std::to_wstring(Profile{}.CacheMiB), false);
        const auto threads = Numeric(LlvmThreads, IDS_LlvmThreads, _draft.LlvmThreads);
        if (_capabilities.LlvmThreads)
        {
            DefaultHint(threads, std::to_wstring(*_capabilities.LlvmThreads));
        }
        ShowRow(threads, _draft.Llvm && _capabilities.LlvmThreads);
        BoolChoice(NoReboot, IDS_NoReboot, _draft.NoReboot);
        if (!_editing)
        {
            MachineOptions();
        }
        if (!_editing)
        {
            for (size_t index = 0; index < DeviceOptions.size(); ++index)
            {
                if (MachineInfo().Properties.contains(DeviceOptions[index].first))
                {
                    OptionField(index, DeviceOptions[index].second);
                }
            }
        }
        ShowRow(Find(Cache), _draft.Llvm);
    }

    void MachineOptions()
    {
        Field(IDS_Alat, Combo(Alat));
        Field(IDS_FirmwareBase, Edit(FirmwareBase, _draft.FirmwareBase));
        UpdateMachineDefaults(_draft.Alat);
        if (_editing)
        {
            OptionField(4, IDS_Usb1);
            ShowRow(Find(OptionStart + 4), MachineInfo().Properties.contains(L"usb1"));
        }
    }

    void UpdateMachineDefaults(std::wstring_view alat)
    {
        const auto& machine = MachineInfo();
        if (auto field = Find(Alat))
        {
            ShowRow(field, !machine.DefaultAlat.empty());
            if (!machine.DefaultAlat.empty())
            {
                Options(field, {LFmt(IDS_DefaultValue, machine.DefaultAlat), L"zero", L"full"}, alat);
            }
            else
            {
                SendMessageW(field, CB_RESETCONTENT, 0, 0);
            }
        }
        if (auto field = Find(FirmwareBase))
        {
            ShowRow(field, !machine.DefaultFirmwareBase.empty());
            if (!machine.DefaultFirmwareBase.empty())
            {
                DefaultHint(field, machine.DefaultFirmwareBase);
            }
            else
            {
                Set(field, L"");
            }
        }
    }

    void OptionField(size_t index, int label)
    {
        Field(label, Combo(OptionStart + static_cast<int>(index)));
        UpdateOption(index);
    }

    void UpdateOption(size_t index)
    {
        const auto key = DeviceOptions[index].first;
        const auto control = Find(OptionStart + static_cast<int>(index));
        if (!control || !_selected.Properties.contains(key))
        {
            return;
        }
        const auto found = _draft.MachineOptions.find(key);
        const auto defaultLabel =
            LFmt(IDS_DefaultValue, LText(_selected.BooleanDefaults.at(key) ? IDS_Enabled : IDS_Disabled));
        Options(control, {defaultLabel, LText(IDS_Enabled), LText(IDS_Disabled)},
                found == _draft.MachineOptions.end() ? defaultLabel
                                                     : LText(found->second ? IDS_Enabled : IDS_Disabled));
    }

    void Review()
    {
        const auto list = Field(0, PropertyList(ReviewList));
        ShowProfileDetails(list, _draft, &_capabilities, true,
                           _diskMode == DiskMode::Create ? std::optional(_size) : std::nullopt);
    }

    Machine ReadMachine(const std::wstring& name)
    {
        const auto base = _capabilities.Find(name);
        if (!base)
        {
            Fail(LFmt(IDS_MachineUnavailable, name));
        }
        Machine machine;
        Busy(_window, _appearance, LText(IDS_HardwareLoading), [&] { machine = ProbeMachine(_engine, *base); });
        return machine;
    }

    const Machine& MachineInfo()
    {
        if (_selected.Name != _draft.Machine)
        {
            _selected = ReadMachine(_draft.Machine);
        }
        return _selected;
    }

    void SetBus(std::wstring bus)
    {
        _draft.Bus = std::move(bus);
        if (const auto control = Find(Bus))
        {
            const auto buses = Buses(_draft.Machine);
            Options(control, buses, _draft.Bus);
            DefaultHint(control, buses.front());
        }
        if (_tree)
        {
            StorageTree();
        }
    }

    static std::wstring OverrideChoice(HWND control)
    {
        return CurrentIndex(control) > 0 ? Selection(control) : L"";
    }

    void SetMachine(const std::wstring& name)
    {
        if (name == _draft.Machine || name == LText(IDS_KeepMachine))
        {
            return;
        }
        if (_editing)
        {
            Machine selected;
            try
            {
                selected = ReadMachine(name);
            }
            catch (...)
            {
                MachineChoices();
                throw;
            }
            _draft.Machine = name;
            _selected = std::move(selected);
            if (!_selected.Properties.contains(L"nvram"))
            {
                _draft.Nvram.clear();
                if (const auto path = Find(NvramFile))
                {
                    Set(path, L"");
                }
            }
            if (const auto path = Find(NvramFile))
            {
                ShowRow(path, _selected.Properties.contains(L"nvram"));
            }
            if (!IsVpc(name))
            {
                _draft.DebugPort.Mode = L"none";
            }
            if (Find(DebugMode))
            {
                const std::vector labels{LText(IDS_None), LText(IDS_ConsoleWindow), LText(IDS_FileOutput),
                                         LText(IDS_TcpListen), LText(IDS_TcpConnect)};
                Enable(Find(DebugMode), IsVpc(name));
                if (!IsVpc(name))
                {
                    Options(Find(DebugMode), {LText(IDS_NoMachineSupport)}, LText(IDS_NoMachineSupport));
                }
                else
                {
                    Options(Find(DebugMode), labels,
                            labels.at(std::ranges::find(PortModes(), _draft.DebugPort.Mode) - PortModes().begin()));
                }
                UpdatePort(DebugMode);
            }
            if (!_selected.Properties.contains(L"alat"))
            {
                _draft.Alat.clear();
            }
            if (!_selected.Properties.contains(L"firmware-base"))
            {
                _draft.FirmwareBase.clear();
            }
            if (auto a = Find(Alat))
            {
                UpdateMachineDefaults(OverrideChoice(a));
            }
            if (auto a = Find(OptionStart + 4))
            {
                ShowRow(a, _selected.Properties.contains(L"usb1"));
            }
            if (Find(Backend))
            {
                NetworkChoices();
            }
            MachineChoices();
            const auto buses = Buses(name);
            SetBus(std::ranges::find(buses, _draft.Bus) != buses.end() ? _draft.Bus : buses.front());
            if (Find(Cpu))
            {
                CpuChoices(OverrideChoice(Find(Cpu)));
                Set(_cpuLimit, LFmt(IDS_ProcessorLimit, _selected.MaxCpus));
            }
            if (Find(Ram))
            {
                unsigned memory;
                try
                {
                    memory = Number(Find(Ram), LText(IDS_Ram), UINT_MAX);
                }
                catch (const std::exception&)
                {
                    memory = _draft.RamMiB;
                }
                MemoryRange(memory);
            }
            if (std::ranges::find(_selected.VgaTypes, _draft.Vga) == _selected.VgaTypes.end())
            {
                _draft.Vga.clear();
            }
            const auto adapter = std::wstring(EffectiveVga(_draft, _selected));
            const auto properties = _capabilities.VideoProperties.find(adapter);
            const auto supports = [&](const wchar_t* property) {
                return properties != _capabilities.VideoProperties.end() && properties->second.contains(property);
            };
            if (!supports(L"xres") || !supports(L"yres"))
            {
                _draft.Width = _draft.Height = 0;
            }
            if (!supports(L"xmax") || !supports(L"ymax"))
            {
                _draft.MaxWidth = _draft.MaxHeight = 0;
            }
            if (!supports(L"vgamem_mb"))
            {
                _draft.VramMiB = 0;
            }
            if (!supports(L"model") || adapter != L"ati")
            {
                _draft.AtiModel.clear();
            }
            if (Find(Vga))
            {
                AdapterChoices(OverrideChoice(Find(Vga)), OverrideChoice(Find(Ati)));
                UpdateVideo(false);
            }
            std::erase_if(_draft.MachineOptions,
                          [&](const auto& option) { return !_selected.Properties.contains(option.first); });
            UpdateTree();
            for (size_t index = 0; index < DeviceOptions.size(); ++index)
            {
                UpdateOption(index);
            }
            Refresh();
            return;
        }
        _draft.Machine = name;
        _draft.Cpu.clear();
        _draft.Sockets = _draft.Cores = _draft.Threads = 1;
        _draft.RamMiB = DefaultRam(name);
        _draft.Bus = Buses(name).front();
        _draft.Vga.clear();
        _draft.AtiModel.clear();
        _draft.Width = _draft.Height = _draft.MaxWidth = _draft.MaxHeight = _draft.VramMiB = 0;
        _draft.MachineOptions.clear();
    }

    void InspectMedia()
    {
        const Fs::path current(Trim(Read(Find(Iso))));
        if (_inspectedIso == current)
        {
            return;
        }
        MediaRecommendation result;
        if (!current.empty())
        {
            Busy(_window, _appearance, LText(IDS_Inspect), [&] { result = InspectIso(current); });
        }
        else
        {
            result = InspectIso(current);
        }
        _inspectedIso = current;
        _appliedRecommendation = false;
        _recommendation = std::move(result);
        ShowRecommendation();
    }

    bool HasRecommendation() const
    {
        return !IsTestModel(_recommendation.Machine) && _capabilities.Find(_recommendation.Machine);
    }

    void ApplyMediaDefaults()
    {
        InspectMedia();
        _draft.Optical.UpdateImage(*_inspectedIso);
        if (!_appliedRecommendation && HasRecommendation())
        {
            SetMachine(_recommendation.Machine);
            SetBus(_recommendation.Ahci ? L"AHCI" : Buses(_draft.Machine).front());
            _appliedRecommendation = true;
        }
    }

    void ShowRecommendation()
    {
        const Fs::path path = Trim(Read(Find(Iso)));
        const auto inspected = !path.empty() && _inspectedIso == path;
        const auto available = inspected && HasRecommendation();
        Set(_hint, available   ? LFmt(IDS_IsoHint, _recommendation.Description, _recommendation.Machine)
                   : inspected ? _recommendation.Description
                               : L"");
        ShowRow(_hint, inspected);
        if (!_building)
        {
            Refresh();
        }
    }

    void UpdateStorage()
    {
        const bool hasDisk = _diskMode != DiskMode::None;
        ShowRow(Find(DiskPath), hasDisk);
        ShowRow(Find(DiskSize), _diskMode == DiskMode::Create);
        ShowRow(Find(SizeSlider), _diskMode == DiskMode::Create);
    }

    static int CurrentIndex(HWND control)
    {
        return static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
    }

    Profile VideoSettings(bool strict)
    {
        auto settings = _draft;
        settings.Vga = OverrideChoice(Find(Vga));
        if (settings.Vga == LText(IDS_NoVideo))
        {
            settings.Vga = L"none";
        }
        settings.AtiModel = CurrentIndex(Find(Ati)) > 0 && IsWindowEnabled(Find(Ati)) ? Selection(Find(Ati)) : L"";
        const auto read = [&](int mode, int width, int height, int widthLabel, int heightLabel) {
            if (!IsWindowEnabled(Find(mode)))
            {
                return Resolution{};
            }
            const auto index = CurrentIndex(Find(mode));
            if (index <= 0)
            {
                return Resolution{};
            }
            if (index <= static_cast<int>(Presets.size()))
            {
                return Presets[static_cast<size_t>(index - 1)];
            }
            try
            {
                return Resolution{Number(Find(width), LText(widthLabel), MaxDisplayWidth),
                                  Number(Find(height), LText(heightLabel), MaxDisplayHeight)};
            }
            catch (const std::exception&)
            {
                if (strict)
                {
                    throw;
                }
                return Resolution{};
            }
        };
        const auto initial = read(InitialMode, Width, Height, IDS_Width, IDS_Height);
        const auto maximum = read(MaximumMode, MaxWidth, MaxHeight, IDS_MaxWidth, IDS_MaxHeight);
        settings.Width = initial.Width;
        settings.Height = initial.Height;
        settings.MaxWidth = maximum.Width;
        settings.MaxHeight = maximum.Height;
        return settings;
    }

    void UpdateVideo(bool changed)
    {
        const auto& machine = MachineInfo();
        const auto selected = CurrentIndex(Find(Vga)) > 0 ? Selection(Find(Vga)) : machine.DefaultVga;
        const auto found = _capabilities.VideoProperties.find(selected);
        const auto supports = [&](std::wstring_view key) {
            return found != _capabilities.VideoProperties.end() && found->second.contains(std::wstring(key));
        };
        const bool initial = supports(L"xres") && supports(L"yres");
        const bool maximum = supports(L"xmax") && supports(L"ymax");
        const bool initialCustom = initial && CurrentIndex(Find(InitialMode)) > static_cast<int>(Presets.size());
        const bool maximumCustom = maximum && CurrentIndex(Find(MaximumMode)) > static_cast<int>(Presets.size());
        Enable(Find(Ati), selected == L"ati" && supports(L"model"));
        Enable(Find(InitialMode), initial);
        Enable(Find(MaximumMode), maximum);
        const auto fields = std::array{std::pair{Width, initialCustom}, std::pair{Height, initialCustom},
                                       std::pair{MaxWidth, maximumCustom}, std::pair{MaxHeight, maximumCustom}};
        for (const auto& [id, visible] : fields)
        {
            ShowRow(Find(id), visible);
        }

        _showVram = supports(L"vgamem_mb");
        ShowRow(Find(Vram), _showVram);
        ShowRow(Find(VramSlider), _showVram);
        ShowRow(Find(Ati), selected == L"ati" && supports(L"model"));
        ShowRow(Find(InitialMode), initial);
        ShowRow(Find(MaximumMode), maximum);
        const auto profile = VideoSettings(false);
        const auto maximumChoice = Find(MaximumMode);
        const auto oldIndex = CurrentIndex(maximumChoice);
        const auto defaultMaximum =
            LFmt(IDS_DefaultValue, ResolutionText({profile.Width ? profile.Width : DefaultDisplayWidth,
                                                   profile.Height ? profile.Height : DefaultDisplayHeight}));
        SendMessageW(maximumChoice, CB_DELETESTRING, 0, 0);
        SendMessageW(maximumChoice, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(defaultMaximum.c_str()));
        SendMessageW(maximumChoice, CB_SETCURSEL, oldIndex, 0);
        if (_showVram)
        {
            _vramMaximum = MaximumVram(selected);
            _vramMinimum = MinimumVram(profile, machine);
            if (_vramMinimum > _vramMaximum)
            {
                _vramMinimum = _vramMaximum;
            }
            const auto automatic = std::clamp(DefaultVram(selected), _vramMinimum, _vramMaximum);
            DefaultHint(Find(Vram), std::to_wstring(profile.Width || profile.MaxWidth ? _vramMinimum : automatic),
                        false);
            unsigned value = automatic;
            if (changed && (profile.Width || profile.MaxWidth))
            {
                value = _vramMinimum;
            }
            else if (!changed)
            {
                try
                {
                    value = Number(Find(Vram), LText(IDS_Vram), _vramMaximum);
                }
                catch (const std::exception&)
                {
                }
                value = std::clamp(value, _vramMinimum, _vramMaximum);
            }
            if (changed || Read(Find(Vram)).empty())
            {
                Set(Find(Vram), std::to_wstring(value));
            }
            SetSlider(Find(VramSlider), std::bit_width(_vramMinimum) - 1, std::bit_width(_vramMaximum) - 1,
                      std::bit_width(value) - 1);
        }
        if (!_building)
        {
            Refresh();
        }
    }

    void Capture(Page page, bool validate)
    {
        const auto number = [&](int control, int label, unsigned maximum, bool optional = false) {
            try
            {
                return Number(Find(control), LText(label), maximum, optional);
            }
            catch (const std::exception&)
            {
                if (validate)
                {
                    _invalidField = control;
                    throw;
                }
                return 0u;
            }
        };
        if (page == Page::Identity || page == Page::System)
        {
            _draft.Name = Trim(Read(Find(Name)));
            if (validate && _draft.Name.empty())
            {
                _invalidField = Name;
                Fail(LText(IDS_NameRequired));
            }
        }
        if (page == Page::Identity && validate)
        {
            ApplyMediaDefaults();
        }
        if (page == Page::Optical || (page == Page::Identity && !validate))
        {
            _draft.Optical.UpdateImage(Trim(Read(Find(Iso))));
        }
        if (page == Page::Hardware || page == Page::Processor)
        {
            _draft.Cpu = OverrideChoice(Find(Cpu));
            _draft.Sockets = number(Sockets, IDS_Sockets, MachineInfo().MaxCpus);
            _draft.Cores = number(Cores, IDS_Cores, MachineInfo().MaxCpus);
            _draft.Threads = number(Threads, IDS_Threads, MachineInfo().MaxCpus);
            if (validate &&
                static_cast<uint64_t>(_draft.Sockets) * _draft.Cores * _draft.Threads > MachineInfo().MaxCpus)
            {
                Fail(LFmt(IDS_CpuLimit, MachineInfo().MaxCpus));
            }
        }
        if (page == Page::Hardware || page == Page::Memory)
        {
            _draft.RamMiB = number(Ram, IDS_Ram, _ramScale.Maximum);
            if (validate && _draft.RamMiB < _ramScale.Minimum)
            {
                Fail(LFmt(IDS_RamRange, _ramScale.Minimum, _ramScale.Maximum));
            }
        }
        if (page == Page::Storage || page == Page::Controller)
        {
            _draft.Bus = Selection(Find(Bus));
        }
        if (page == Page::Storage || page == Page::Disk)
        {
            _draft.Primary.UpdateImage(_diskMode == DiskMode::None ? Fs::path{} : Fs::path(Trim(Read(Find(DiskPath)))),
                                       _diskMode == DiskMode::Create);
            if (_diskMode == DiskMode::Create)
            {
                _newDiskPath = _draft.Primary.File == DefaultDisk() ? Fs::path{} : _draft.Primary.File;
                _size = number(DiskSize, IDS_DiskSize, MaxDiskMiB);
            }
            else if (_diskMode == DiskMode::Existing)
            {
                _existingPath = _draft.Primary.File;
            }
            if (validate && _diskMode != DiskMode::None && _draft.Primary.File.empty())
            {
                Fail(LText(IDS_DiskRequired));
            }
        }
        if (page == Page::Display)
        {
            auto video = VideoSettings(validate);
            const auto& machine = MachineInfo();
            const auto required = MinimumVram(video, machine);
            if (validate && _showVram && required > _vramMaximum)
            {
                Fail(LFmt(IDS_ExceededVram, required, _vramMaximum));
            }
            if (_showVram)
            {
                video.VramMiB = number(Vram, IDS_Vram, _vramMaximum);
            }
            else
            {
                video.VramMiB = 0;
            }
            if (validate && _showVram &&
                (!std::has_single_bit(video.VramMiB) || video.VramMiB < required || video.VramMiB > _vramMaximum))
            {
                Fail(LFmt(IDS_VramRange, required, _vramMaximum));
            }
            _draft = std::move(video);
        }
        if (page == Page::Input)
        {
            _draft.ForcePs2 = CurrentIndex(Find(ForcePs2)) == 1;
            _draft.UsbMouse = CurrentIndex(Find(UsbMouse)) == 1;
        }
        if (page == Page::Network)
        {
            auto& n = _draft.Network;
            n.Backend = _backends.at(CurrentIndex(Find(Backend)));
            n.Model = Selection(Find(NicModel));
            n.Mac = Trim(Read(Find(Mac)));
            n.Host = Trim(Read(Find(NetHost)));
            n.Interface = Trim(Read(Find(Tap)));
            if (n.Backend == L"listen" || n.Backend == L"connect")
            {
                n.Port = number(NetPort, IDS_Port, 65535);
            }
            n.Forward = Trim(Read(Find(Forward)));
            n.BootRom = CurrentIndex(Find(NetRom)) == 1;
        }
        if (page == Page::Ports)
        {
            for (const auto& [id, port] :
                 {std::pair{SerialMode, &_draft.Serial}, std::pair{DebugMode, &_draft.DebugPort}})
            {
                port->Mode = PortModes().at(CurrentIndex(Find(id)));
                port->File = Trim(Read(Find(id + 1)));
                port->Host = Trim(Read(Find(id + 3)));
                if (port->Mode == L"listen" || port->Mode == L"connect")
                {
                    port->Port = number(id + 4, IDS_Port, 65535);
                }
            }
        }
        if (page == Page::MachineOptions || (!_editing && page == Page::Advanced))
        {
            _draft.Alat = MachineInfo().Properties.contains(L"alat") ? OverrideChoice(Find(Alat)) : L"";
            _draft.FirmwareBase =
                MachineInfo().Properties.contains(L"firmware-base") ? Trim(Read(Find(FirmwareBase))) : L"";
        }
        if (page == Page::Advanced)
        {
            _draft.NoReboot = CurrentIndex(Find(NoReboot)) == 1;
            _draft.Nvram = MachineInfo().Properties.contains(L"nvram") ? Trim(Read(Find(NvramFile))) : L"";
            _draft.SkipSnapshotPrompt = CurrentIndex(Find(SnapshotStartup)) == 1;
            _draft.Llvm = Selection(Find(Llvm)) == L"TCG-LLVM";
            if (_draft.Llvm)
            {
                _draft.CacheMiB = number(Cache, IDS_Cache, 65536);
                if (_capabilities.LlvmThreads)
                {
                    _draft.LlvmThreads = number(LlvmThreads, IDS_LlvmThreads, UINT_MAX, true);
                }
            }
        }
    }

    void ShowInvalid(Page page, int field = 0)
    {
        if (!_editing)
        {
            return;
        }
        TreeView_SelectItem(_tree, _nodes.at(page));
        const auto target = field ? Find(field) : Rows().front().Value;
        int offset = 0;
        for (const auto& row : Rows())
        {
            if (row.Value == target)
            {
                _scroll = std::min(offset, std::max(_scroll, offset + row.Height - Viewport()));
                Refresh();
                SetFocus(target);
                break;
            }
            offset += row.Height;
        }
    }

    void CaptureEdits(bool focusInvalid)
    {
        for (const auto& entry : SettingsPages)
        {
            if (_changed.contains(entry.Id))
            {
                _invalidField = 0;
                try
                {
                    Capture(entry.Id, true);
                }
                catch (...)
                {
                    if (focusInvalid)
                    {
                        ShowInvalid(entry.Id, _invalidField);
                    }
                    throw;
                }
            }
        }
    }

    void UpdateCommandLine()
    {
        const auto text = Find(LaunchText), copy = Find(CopyLaunch);
        if (_activeCommand)
        {
            Set(text, *_activeCommand);
            return;
        }
        try
        {
            CaptureEdits(false);
            const auto& machine = MachineInfo();
            if (_diskMode == DiskMode::Create && Fs::exists(_draft.Primary.File))
            {
                Fail(LText(IDS_DiskExists));
            }
            auto profile = _draft;
            Busy(_window, _appearance, LText(IDS_DiskRead),
                 [&] { InspectStorage(profile, _engine, _diskMode == DiskMode::Create); });
            ValidateProfile(profile, machine, _capabilities, _engine);
            Set(text, _preview(profile, machine));
            Enable(copy, true);
        }
        catch (const std::exception& error)
        {
            Set(text, LFmt(IDS_CommandUnavailable, Wide(error.what())));
            Enable(copy, false);
        }
    }

    void Finish()
    {
        if (_editing)
        {
            CaptureEdits(true);
        }
        if (!_capabilities.Find(_draft.Machine))
        {
            ShowInvalid(Page::System, MachineId);
            Fail(LFmt(IDS_MachineUnavailable, _draft.Machine));
        }
        const auto& machine = MachineInfo();
        try
        {
            Busy(_window, _appearance, LText(IDS_DiskRead),
                 [&] { InspectStorage(_draft, _engine, _diskMode == DiskMode::Create); });
            ValidateProfile(_draft, machine, _capabilities, _engine);
        }
        catch (const ProfileValidationError& error)
        {
            switch (error.Section)
            {
            case ProfileSection::Network:
                ShowInvalid(Page::Network);
                break;
            case ProfileSection::Ports:
                ShowInvalid(Page::Ports);
                break;
            case ProfileSection::General:
                ShowInvalid(Page::System, Name);
                break;
            case ProfileSection::Processor:
                ShowInvalid(Page::Processor);
                break;
            case ProfileSection::Memory:
                ShowInvalid(Page::Memory, Ram);
                break;
            case ProfileSection::Controller:
                ShowInvalid(Page::Controller, Bus);
                break;
            case ProfileSection::Optical:
                ShowInvalid(Page::Optical, Iso);
                break;
            case ProfileSection::Display:
                ShowInvalid(Page::Display);
                break;
            case ProfileSection::Advanced:
                ShowInvalid(Page::Advanced);
                break;
            }
            throw;
        }
        if (_diskMode == DiskMode::Create && Fs::exists(_draft.Primary.File))
        {
            ShowInvalid(Page::Disk, DiskPath);
            Fail(LText(IDS_DiskExists));
        }
        const bool newDisk = _diskMode == DiskMode::Create;
        Busy(_window, _appearance, LText(IDS_ValidateConfig), [&] {
            auto test = _draft;
            if (newDisk)
            {
                test.Primary.Attached = false;
            }
            ValidateBoot(_engine, test, machine);
        });
        if (newDisk)
        {
            try
            {
                Busy(_window, _appearance, LText(IDS_Creating),
                     [&] { CreateDisk(_engine, _draft.Primary.File, _size); });
            }
            catch (...)
            {
                ShowInvalid(Page::Disk, DiskPath);
                throw;
            }
        }
        Result = ProfileResult{_draft, newDisk};
        DestroyWindow(_window);
    }

    void PickFile(int id)
    {
        Fs::path path;
        if (id == BrowseNvram)
        {
            const auto current = Fs::path(Trim(Read(Find(NvramFile))));
            path = ChooseFile(_window, LText(IDS_SelectNvram), LText(IDS_NvramFile), L"*.nvram;*.bin;nvram",
                              FileChoice::ExistingOrNew, current.empty() ? NvramPath(_draft) : current);
            if (!path.empty())
            {
                Set(Find(NvramFile), path.native());
            }
        }
        else if (id == Browse)
        {
            path = ChooseFile(_window, LText(IDS_SelectIso), LText(IDS_FileIso), L"*.iso");
            if (!path.empty())
            {
                _draft.Optical.SelectImage(path);
                Set(Find(Iso), path.native());
                InspectMedia();
            }
        }
        else
        {
            const bool creating = _diskMode == DiskMode::Create;
            path = ChooseFile(_window, LText(creating ? IDS_NewDisk : IDS_SelectDisk), LText(IDS_DiskFile),
                              L"*.qcow2;*.raw;*.img;*.iso;*.bin", creating ? FileChoice::CreateDisk : FileChoice::Open,
                              Read(Find(DiskPath)));
            if (!path.empty())
            {
                if (!creating)
                {
                    _draft.Primary.SelectImage(path);
                }
                Set(Find(DiskPath), path.native());
            }
        }
    }

    int BodyTop() const
    {
        return _metrics.Header() + _metrics.Gap;
    }
    int FooterSpace() const
    {
        return _metrics.Footer() + _metrics.Gap;
    }
    int Viewport() const
    {
        return std::max(1, _height - FooterSpace() - BodyTop());
    }

    int TreeWidth(int width) const
    {
        if (!_editing)
        {
            return 0;
        }
        int preferred = 0;
        for (const auto& entry : SettingsPages)
        {
            if (!entry.Option || _selected.Properties.contains(DeviceOptions[*entry.Option].first))
            {
                const auto text =
                    entry.Id == Page::Controller ? LFmt(IDS_ControllerName, _draft.Bus) : LText(entry.Title);
                const int indent = (entry.Parent ? 2 : 1) * _metrics.Line;
                preferred = std::max(preferred, TextWidth(text) + indent + _metrics.Margin);
            }
        }
        return std::min(width / 3, preferred + _metrics.Gap);
    }

    void Layout(int width, int height) override
    {
        _height = height;
        const int gap = _metrics.Gap, margin = _metrics.Margin, control = _metrics.Control;
        const int bottom = height - FooterSpace();
        const auto review = Find(ReviewList);
        const bool commandPage = _page == Page::CommandLine;
        int formLeft{}, formRight{}, labelWidth{};
        bool stacked{};
        // A vertical scrollbar changes the available width for wrapping.
        for (int pass = 0; pass < 2; ++pass)
        {
            _width = width;
            _side = TreeWidth(width);
            formLeft = _side + margin;
            formRight = width - margin;
            labelWidth = 0;
            for (const auto& row : Rows())
            {
                if (row.Shown && row.Caption)
                {
                    labelWidth = std::max(labelWidth, TextWidth(Read(row.Caption)) + gap);
                }
            }
            if (!review && !commandPage)
            {
                formRight = std::min(formRight, formLeft + labelWidth + _metrics.PathWidth() + control * 2 + gap);
            }
            stacked = formRight - formLeft < labelWidth + _metrics.Character * 24;
            if (stacked)
            {
                labelWidth = 0;
            }
            int extent = 0;
            for (auto& row : Rows())
            {
                row.Height = review || commandPage ? Viewport() : 0;
                if (!row.Shown || review || commandPage)
                {
                    continue;
                }
                int content = control;
                if (row.Kind == RowKind::Note)
                {
                    content = WrappedHeight(row.Value, formRight - formLeft - labelWidth);
                }
                if (row.Kind == RowKind::Scale)
                {
                    content += _metrics.Line + gap;
                }
                row.Height = content + gap + (stacked && row.Caption ? _metrics.Line : 0);
                extent += row.Height;
            }
            _scroll = std::clamp(_scroll, 0, std::max(0, extent - Viewport()));
            SCROLLINFO info{
                sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, extent, static_cast<UINT>(Viewport() + 1), _scroll, 0};
            SetScrollInfo(_window, SB_VERT, &info, TRUE);
            RECT client{};
            GetClientRect(_window, &client);
            width = Dip(client.right);
            if (width == _width)
            {
                break;
            }
        }
        if (_tree)
        {
            TreeView_SetItemHeight(_tree, Px(control));
            TreeView_SetIndent(_tree, Px(_metrics.Line));
            Place(_tree, _metrics.Unit, gap, _side - gap, height - _metrics.Footer() - gap * 2);
        }
        const int stepWidth = _step ? TextWidth(Read(_step)) + gap : 0;
        const int copyWidth = commandPage ? ButtonWidth(Find(CopyLaunch)) : 0;
        Place(_title, formLeft, _metrics.Unit, formRight - formLeft - stepWidth - (commandPage ? copyWidth + gap : 0),
              _metrics.Header() - gap);
        if (_step)
        {
            Place(_step, formRight - stepWidth, _metrics.Unit, stepWidth, _metrics.Header() - gap);
        }
        PlaceFooter(width, height, {_cancel, _next, _back});
        if (commandPage)
        {
            const auto text = Find(LaunchText), copy = Find(CopyLaunch);
            Place(copy, formRight - copyWidth, (_metrics.Header() - control) / 2, copyWidth, control);
            Place(text, formLeft, BodyTop(), formRight - formLeft, Viewport());
            ShowWindow(text, SW_SHOWNA);
            ShowWindow(copy, SW_SHOWNA);
            return;
        }
        if (review)
        {
            PlacePropertyList(review, formLeft, BodyTop(), formRight - formLeft, Viewport());
            return;
        }
        int y = BodyTop() - _scroll;
        for (const auto& row : Rows())
        {
            const int labelAbove = stacked && row.Caption ? _metrics.Line : 0;
            const bool visible =
                row.Shown && y >= BodyTop() && y + labelAbove + std::min(control, row.Height - gap) <= bottom;
            for (auto item : {row.Caption, row.Value, row.Action})
            {
                if (item)
                {
                    ShowWindow(item, visible ? SW_SHOWNA : SW_HIDE);
                }
            }
            if (visible)
            {
                const int left = formLeft + labelWidth;
                if (row.Caption)
                {
                    Place(row.Caption, formLeft, y, stacked ? formRight - formLeft : labelWidth - gap,
                          stacked ? labelAbove : control);
                }
                const int actionWidth = row.Action ? ButtonWidth(row.Action) + gap : 0;
                const int room = formRight - left - actionWidth;
                int limit = _metrics.PathWidth();
                wchar_t type[32]{};
                GetClassNameW(row.Value, type, static_cast<int>(std::size(type)));
                const bool combo = _wcsicmp(type, WC_COMBOBOXW) == 0;
                if (row.Kind == RowKind::Note || combo)
                {
                    limit = room;
                }
                else if (!row.Action && (GetDlgCtrlID(row.Value) == VramSlider || _wcsicmp(type, WC_EDITW) == 0))
                {
                    limit = _metrics.Character * 30;
                }
                const int available = InputWidth(row.Value, std::min(room, limit));
                const int fieldY = y + labelAbove;
                const int fieldHeight =
                    row.Kind == RowKind::Note ? std::min(row.Height - gap, bottom - fieldY) : control;
                Place(row.Value, left, fieldY, available, combo ? control * 10 : fieldHeight);
                if (row.Action)
                {
                    Place(row.Action, left + available + gap, fieldY, actionWidth - gap, control);
                }
            }
            y += row.Height;
        }
    }

    void Paint(HDC dc, const RECT&) override
    {
        const int footer = _height - _metrics.Footer();
        if (_tree)
        {
            Fill(dc, Rect(0, 0, _side, footer), _colors.Panel);
            Fill(dc, Rect(_side - 1, 0, _side, footer), _colors.Border);
        }
        Fill(dc, Rect(_side, _metrics.Header() - 1, _width, _metrics.Header()), _colors.Border);
        if (_page == Page::Hardware || _page == Page::Memory)
        {
            DrawRamMarks(dc);
        }
        else if ((_page == Page::Storage || _page == Page::Disk) && _diskMode == DiskMode::Create)
        {
            DrawStorageMarks(dc);
        }
        Fill(dc, Rect(0, footer, _width, _height), _colors.Panel);
        Fill(dc, Rect(0, footer, _width, footer + 1), _colors.Border);
    }

    POINT MinimumSize() const override
    {
        return {_metrics.Character * (_editing ? 57 : 42), _metrics.Line * 15};
    }

    SIZE PreferredSize(SIZE available) const override
    {
        const auto minimum = MinimumSize();
        const auto rows =
            _editing
                ? std::ranges::count_if(SettingsPages,
                                        [&](const auto& entry) {
                                            return !entry.Option ||
                                                   _selected.Properties.contains(DeviceOptions[*entry.Option].first);
                                        })
                : static_cast<ptrdiff_t>(CreationPages.size());
        int labelWidth = 0;
        for (const auto label : {IDS_MaxResolution, IDS_DiskSize, IDS_Accel, IDS_Cache, IDS_InitialResolution})
        {
            labelWidth = std::max(labelWidth, TextWidth(LText(label)));
        }
        const int pathRow = _metrics.Character * 32 + TextWidth(LText(IDS_Browse)) + _metrics.Gap * 3;
        const int preferred = TreeWidth(available.cx) + _metrics.Margin * 2 + labelWidth + _metrics.Gap + pathRow;
        const int formHeight = BodyTop() + _metrics.Row * 8 + FooterSpace();
        const int treeHeight = static_cast<int>(rows) * _metrics.Control + _metrics.Footer() + _metrics.Margin;
        return {std::clamp<LONG>(preferred, std::min(minimum.x, available.cx), available.cx),
                std::min<LONG>(available.cy, std::max(formHeight, treeHeight))};
    }

    bool TranslateKey(const MSG& message) override
    {
        if (message.message != WM_KEYDOWN || message.wParam != VK_TAB || GetKeyState(VK_CONTROL) < 0 ||
            GetAncestor(message.hwnd, GA_ROOT) != _window)
        {
            return false;
        }
        std::vector<TabStop> stops;
        if (_tree)
        {
            stops.push_back({_tree, -1, -1});
        }
        int offset{};
        for (const auto& row : Rows())
        {
            if (!row.Height)
            {
                continue;
            }
            for (const auto control : {row.Value, row.Action})
            {
                if (control && IsWindowEnabled(control) && (GetWindowLongPtrW(control, GWL_STYLE) & WS_TABSTOP))
                {
                    stops.push_back({control, offset, offset + row.Height});
                }
            }
            offset += row.Height;
        }
        for (const auto button : {_back, _next, _cancel})
        {
            if (IsWindowEnabled(button))
            {
                stops.push_back({button, -1, -1});
            }
        }
        return NextFormControl(stops, _scroll, _page == Page::Review || _page == Page::CommandLine ? 0 : Viewport());
    }

    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override
    {
        if (message == WM_NOTIFY && _tree)
        {
            const auto header = reinterpret_cast<NMHDR*>(lparam);
            if (header->hwndFrom == _tree && (header->code == TVN_SELCHANGINGW || header->code == TVN_SELCHANGEDW))
            {
                const auto& item = reinterpret_cast<NMTREEVIEWW*>(lparam)->itemNew;
                const auto page = static_cast<Page>(item.lParam);
                if (!_building && header->code == TVN_SELCHANGEDW)
                {
                    auto found = _connections.find(item.hItem);
                    _storageSelection = found == _connections.end() ? L"" : found->second;
                    if (page == Page::Controller && _page == page)
                    {
                        ConnectionSummary();
                        Refresh();
                    }
                }
                if (page != _page && !_building)
                {
                    if (header->code == TVN_SELCHANGEDW)
                    {
                        Build(page);
                    }
                    else if (page == Page::Processor || page == Page::Memory || page == Page::Display ||
                             page == Page::Advanced)
                    {
                        try
                        {
                            MachineInfo();
                        }
                        catch (const std::exception& error)
                        {
                            ShowError(_window, Wide(error.what()));
                            return TRUE;
                        }
                    }
                }
                return 0;
            }
        }
        if (message == WM_VSCROLL || message == WM_MOUSEWHEEL)
        {
            if (message == WM_MOUSEWHEEL)
            {
                _scroll -= GET_WHEEL_DELTA_WPARAM(wparam) * _metrics.Row / WHEEL_DELTA;
            }
            else
            {
                SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
                GetScrollInfo(_window, SB_VERT, &info);
                switch (LOWORD(wparam))
                {
                case SB_LINEUP:
                    _scroll -= _metrics.Row;
                    break;
                case SB_LINEDOWN:
                    _scroll += _metrics.Row;
                    break;
                case SB_PAGEUP:
                    _scroll -= Viewport();
                    break;
                case SB_PAGEDOWN:
                    _scroll += Viewport();
                    break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:
                    _scroll = info.nTrackPos;
                    break;
                default:
                    break;
                }
            }
            Refresh();
            return 0;
        }
        if (message == WM_HSCROLL)
        {
            if (_activeCommand)
            {
                return 0;
            }
            const auto control = reinterpret_cast<HWND>(lparam);
            if (!control)
            {
                return 0;
            }
            const auto position = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            const auto id = GetDlgCtrlID(control);
            if (id == SizeSlider || id == RamSlider)
            {
                MoveCapacitySlider(control, id, position, LOWORD(wparam));
            }
            else if (id == VramSlider)
            {
                Set(Find(Vram), std::to_wstring(1u << position));
            }
            return 0;
        }
        if (message == WM_COMMAND && !_building)
        {
            const auto id = LOWORD(wparam);
            if (_activeCommand && id != IDCANCEL && id != StorageEdit && id != DiskOptions && id != OpticalOptions &&
                id != CopyLaunch)
            {
                return 0;
            }
            if (_editing && (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_SELCHANGE))
            {
                _changed.insert(_page);
            }
            if (_editing && id == Name && HIWORD(wparam) == EN_CHANGE)
            {
                const auto path = Find(DiskPath);
                const bool autoPath = _diskMode == DiskMode::Create && path && Read(path) == DefaultDisk().native();
                _draft.Name = Trim(Read(Find(Name)));
                if (autoPath)
                {
                    Set(path, DefaultDisk().native());
                }
                return 0;
            }
            if (HIWORD(wparam) == EN_CHANGE && id == Iso)
            {
                ShowRecommendation();
                return 0;
            }
            if (HIWORD(wparam) == EN_CHANGE && (id == Width || id == Height || id == MaxWidth || id == MaxHeight) &&
                _page == Page::Display)
            {
                UpdateVideo(true);
                return 0;
            }
            if (HIWORD(wparam) == EN_KILLFOCUS)
            {
                if (id == Iso && _editing && IsFile(Trim(Read(Find(Iso)))))
                {
                    InspectMedia();
                }
                try
                {
                    if (id == DiskSize)
                    {
                        const auto size = Number(Find(DiskSize), LText(IDS_DiskSize), MaxDiskMiB);
                        SetCapacityPosition(Find(SizeSlider), DiskPosition(size));
                    }
                    else if (id == Ram)
                    {
                        const auto size = Number(Find(Ram), LText(IDS_Ram), _ramScale.Maximum);
                        SetCapacityPosition(Find(RamSlider), _ramScale.Position(size));
                    }
                    else if (id == Vram && _showVram)
                    {
                        const auto size = Number(Find(Vram), LText(IDS_Vram), _vramMaximum);
                        SendMessageW(Find(VramSlider), TBM_SETPOS, TRUE, std::bit_width(size) - 1);
                    }
                }
                catch (const std::exception&)
                {
                }
                return 0;
            }
            if (HIWORD(wparam) == CBN_SELCHANGE)
            {
                if (id >= OptionStart && id < OptionStart + DeviceOptions.size())
                {
                    const auto key = DeviceOptions[id - OptionStart].first;
                    const auto choice = CurrentIndex(Find(id));
                    if (choice > 0)
                    {
                        _draft.MachineOptions[key] = choice == 1;
                    }
                    else
                    {
                        _draft.MachineOptions.erase(key);
                    }
                    return 0;
                }
                switch (id)
                {
                case MachineId:
                    SetMachine(Selection(Find(MachineId)));
                    if (!_editing)
                    {
                        Build(_page);
                    }
                    break;
                case Bus:
                    if (_editing)
                    {
                        SetBus(Selection(Find(Bus)));
                    }
                    break;
                case ForcePs2:
                    if (CurrentIndex(Find(ForcePs2)) == 1 && MachineInfo().Properties.contains(L"i8042"))
                    {
                        _draft.MachineOptions[L"i8042"] = true;
                        UpdateOption(0);
                    }
                    break;
                case Backend:
                    UpdateNetwork();
                    Refresh();
                    break;
                case SerialMode:
                case DebugMode:
                    UpdatePort(id);
                    Refresh();
                    break;
                case DiskModeId:
                    ChangeDiskMode();
                    break;
                case Vga:
                case Ati:
                case InitialMode:
                case MaximumMode:
                    UpdateVideo(true);
                    break;
                case Llvm:
                    ShowRow(Find(Cache), Selection(Find(Llvm)) == L"TCG-LLVM");
                    ShowRow(Find(LlvmThreads), Selection(Find(Llvm)) == L"TCG-LLVM" && _capabilities.LlvmThreads);
                    Refresh();
                    break;
                default:
                    break;
                }
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED)
            {
                switch (id)
                {
                case StorageEdit:
                case DiskOptions:
                case OpticalOptions:
                    StorageConnections();
                    return 0;
                case SerialBrowse:
                case DebugBrowse: {
                    auto path = ChooseFile(_window, LText(IDS_LogFile), LText(IDS_LogFile), L"*.*",
                                           FileChoice::ExistingOrNew, Read(Find(id - 1)));
                    if (!path.empty())
                    {
                        Set(Find(id - 1), path.native());
                    }
                    return 0;
                }
                case CopyLaunch:
                    CopyCommandText(Find(LaunchText));
                    return 0;
                case Next:
                    if (_editing || _page == Page::Review)
                    {
                        Finish();
                    }
                    else
                    {
                        Capture(_page, true);
                        if (_page == Page::Identity)
                        {
                            MachineInfo();
                        }
                        Build(std::next(std::ranges::find(CreationPages, _page, &PageInfo::Id))->Id);
                    }
                    return 0;
                case Back:
                    if (!_editing && _page != Page::Identity)
                    {
                        Capture(_page, false);
                        Build(std::prev(std::ranges::find(CreationPages, _page, &PageInfo::Id))->Id);
                    }
                    return 0;
                case Cancel:
                case IDCANCEL:
                    DestroyWindow(_window);
                    return 0;
                case Browse:
                case BrowseDisk:
                case BrowseNvram:
                    PickFile(id);
                    return 0;
                default:
                    break;
                }
            }
        }
        return Window::Message(message, wparam, lparam);
    }

    const Engine& _engine;
    const Capabilities& _capabilities;
    bool _editing{};
    CommandPreview _preview;
    std::optional<std::wstring> _activeCommand;
    Profile _draft;
    Machine _selected;
    std::optional<Fs::path> _inspectedIso;
    bool _appliedRecommendation{};
    MediaRecommendation _recommendation;
    DiskMode _diskMode = DiskMode::None;
    Fs::path _existingPath;
    Fs::path _newDiskPath;
    unsigned _size = NewDiskMiB;
    RamSliderScale _ramScale;
    int _capacityPosition{};
    bool _draggingCapacity{};
    unsigned _vramMinimum{};
    unsigned _vramMaximum{};
    bool _showVram{};
    Page _page;
    std::map<Page, HTREEITEM> _nodes;
    std::set<Page> _changed;
    std::map<Page, std::vector<Row>> _forms;
    std::vector<std::wstring> _backends;
    int _side{};
    std::map<HTREEITEM, std::wstring> _connections;
    std::wstring _storageSelection;
    int _invalidField{};
    bool _building{};
    int _width{};
    int _height{};
    int _scroll{};
    HWND _tree{}, _cpuLimit{}, _memoryLimit{};
    HWND _title{}, _step{}, _back{}, _next{}, _cancel{}, _hint{};
};

} // namespace

void ShowRunningProfile(HWND owner, Appearance appearance, Engine engine, Capabilities capabilities, Machine machine,
                        Profile profile, std::wstring command)
{
    ProfileWindow window(appearance, engine, capabilities, &profile, std::move(machine), {}, std::move(command));
    window.Open(LFmt(IDS_WizardEdit, profile.Name), owner,
                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN);
    window.Modal();
}

std::optional<ProfileResult> ShowProfileWindow(HWND owner, Appearance appearance, const Engine& engine,
                                               const Capabilities& capabilities, const Profile* existing,
                                               CommandPreview preview)
{
    Machine machine;
    if (existing)
    {
        if (const auto base = capabilities.Find(existing->Machine))
        {
            Busy(owner, appearance, LText(IDS_HardwareLoading), [&] { machine = ProbeMachine(engine, *base); });
        }
    }
    ProfileWindow window(appearance, engine, capabilities, existing, std::move(machine), std::move(preview));
    window.Open(existing ? LFmt(IDS_WizardEdit, existing->Name) : LText(IDS_WizardCreate), owner,
                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN);
    window.Modal();
    return std::move(window.Result);
}
} // namespace Ia64::Ui
