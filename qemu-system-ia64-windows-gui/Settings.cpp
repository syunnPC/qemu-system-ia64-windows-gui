// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Devices.h"
#include "Localization.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <tomlplusplus/toml.hpp>

namespace Ia64
{
namespace
{
Fs::path SettingsPath()
{
    return DataDirectory() / L"settings.toml";
}

template <typename T>
T Setting(const toml::table& table, std::string_view key, std::optional<T> defaultValue = std::nullopt)
{
    const auto node = table[key];
    if (!node && defaultValue)
    {
        return *defaultValue;
    }
    if (auto value = node.value_exact<T>())
    {
        return *value;
    }
    Fail(LFmt(IDS_InvalidSetting, Wide(key)));
}

const toml::table& Table(const toml::table& parent, std::string_view key)
{
    if (const auto node = parent[key])
    {
        if (const auto table = node.as_table())
        {
            return *table;
        }
        Fail(LFmt(IDS_InvalidSetting, Wide(key)));
    }
    static const toml::table empty;
    return empty;
}

std::wstring Text(const toml::table& table, std::string_view key, std::wstring_view defaultValue = {})
{
    return Wide(Setting<std::string>(table, key, Utf8(defaultValue)));
}

unsigned Unsigned(const toml::table& table, std::string_view key, unsigned defaultValue)
{
    const auto value = Setting<int64_t>(table, key, defaultValue);
    if (value < 0 || value > std::numeric_limits<unsigned>::max())
    {
        Fail(LFmt(IDS_InvalidSetting, Wide(key)));
    }
    return static_cast<unsigned>(value);
}

DisplaySettings ReadDisplaySettings(const toml::table& table)
{
    DisplaySettings settings;
    settings.FitToWindow = Setting<bool>(table, "fit_to_window", settings.FitToWindow);
    settings.ResizeToGuest = Setting<bool>(table, "resize_to_guest", settings.ResizeToGuest);
    return settings;
}

void WriteDisplaySettings(toml::table& table, const DisplaySettings& settings)
{
    table.insert("fit_to_window", settings.FitToWindow);
    table.insert("resize_to_guest", settings.ResizeToGuest);
}

toml::table DriveOptions(const Drive& d)
{
    return toml::table{{"controller", Utf8(d.Controller)}, {"slot", d.Slot},
                       {"media", Utf8(d.Media)},           {"attached", d.Attached},
                       {"readonly", d.ReadOnly},           {"cache", Utf8(d.Cache)},
                       {"discard", Utf8(d.Discard)},       {"detect_zeroes", Utf8(d.DetectZeroes)},
                       {"boot_index", d.BootIndex}};
}

void ReadDrive(Drive& d, const toml::table& t)
{
    d.Controller = Text(t, "controller", d.Controller);
    d.Media = Text(t, "media", d.Media);
    d.Slot = Unsigned(t, "slot", d.Slot);
    d.Attached = Setting<bool>(t, "attached", true);
    d.ReadOnly = Setting<bool>(t, "readonly", false);
    d.Cache = Text(t, "cache");
    d.Discard = Text(t, "discard");
    d.DetectZeroes = Text(t, "detect_zeroes");
    const auto index = Setting<int64_t>(t, "boot_index", -1);
    if (index < -1 || index > INT_MAX)
    {
        Fail(LFmt(IDS_InvalidSetting, L"boot_index"));
    }
    d.BootIndex = static_cast<int>(index);
}

toml::table WritePort(const CharPort& p)
{
    return toml::table{
        {"mode", Utf8(p.Mode)}, {"file", Utf8(p.File.native())}, {"host", Utf8(p.Host)}, {"port", p.Port}};
}

CharPort ReadPort(const toml::table& t)
{
    return {Text(t, "mode", L"none"), Text(t, "file"), Text(t, "host", L"127.0.0.1"), Unsigned(t, "port", 0)};
}

void EachTable(const toml::table& from, std::string_view key, const auto& visit)
{
    if (!from.contains(key))
    {
        return;
    }
    const auto array = from[key].as_array();
    if (!array)
    {
        Fail(LFmt(IDS_InvalidSetting, Wide(key)));
    }
    for (const auto& node : *array)
    {
        const auto table = node.as_table();
        if (!table)
        {
            Fail(LFmt(IDS_InvalidSetting, Wide(key)));
        }
        visit(*table);
    }
}

toml::table WriteProfile(const Profile& profile)
{
    toml::table options;
    for (const auto& [name, enabled] : profile.MachineOptions)
    {
        options.emplace(Utf8(name), enabled);
    }
    toml::array controllers, drives;
    for (const auto& c : profile.Controllers)
    {
        controllers.push_back(toml::table{{"id", Utf8(c.Id)}, {"model", Utf8(c.Model)}, {"pci_slot", c.PciSlot}});
    }
    for (const auto& d : profile.Drives)
    {
        auto opts = DriveOptions(d);
        opts.insert("id", Utf8(d.Id));
        opts.insert("file", Utf8(d.File.native()));
        drives.push_back(std::move(opts));
    }
    const auto& n = profile.Network;
    toml::table result{
        {"id", Utf8(profile.Id)},
        {"name", Utf8(profile.Name)},
        {"machine", Utf8(profile.Machine)},
        {"ram_mib", profile.RamMiB},
        {"alat", Utf8(profile.Alat)},
        {"firmware_base", Utf8(profile.FirmwareBase)},
        {"input", toml::table{{"force_ps2", profile.ForcePs2}, {"usb_mouse", profile.UsbMouse}}},
        {"serial", WritePort(profile.Serial)},
        {"debug_port", WritePort(profile.DebugPort)},
        {"network", toml::table{{"backend", Utf8(n.Backend)},
                                {"model", Utf8(n.Model)},
                                {"mac", Utf8(n.Mac)},
                                {"interface", Utf8(n.Interface)},
                                {"host", Utf8(n.Host)},
                                {"port", n.Port},
                                {"forward", Utf8(n.Forward)},
                                {"boot_rom", n.BootRom}}},
        {"cpu", toml::table{{"model", Utf8(profile.Cpu)},
                            {"sockets", profile.Sockets},
                            {"cores", profile.Cores},
                            {"threads", profile.Threads}}},
        {"storage", toml::table{{"disk", Utf8(profile.Primary.File.native())},
                                {"disk_format", Utf8(profile.Primary.Format)},
                                {"nvram", Utf8(profile.Nvram.native())},
                                {"primary", DriveOptions(profile.Primary)},
                                {"optical", DriveOptions(profile.Optical)},
                                {"controllers", std::move(controllers)},
                                {"drives", std::move(drives)},
                                {"interface", Utf8(profile.Bus)},
                                {"iso", Utf8(profile.Optical.File.native())}}},
        {"startup", toml::table{{"no_reboot", profile.NoReboot}, {"skip_snapshot_prompt", profile.SkipSnapshotPrompt}}},
        {"display", toml::table{{"adapter", Utf8(profile.Vga)},
                                {"ati_model", Utf8(profile.AtiModel)},
                                {"width", profile.Width},
                                {"height", profile.Height},
                                {"maximum_width", profile.MaxWidth},
                                {"maximum_height", profile.MaxHeight},
                                {"vram_mib", profile.VramMiB}}},
        {"acceleration",
         toml::table{{"llvm", profile.Llvm}, {"threads", profile.LlvmThreads}, {"cache_mib", profile.CacheMiB}}}};
    if (profile.DisplayOverride)
    {
        WriteDisplaySettings(*result["display"].as_table(), *profile.DisplayOverride);
    }
    if (!options.empty())
    {
        result.insert("machine_options", std::move(options));
    }
    return result;
}

Profile ReadProfile(const toml::table& table)
{
    Profile profile;
    profile.Id = Wide(Setting<std::string>(table, "id"));
    if (profile.Id.size() != 36 || profile.Id.find_first_not_of(L"0123456789abcdefABCDEF-") != std::wstring::npos)
    {
        Fail(LText(IDS_InvalidVmId));
    }

    profile.Name = Wide(Setting<std::string>(table, "name"));
    profile.Machine = Text(table, "machine", profile.Machine);
    profile.RamMiB = Unsigned(table, "ram_mib", profile.RamMiB);

    const auto& cpu = Table(table, "cpu");
    profile.Cpu = Text(cpu, "model");
    profile.Sockets = Unsigned(cpu, "sockets", profile.Sockets);
    profile.Cores = Unsigned(cpu, "cores", profile.Cores);
    profile.Threads = Unsigned(cpu, "threads", profile.Threads);

    const auto& storage = Table(table, "storage");
    profile.Primary.File = Text(storage, "disk");
    profile.Primary.Format = Text(storage, "disk_format");
    profile.Bus = Text(storage, "interface", profile.Bus);
    profile.Optical.File = Text(storage, "iso");
    profile.Nvram = Text(storage, "nvram");
    profile.SkipSnapshotPrompt = Setting<bool>(Table(table, "startup"), "skip_snapshot_prompt", false);

    ReadDrive(profile.Primary, Table(storage, "primary"));
    ReadDrive(profile.Optical, Table(storage, "optical"));
    EachTable(storage, "controllers", [&](const toml::table& item) {
        profile.Controllers.push_back({Text(item, "id"), Text(item, "model"), Unsigned(item, "pci_slot", 0)});
    });
    EachTable(storage, "drives", [&](const toml::table& item) {
        Drive d{Text(item, "id")};
        d.File = Text(item, "file");
        ReadDrive(d, item);
        profile.Drives.push_back(std::move(d));
    });
    profile.NoReboot = Setting<bool>(Table(table, "startup"), "no_reboot", false);
    profile.ForcePs2 = Setting<bool>(Table(table, "input"), "force_ps2", false);
    profile.UsbMouse = Setting<bool>(Table(table, "input"), "usb_mouse", false);
    profile.Alat = Text(table, "alat");
    profile.FirmwareBase = Text(table, "firmware_base");
    profile.Serial = ReadPort(Table(table, "serial"));
    profile.DebugPort = ReadPort(Table(table, "debug_port"));
    const auto& network = Table(table, "network");
    auto& n = profile.Network;
    n.Backend = Text(network, "backend");
    n.Model = Text(network, "model");
    n.Mac = Text(network, "mac");
    n.Interface = Text(network, "interface");
    n.Host = Text(network, "host", L"127.0.0.1");
    n.Port = Unsigned(network, "port", 0);
    n.Forward = Text(network, "forward");
    n.BootRom = Setting<bool>(network, "boot_rom", false);

    const auto& display = Table(table, "display");
    if (display.contains("fit_to_window") || display.contains("resize_to_guest"))
    {
        profile.DisplayOverride = ReadDisplaySettings(display);
    }
    profile.Vga = Text(display, "adapter");
    profile.AtiModel = Text(display, "ati_model");
    profile.Width = Unsigned(display, "width", 0);
    profile.Height = Unsigned(display, "height", 0);
    profile.MaxWidth = Unsigned(display, "maximum_width", 0);
    profile.MaxHeight = Unsigned(display, "maximum_height", 0);
    profile.VramMiB = Unsigned(display, "vram_mib", 0);

    for (const auto& [name, node] : Table(table, "machine_options"))
    {
        const auto enabled = node.value_exact<bool>();
        if (!enabled)
        {
            Fail(LFmt(IDS_InvalidSetting, Wide(name.str())));
        }
        profile.MachineOptions.emplace(Wide(name.str()), *enabled);
    }

    const auto& acceleration = Table(table, "acceleration");
    profile.Llvm = Setting<bool>(acceleration, "llvm", profile.Llvm);
    profile.LlvmThreads = Unsigned(acceleration, "threads", 0);
    profile.CacheMiB = Unsigned(acceleration, "cache_mib", profile.CacheMiB);
    return profile;
}
} // namespace

State ParseSettings(std::string_view source)
{
    const auto root = toml::parse(source);
    State state;
    const auto& qemu = Table(root, "qemu");
    state.Tools = {Text(qemu, "emulator"), Text(qemu, "image_tool"), Text(qemu, "firmware"),
                   Setting<std::string>(qemu, "build", std::string{})};
    const auto& app = Table(root, "app");
    const auto theme = Setting<std::string>(app, "theme", std::string("system"));
    if (theme == "dark")
    {
        state.Interface.Theme = Appearance::Dark;
    }
    else if (theme == "light")
    {
        state.Interface.Theme = Appearance::Light;
    }
    else if (theme != "system")
    {
        Fail(LFmt(IDS_InvalidSetting, L"app.theme"));
    }
    const auto tag = Text(app, "language", L"system");
    const auto language = std::ranges::find(InterfaceLanguages, tag, &LanguageInfo::Tag);
    if (language == InterfaceLanguages.end())
    {
        Fail(LFmt(IDS_InvalidSetting, L"app.language"));
    }
    state.Interface.Language = static_cast<UiLanguage>(language - InterfaceLanguages.begin());
    state.Interface.CloseOnShutdown = Setting<bool>(app, "close_on_shutdown", state.Interface.CloseOnShutdown);
    state.Interface.DefaultDisplay = ReadDisplaySettings(Table(app, "display"));

    if (const auto profiles = root["profiles"])
    {
        const auto entries = profiles.as_array();
        if (!entries)
        {
            Fail(LFmt(IDS_InvalidSetting, L"profiles"));
        }
        std::set<std::wstring> identifiers;
        for (const auto& node : *entries)
        {
            auto entry = node.as_table();
            if (!entry)
            {
                Fail(LFmt(IDS_InvalidSetting, L"profiles"));
            }
            auto profile = ReadProfile(*entry);
            if (!identifiers.insert(profile.Id).second)
            {
                Fail(LFmt(IDS_InvalidSetting, L"profiles.id"));
            }
            state.Profiles.push_back(std::move(profile));
        }
    }
    return state;
}

std::string FormatSettings(const State& state)
{
    const auto appearance = state.Interface.Theme == Appearance::Dark    ? "dark"
                            : state.Interface.Theme == Appearance::Light ? "light"
                                                                         : "system";
    toml::table display;
    WriteDisplaySettings(display, state.Interface.DefaultDisplay);
    toml::table root{{"app", toml::table{{"theme", appearance},
                                         {"language", Utf8(LanguageInfoFor(state.Interface.Language).Tag)},
                                         {"close_on_shutdown", state.Interface.CloseOnShutdown},
                                         {"display", std::move(display)}}},
                     {"qemu", toml::table{{"emulator", Utf8(state.Tools.Qemu.native())},
                                          {"image_tool", Utf8(state.Tools.ImageTool.native())},
                                          {"firmware", Utf8(state.Tools.Firmware.native())},
                                          {"build", state.Tools.ReleaseTag}}}};
    if (!state.Profiles.empty())
    {
        toml::array profiles;
        for (const auto& profile : state.Profiles)
        {
            profiles.push_back(WriteProfile(profile));
        }
        root.insert("profiles", std::move(profiles));
    }
    std::ostringstream output;
    output << toml::toml_formatter{root} << '\n';
    return std::move(output).str();
}

State LoadState()
{
    const auto path = SettingsPath();
    if (!Fs::exists(path))
    {
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        Fail(LText(IDS_ReadSettings));
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    try
    {
        return ParseSettings(contents.view());
    }
    catch (const toml::parse_error& error)
    {
        Fail(LFmt(IDS_SettingsSyntax, error.source().begin.line, error.source().begin.column,
                  Wide(error.description())));
    }
}

void SaveState(const State& state)
{
    WriteAtomic(SettingsPath(), FormatSettings(state));
}

bool DiskUsedElsewhere(const State& state, const Profile& profile)
{
    if (profile.Primary.File.empty())
    {
        return false;
    }
    return std::ranges::any_of(state.Profiles, [&](const Profile& other) {
        return other.Id != profile.Id &&
               (std::ranges::any_of(AllDrives(other),
                                    [&](const Drive* d) { return SameFilePath(profile.Primary.File, d->File); }) ||
                SameFilePath(profile.Primary.File, NvramPath(other)) ||
                (other.Serial.Mode == L"file" && SameFilePath(profile.Primary.File, other.Serial.File)) ||
                (other.DebugPort.Mode == L"file" && SameFilePath(profile.Primary.File, other.DebugPort.File)));
    });
}

void SaveProfileRemoval(const State& previous, const State& updated, const Fs::path& disk)
{
    if (disk.empty())
    {
        SaveState(updated);
        return;
    }

    const auto path = Fs::absolute(disk);
    Handle file(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file)
    {
        WinError(LText(IDS_RemoveDiskError));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file.Get(), FileAttributeTagInfo, &attributes, sizeof(attributes)))
    {
        WinError(LText(IDS_RemoveDiskError));
    }
    if (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
    {
        Fail(LText(IDS_RemoveDiskFileOnly));
    }
    if (attributes.FileAttributes & FILE_ATTRIBUTE_READONLY)
    {
        Fail(LText(IDS_RemoveDiskReadOnly));
    }

    FILE_DISPOSITION_INFO disposition{TRUE};
    SaveState(updated);
    if (!SetFileInformationByHandle(file.Get(), FileDispositionInfo, &disposition, sizeof(disposition)))
    {
        const auto error = GetLastError();
        SaveState(previous);
        WinError(LText(IDS_RemoveDiskError), error);
    }
}
} // namespace Ia64
