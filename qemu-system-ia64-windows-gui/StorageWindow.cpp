// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "StorageWindow.h"
#include "Media.h"
#include "Devices.h"
#include "Localization.h"

#include <algorithm>
#include <format>
#include <windowsx.h>

namespace Ia64::Ui
{
namespace
{
enum Id
{
    Tree = 4000,
    AddController,
    AddDrive,
    Remove,
    Model,
    Enabled,
    Controller,
    Media,
    Slot,
    Path,
    Browse,
    Format,
    Protect,
    Cache,
    Discard,
    Zeroes,
    Boot,
    PciSlot
};
struct Field
{
    int Id;
    HWND Label, Value;
    bool Combo;
};

class StorageWindow final : public Window
{
public:
    StorageWindow(Appearance theme, const Profile& p, const Engine& engine, const Machine& m, const Capabilities& caps,
                  Fs::path pending, std::wstring selection, bool readOnly)
        : Window(theme), Draft(p), _engine(engine), _machine(m), _caps(caps), _pending(std::move(pending)),
          _id(std::move(selection)), _readOnly(readOnly)
    {
    }
    Profile Draft;
    bool Saved{};

private:
    HWND At(int id) const
    {
        return GetDlgItem(_window, id);
    }
    Drive* CurrentDrive()
    {
        for (auto d : AllDrives(Draft))
        {
            if (d->Id == _id)
            {
                return d;
            }
        }
        return nullptr;
    }
    std::vector<StorageController> Controllers() const
    {
        std::vector<StorageController> all{{L"onboard", Draft.Bus}};
        all.insert(all.end(), Draft.Controllers.begin(), Draft.Controllers.end());
        return all;
    }
    std::wstring Caption(const StorageController& c) const
    {
        unsigned number{};
        for (const auto& item : Controllers())
        {
            if (item.Model == c.Model)
            {
                ++number;
            }
            if (item.Id == c.Id)
            {
                break;
            }
        }
        return number > 1 ? std::format(L"{} ({})", c.Model, number) : c.Model;
    }
    static std::wstring DriveTitle(const Drive& d)
    {
        auto name =
            d.File.empty() ? LText(d.Media == L"cdrom" ? IDS_OpticalDrive : IDS_DiskFile) : d.File.filename().native();
        if (!d.Attached)
        {
            name += L" (" + LText(IDS_Disabled) + L")";
        }
        return std::format(L"{}  {}", d.Slot, name);
    }
    void Create() override
    {
        _tree = Child(WC_TREEVIEWW, LText(IDS_Attachments), Tree,
                      WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT |
                          TVS_NONEVENHEIGHT);
        if (_readOnly)
        {
            _cancel = OnPanel(Button(IDCANCEL, LText(IDS_CloseCommand), true));
        }
        else
        {
            _addController = Button(AddController, LText(IDS_AddController));
            _addDrive = Button(AddDrive, LText(IDS_AddDrive));
            _remove = Button(Remove, LText(IDS_Unplug));
            _save = OnPanel(Button(IDOK, LText(IDS_ApplyChanges), true));
            _cancel = OnPanel(Button(IDCANCEL, LText(IDS_Cancel)));
        }
        const auto field = [&](int id, int caption, bool combo, DWORD style = ES_AUTOHSCROLL) {
            auto value = combo ? Combo(id) : Edit(id, L"", style);
            if (_readOnly)
            {
                if (combo)
                {
                    Enable(value, false);
                }
                else
                {
                    SendMessageW(value, EM_SETREADONLY, TRUE, 0);
                }
            }
            _fields.push_back({id, Label(LText(caption), TextRole::Body, SS_CENTERIMAGE), value, combo});
        };
        field(Model, IDS_ControllerModel, true);
        field(Enabled, IDS_DeviceState, true);
        field(Controller, IDS_Bus, true);
        field(Media, IDS_MediaType, true);
        field(Slot, IDS_Connector, false, ES_NUMBER);
        field(Path, IDS_Image, false);
        field(Format, IDS_ImageFormat, false, ES_READONLY);
        field(Protect, IDS_Access, true);
        field(Cache, IDS_CacheMode, true);
        field(Discard, IDS_Discard, true);
        field(Zeroes, IDS_DetectZeroes, true);
        field(Boot, IDS_BootIndex, false, ES_NUMBER);
        field(PciSlot, IDS_PciSlot, false, ES_NUMBER);
        DefaultHint(At(Boot), LText(IDS_FirmwareBootOrder));
        if (!_readOnly)
        {
            _browse = Button(Browse, LText(IDS_Browse));
        }
        Rebuild(_id);
    }
    void Rebuild(const std::wstring& select)
    {
        _loading = true;
        TreeView_DeleteAllItems(_tree);
        _items.clear();
        for (const auto& c : Controllers())
        {
            const auto parent = Insert(TVI_ROOT, c.Id, Caption(c));
            for (const auto d : AllDrives(Draft))
            {
                if (d->Controller == c.Id)
                {
                    Insert(parent, d->Id, DriveTitle(*d));
                }
            }
            TreeView_Expand(_tree, parent, TVE_EXPAND);
        }
        _id = _items.contains(select) ? select : L"onboard";
        TreeView_SelectItem(_tree, _items.at(_id));
        _loading = false;
        Load();
    }
    HTREEITEM Insert(HTREEITEM parent, const std::wstring& id, std::wstring title)
    {
        TVINSERTSTRUCTW item{};
        item.hParent = parent;
        item.hInsertAfter = TVI_LAST;
        item.item.mask = TVIF_TEXT;
        item.item.pszText = title.data();
        return _items[id] = TreeView_InsertItem(_tree, &item);
    }
    void Load()
    {
        _loading = true;
        _offset = 0;
        const auto d = CurrentDrive();
        if (d)
        {
            std::vector<std::wstring> names;
            int index = 0;
            for (const auto& c : Controllers())
            {
                if (c.Id == d->Controller)
                {
                    index = static_cast<int>(names.size());
                }
                names.push_back(Caption(c));
            }
            Options(At(Controller), names, names.at(index));
            Options(At(Media), {L"disk", L"cdrom"}, d->Media);
            YesNo(Enabled, d->Attached);
            DefaultHint(At(Enabled), LText(IDS_Enabled));
            Set(At(Slot), std::to_wstring(d->Slot));
            Set(At(Path), d->File.native());
            Set(At(Boot), d->BootIndex >= 0 ? std::to_wstring(d->BootIndex) : L"");
            Choice(Cache, L"writeback", {L"writeback", L"writethrough", L"none", L"directsync", L"unsafe"}, d->Cache);
            Choice(Discard, L"ignore", {L"ignore", L"unmap"}, d->Discard);
            Choice(Zeroes, L"off", {L"off", L"on", L"unmap"}, d->DetectZeroes);
            if (!_readOnly)
            {
                ReadFormat(*d, false);
            }
            ShowImageProperties(*d);
        }
        else
        {
            auto models = Buses(_machine.Name);
            if (_id != L"onboard")
            {
                if (_machine.Name == L"hp-zx6000")
                {
                    models.push_back(L"SCSI 2");
                }
                models.insert(models.end(), _machine.ControllerModels.begin(), _machine.ControllerModels.end());
            }
            Options(At(Model), models, ControllerModel(Draft, _id));
            if (_id != L"onboard")
            {
                const auto slot = std::ranges::find(Draft.Controllers, _id, &StorageController::Id)->PciSlot;
                Set(At(PciSlot), slot ? std::to_wstring(slot) : L"");
                PciDefault();
            }
        }
        if (!_readOnly)
        {
            Enable(_remove, _id != L"onboard" && _id != Draft.Primary.Id && _id != Draft.Optical.Id);
        }
        _loading = false;
        Refresh();
    }
    void PciDefault()
    {
        if (_id == L"onboard" || !std::ranges::contains(_machine.ControllerModels, Selection(At(Model))))
        {
            return;
        }
        auto automatic = Draft;
        auto& controller = *std::ranges::find(automatic.Controllers, _id, &StorageController::Id);
        controller.PciSlot = 0;
        try
        {
            DefaultHint(At(PciSlot), std::to_wstring(ControllerAddress(automatic, controller)));
        }
        catch (const ProfileValidationError&)
        {
            DefaultHint(At(PciSlot), LText(IDS_NoFreeSlot));
        }
    }
    void Choice(int id, std::wstring_view defaultValue, std::vector<std::wstring> values, const std::wstring& value)
    {
        values.insert(values.begin(), LFmt(IDS_DefaultValue, defaultValue));
        Options(At(id), values, value.empty() ? values.front() : value);
    }
    std::wstring Override(int id) const
    {
        return SendMessageW(At(id), CB_GETCURSEL, 0, 0) > 0 ? Selection(At(id)) : L"";
    }
    void YesNo(int id, bool yes)
    {
        const std::vector values{LText(IDS_Disabled), LText(IDS_Enabled)};
        Options(At(id), values, values[yes]);
    }
    void ShowImageProperties(const Drive& d)
    {
        ComboBox_SelectString(At(Media), -1, d.Media.c_str());
        YesNo(Protect, d.Protected());
        DefaultHint(At(Protect), LText(d.Locked() ? IDS_Enabled : IDS_Disabled));
        Enable(At(Protect), !_readOnly && !d.Locked());
        Set(At(Format), d.Format);
    }
    void ReadFormat(Drive& d, bool strict)
    {
        const Fs::path path = Trim(Read(At(Path)));
        d.UpdateImage(path, SameFilePath(path, _pending));
        if (path.empty() || !d.Format.empty() || (!strict && !IsFile(path)))
        {
            return;
        }
        auto format = InspectDisk(_engine, path).Format;
        if (format != L"qcow2" && format != L"raw")
        {
            Fail(LText(IDS_BadFormat));
        }
        d.Format = std::move(format);
    }
    void Store()
    {
        if (auto d = CurrentDrive())
        {
            d->Attached = SendMessageW(At(Enabled), CB_GETCURSEL, 0, 0) == 1;
            d->Controller = Controllers().at(SendMessageW(At(Controller), CB_GETCURSEL, 0, 0)).Id;
            d->Slot = Number(At(Slot), LText(IDS_Connector), UINT_MAX, true);
            d->BootIndex =
                Read(At(Boot)).empty() ? -1 : static_cast<int>(Number(At(Boot), LText(IDS_BootIndex), INT_MAX, true));
            d->Cache = Override(Cache);
            d->Discard = Override(Discard);
            d->DetectZeroes = Override(Zeroes);
            ReadFormat(*d, d->Attached);
            ShowImageProperties(*d);
        }
        else if (_id == L"onboard")
        {
            Draft.Bus = Selection(At(Model));
        }
        else
        {
            auto& c = *std::ranges::find(Draft.Controllers, _id, &StorageController::Id);
            c.Model = Selection(At(Model));
            c.PciSlot = std::ranges::contains(_machine.ControllerModels, c.Model)
                            ? Number(At(PciSlot), LText(IDS_PciSlot), 31, true)
                            : 0;
        }
        RenameItem();
    }
    void RenameItem()
    {
        if (!_items.contains(_id))
        {
            return;
        }
        std::wstring name;
        if (const auto d = CurrentDrive())
        {
            name = DriveTitle(*d);
        }
        else
        {
            for (const auto& c : Controllers())
            {
                if (c.Id == _id)
                {
                    name = Caption(c);
                    break;
                }
            }
        }
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        item.hItem = _items.at(_id);
        item.pszText = name.data();
        TreeView_SetItem(_tree, &item);
    }
    int SidebarWidth(int width) const
    {
        int preferred = _metrics.Character * 17;
        for (const auto& c : Controllers())
        {
            preferred = std::max(preferred, TextWidth(Caption(c)) + _metrics.Line + _metrics.Gap * 2);
        }
        for (const auto d : AllDrives(Draft))
        {
            preferred = std::max(preferred, TextWidth(DriveTitle(*d)) + _metrics.Line * 2 + _metrics.Gap * 3);
        }
        return std::min({preferred, _metrics.Character * 26, width / 3});
    }
    POINT MinimumSize() const override
    {
        return {_metrics.Character * 50, _metrics.Line * 15};
    }
    SIZE PreferredSize(SIZE available) const override
    {
        const int caption = std::max(TextWidth(LText(IDS_ControllerModel)), TextWidth(LText(IDS_BootIndex)));
        const int form = caption + _metrics.Character * 31 + TextWidth(LText(IDS_Browse)) + _metrics.Gap * 4;
        return {std::min<LONG>(available.cx, SidebarWidth(available.cx) + _metrics.Margin * 2 + form),
                std::min<LONG>(available.cy, _metrics.Row * 12 + _metrics.Footer() + _metrics.Margin * 2)};
    }
    void Layout(int width, int height) override
    {
        const int gap = _metrics.Gap, margin = _metrics.Margin, row = _metrics.Row, control = _metrics.Control;
        int x = gap, barY = gap;
        for (auto button : {_addController, _addDrive, _remove})
        {
            if (!button)
            {
                continue;
            }
            const int w = ButtonWidth(button);
            if (x > gap && x + w > width - gap)
            {
                x = gap;
                barY += row;
            }
            Place(button, x, barY, w, control);
            x += w + gap;
        }
        _barBottom = _readOnly ? 0 : barY + control + gap;
        _footerTop = height - _metrics.Footer();
        _side = SidebarWidth(width);
        const int top = _barBottom + gap, bottom = _footerTop - gap;
        Place(_tree, _metrics.Unit, top, _side - gap, bottom - top);
        TreeView_SetItemHeight(_tree, Px(control));
        TreeView_SetIndent(_tree, Px(_metrics.Line));

        const bool drive = CurrentDrive() != nullptr;
        const auto relevant = [&](int id) {
            if (id == PciSlot)
            {
                return !drive && _id != L"onboard" &&
                       std::ranges::contains(_machine.ControllerModels, Selection(At(Model)));
            }
            return (id != Model) == drive;
        };
        int caption = 0, count = 0;
        for (const auto& f : _fields)
        {
            if (relevant(f.Id))
            {
                caption = std::max(caption, TextWidth(Read(f.Label)));
                ++count;
            }
        }
        const int left = _side + margin, right = width - margin;
        const bool stacked = right - left < caption + gap + _metrics.Character * 23;
        const int above = stacked ? _metrics.Line : 0;
        const int start = stacked ? left : left + caption + gap;
        const int viewport = std::max(0, bottom - top);
        const int extent = count * (row + above);
        _offset = std::clamp(_offset, 0, std::max(0, extent - viewport));
        SCROLLINFO sc{
            sizeof(sc), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, extent - 1), static_cast<UINT>(viewport),
            _offset};
        SetScrollInfo(_window, SB_VERT, &sc, TRUE);
        RECT client{};
        GetClientRect(_window, &client);
        if (Dip(client.right) != width)
        {
            Layout(Dip(client.right), height);
            return;
        }

        int y = top - _offset;
        _tabs = {{_addController}, {_addDrive}, {_remove}, {_tree}};
        for (const auto& f : _fields)
        {
            const bool use = relevant(f.Id);
            const bool show = use && y >= top && y + above + control <= bottom;
            for (auto h : {f.Label, f.Value})
            {
                ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
            }
            if (f.Id == Path && _browse)
            {
                ShowWindow(_browse, show ? SW_SHOWNA : SW_HIDE);
            }
            if (!use)
            {
                continue;
            }
            const int rowTop = y - top + _offset;
            _tabs.push_back({f.Value, rowTop, rowTop + above + control});
            if (f.Id == Path && _browse)
            {
                _tabs.push_back({_browse, rowTop, rowTop + above + control});
            }
            if (show)
            {
                const int browse = f.Id == Path && _browse ? ButtonWidth(_browse) + gap : 0;
                const int maxWidth = f.Id == Format ? _metrics.Character * 9 + 2 * gap : _metrics.PathWidth();
                const int fieldWidth = InputWidth(f.Value, std::min(right - start - browse, maxWidth));
                Place(f.Label, left, y, stacked ? right - left : caption, stacked ? above : control);
                Place(f.Value, start, y + above, fieldWidth, f.Combo ? control * 10 : control);
                if (browse)
                {
                    Place(_browse, start + fieldWidth + gap, y + above, browse - gap, control);
                }
            }
            y += row + above;
        }
        _tabs.push_back({_save});
        _tabs.push_back({_cancel});
        PlaceFooter(width, height, {_cancel, _save});
    }
    int Viewport() const
    {
        return std::max(0, _footerTop - _barBottom - _metrics.Gap * 2);
    }
    bool TranslateKey(const MSG& message) override
    {
        return message.message == WM_KEYDOWN && message.wParam == VK_TAB && GetKeyState(VK_CONTROL) >= 0 &&
               GetAncestor(message.hwnd, GA_ROOT) == _window && NextFormControl(_tabs, _offset, Viewport());
    }
    void Paint(HDC dc, const RECT& area) override
    {
        const int right = Dip(area.right), bottom = Dip(area.bottom);
        Fill(dc, Rect(0, _barBottom, _side, _footerTop), _colors.Panel);
        Fill(dc, Rect(_side - 1, _barBottom, _side, _footerTop), _colors.Border);
        Fill(dc, Rect(0, _barBottom - 1, right, _barBottom), _colors.Border);
        Fill(dc, Rect(0, _footerTop, right, bottom), _colors.Panel);
        Fill(dc, Rect(0, _footerTop, right, _footerTop + 1), _colors.Border);
    }
    LRESULT Message(UINT msg, WPARAM wp, LPARAM lp) override
    {
        if (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL)
        {
            if (msg == WM_MOUSEWHEEL)
            {
                _offset -= GET_WHEEL_DELTA_WPARAM(wp) * _metrics.Row / WHEEL_DELTA;
            }
            else
            {
                switch (LOWORD(wp))
                {
                case SB_LINEUP:
                    _offset -= _metrics.Row;
                    break;
                case SB_LINEDOWN:
                    _offset += _metrics.Row;
                    break;
                case SB_PAGEUP:
                    _offset -= Viewport();
                    break;
                case SB_PAGEDOWN:
                    _offset += Viewport();
                    break;
                case SB_TOP:
                    _offset = 0;
                    break;
                case SB_BOTTOM:
                    _offset = INT_MAX;
                    break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO sc{sizeof(sc), SIF_TRACKPOS};
                    GetScrollInfo(_window, SB_VERT, &sc);
                    _offset = sc.nTrackPos;
                    break;
                }
                }
            }
            Refresh();
            return 0;
        }
        if (msg == WM_NOTIFY && !_loading && reinterpret_cast<NMHDR*>(lp)->hwndFrom == _tree)
        {
            auto n = reinterpret_cast<NMTREEVIEWW*>(lp);
            if (n->hdr.code == TVN_SELCHANGINGW && !_readOnly)
            {
                try
                {
                    Store();
                }
                catch (const std::exception& e)
                {
                    ShowError(_window, Wide(e.what()));
                    return TRUE;
                }
            }
            if (n->hdr.code == TVN_SELCHANGEDW)
            {
                for (const auto& [id, item] : _items)
                {
                    if (item == n->itemNew.hItem)
                    {
                        _id = id;
                        Load();
                        break;
                    }
                }
            }
        }
        if (msg == WM_COMMAND && !_loading)
        {
            const auto id = LOWORD(wp);
            if (id == IDCANCEL)
            {
                DestroyWindow(_window);
                return 0;
            }
            if (_readOnly)
            {
                return 0;
            }
            if (id == Browse && HIWORD(wp) == BN_CLICKED)
            {
                auto path = ChooseFile(_window, LText(IDS_Image), LText(IDS_StorageFile), L"*.*");
                if (!path.empty())
                {
                    auto d = CurrentDrive();
                    d->SelectImage(path);
                    Set(At(Path), path.native());
                    ReadFormat(*d, true);
                    ShowImageProperties(*d);
                    RenameItem();
                }
                return 0;
            }
            if (id == Model && HIWORD(wp) == CBN_SELCHANGE)
            {
                Store();
                PciDefault();
                Refresh();
                return 0;
            }
            if (id == Protect && HIWORD(wp) == CBN_SELCHANGE)
            {
                CurrentDrive()->ReadOnly = SendMessageW(At(Protect), CB_GETCURSEL, 0, 0) == 1;
                return 0;
            }
            if (id == Media && HIWORD(wp) == CBN_SELCHANGE)
            {
                auto d = CurrentDrive();
                d->Media = Selection(At(Media));
                ShowImageProperties(*d);
                return 0;
            }
            if (id == Path && HIWORD(wp) == EN_CHANGE)
            {
                if (const auto drive = CurrentDrive())
                {
                    auto preview = *drive;
                    const Fs::path path = Trim(Read(At(Path)));
                    if (path != drive->File)
                    {
                        preview.Format.clear();
                        if (HasIsoExtension(path) && !SameFilePath(path, _pending))
                        {
                            preview.Media = L"cdrom";
                        }
                    }
                    if (preview.Media != Selection(At(Media)) || preview.Format != Read(At(Format)))
                    {
                        ShowImageProperties(preview);
                    }
                }
                return 0;
            }
            if (id == Path && HIWORD(wp) == EN_KILLFOCUS && CurrentDrive())
            {
                auto d = CurrentDrive();
                ReadFormat(*d, false);
                ShowImageProperties(*d);
                RenameItem();
                return 0;
            }
            if (HIWORD(wp) != BN_CLICKED)
            {
                return 0;
            }
            if (id == Remove)
            {
                if (CurrentDrive())
                {
                    std::erase_if(Draft.Drives, [&](const Drive& d) { return d.Id == _id; });
                }
                else
                {
                    if (std::ranges::any_of(AllDrives(Draft), [&](const Drive* d) { return d->Controller == _id; }))
                    {
                        Fail(LText(IDS_OccupiedController));
                    }
                    std::erase_if(Draft.Controllers, [&](const auto& c) { return c.Id == _id; });
                }
                Rebuild(L"onboard");
                return 0;
            }
            if (id == IDOK || id == AddController || id == AddDrive)
            {
                Store();
                if (id == IDOK)
                {
                    ValidateDevices(Draft, _machine, _caps, _engine);
                    Saved = true;
                    DestroyWindow(_window);
                }
                else if (id == AddController)
                {
                    auto models = Buses(_machine.Name);
                    if (_machine.Name == L"hp-zx6000")
                    {
                        models.push_back(L"SCSI 2");
                    }
                    for (const auto& c : Controllers())
                    {
                        std::erase(models, c.Model);
                    }
                    models.insert(models.end(), _machine.ControllerModels.begin(), _machine.ControllerModels.end());
                    if (models.empty())
                    {
                        Fail(LText(IDS_BusUnavailable));
                    }
                    auto idNew = L"hba-" + NewId();
                    Draft.Controllers.push_back({idNew, models.front()});
                    Rebuild(idNew);
                }
                else
                {
                    Drive d{L"disk-" + NewId().substr(0, 24)};
                    const auto selectedDrive = CurrentDrive();
                    d.Controller = selectedDrive ? selectedDrive->Controller : _id;
                    const auto model = ControllerModel(Draft, d.Controller);
                    auto drives = AllDrives(Draft);
                    while (
                        d.Slot < PortCount(model) &&
                        ((IsScsi(model) && d.Slot == 7) ||
                         std::ranges::any_of(
                             drives,
                             [&](auto other) { return other->Controller == d.Controller && other->Slot == d.Slot; })))
                    {
                        ++d.Slot;
                    }
                    if (d.Slot == PortCount(model))
                    {
                        Fail(LText(IDS_BadDrive));
                    }
                    Draft.Drives.push_back(d);
                    Rebuild(d.Id);
                    SetFocus(At(Path));
                }
                return 0;
            }
        }
        return Window::Message(msg, wp, lp);
    }
    const Engine& _engine;
    const Machine& _machine;
    const Capabilities& _caps;
    Fs::path _pending;
    std::wstring _id;
    std::map<std::wstring, HTREEITEM> _items;
    std::vector<Field> _fields;
    std::vector<TabStop> _tabs;
    bool _readOnly;
    bool _loading{true};
    int _offset{}, _side{}, _barBottom{}, _footerTop{};
    HWND _tree{}, _browse{}, _addController{}, _addDrive{}, _remove{}, _save{}, _cancel{};
};
} // namespace

bool EditStorage(HWND owner, Appearance appearance, Profile& profile, const Engine& engine, const Machine& machine,
                 const Capabilities& caps, const Fs::path& pendingDisk, std::wstring selection, bool readOnly)
{
    StorageWindow window(appearance, profile, engine, machine, caps, pendingDisk, std::move(selection), readOnly);
    window.Open(LText(IDS_Attachments), owner,
                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN);
    window.Modal();
    if (window.Saved)
    {
        profile = std::move(window.Draft);
    }
    return window.Saved;
}
} // namespace Ia64::Ui
