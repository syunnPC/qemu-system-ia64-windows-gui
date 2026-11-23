// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Devices.h"
#include "Localization.h"
#include "Media.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <sstream>

namespace Ia64
{
using namespace std::string_literals;

namespace
{
std::pair<std::wstring, std::wstring> FirstWord(std::wstring_view line)
{
    const auto trimmed = Trim(line);
    const auto length = trimmed.find_first_of(L" \t");
    return {trimmed.substr(0, length), length == std::wstring::npos ? L"" : Trim(trimmed.substr(length))};
}

template <typename Action> void ForLines(const ProcessResult& process, Action action)
{
    std::wistringstream input(Wide(process.Output + "\n" + process.Errors));
    for (std::wstring line; std::getline(input, line);)
    {
        action(Trim(line));
    }
}

std::set<std::wstring> Properties(const ProcessResult& process)
{
    std::set<std::wstring> result;
    ForLines(process, [&](const std::wstring& line) {
        const auto nameEnd = line.find(L"=<");
        if (nameEnd != std::wstring::npos && line.substr(0, nameEnd).find(L' ') == std::wstring::npos)
        {
            result.insert(line.substr(0, nameEnd));
        }
    });
    return result;
}

void AddVideo(std::vector<std::wstring>& args, const Profile& profile, const Machine& machine)
{
    const auto vga = EffectiveVga(profile, machine);
    const bool hasOverrides = profile.Width || profile.Height || profile.MaxWidth || profile.MaxHeight ||
                              profile.VramMiB || !profile.AtiModel.empty();
    // A VGA global can suppress QEMU's implicit adapter. Name the device whenever we configure it.
    if (!profile.Vga.empty() || hasOverrides)
    {
        args.insert(args.end(), {L"-vga", std::wstring(vga)});
    }
    const auto device = vga == L"ati" ? L"ati-vga" : vga == L"quadro2" ? L"nvidia-quadro2" : L"VGA";
    const auto property = [&](std::wstring_view name, unsigned value) {
        if (value)
        {
            args.insert(args.end(), {L"-global", std::format(L"{}.{}={}", device, name, value)});
        }
    };
    property(L"xres", profile.Width);
    property(L"yres", profile.Height);
    property(L"xmax", profile.MaxWidth);
    property(L"ymax", profile.MaxHeight);
    property(L"vgamem_mb", profile.VramMiB);
    if (!profile.AtiModel.empty())
    {
        args.insert(args.end(), {L"-global", L"ati-vga.model=" + profile.AtiModel});
    }
}
} // namespace

void ValidateEngine(const Engine& engine)
{
    if (!IsFile(engine.Qemu) || !IsFile(engine.ImageTool) || !IsFile(engine.Firmware))
    {
        Fail(LText(IDS_PathsRequired));
    }
}

const Machine* Capabilities::Find(std::wstring_view name) const
{
    const auto found = std::ranges::find(Machines, name, &Machine::Name);
    return found == Machines.end() ? nullptr : &*found;
}

std::wstring Capabilities::CanonicalCpu(std::wstring_view cpu) const
{
    const auto alias = CpuAliases.find(std::wstring(cpu));
    return alias == CpuAliases.end() ? std::wstring(cpu) : alias->second;
}

Capabilities ProbeEngine(const Engine& engine)
{
    ValidateEngine(engine);
    Capabilities result;
    const auto version = Run(engine.Qemu, std::array{L"--version"s});
    RequireSuccess(version, LText(IDS_VersionRead));
    result.Version = Wide(version.Output.substr(0, version.Output.find_first_of("\r\n")));
    RequireSuccess(Run(engine.ImageTool, std::array{L"--version"s}), LText(IDS_ImageToolCheck));

    Listener listener;
    Process process(engine.Qemu, std::array{L"-machine"s, L"none"s, L"-display"s, L"none"s, L"-serial"s, L"none"s,
                                            L"-monitor"s, L"none"s, L"-qmp"s, QmpAddress(listener)});
    process.CloseInput();
    Qmp qmp(listener.Accept(process));
    const auto machines = qmp.Call("query-machines");
    for (const auto& value : machines)
    {
        if (!value.contains("default-cpu-type"))
        {
            continue;
        }
        auto cpu = value.at("default-cpu-type").get<std::string>();
        constexpr std::string_view suffix = "-ia64-cpu";
        if (cpu.ends_with(suffix))
        {
            cpu.resize(cpu.size() - suffix.size());
        }
        result.Machines.push_back({Wide(value.at("name").get<std::string>()),
                                   Wide(cpu),
                                   value.at("cpu-max").get<unsigned>(),
                                   {},
                                   {},
                                   {},
                                   {}});
    }
    const auto types = qmp.Call("qom-list-types", {{"implements", "accel"}, {"abstract", false}});
    if (std::ranges::any_of(types, [](const Json& t) { return t.at("name") == "tcg-llvm-accel"; }))
    {
        const auto props = qmp.Call("qom-list-properties", {{"typename", "tcg-llvm-accel"}});
        for (const auto& prop : props)
        {
            if (prop.at("name") == "llvm-thread")
            {
                result.LlvmThreads = prop.at("default-value").get<unsigned>();
            }
        }
    }
    const auto backends = Run(engine.Qemu, std::array{L"-machine"s, L"none"s, L"-netdev"s, L"help"s});
    RequireSuccess(backends, LText(IDS_HardwareLoading));
    result.TapAdapters = TapAdapterNames();
    ForLines(backends, [&](const auto& line) {
        if (line == L"user" || line == L"socket" || line == L"tap")
        {
            result.NetBackends.push_back(line);
        }
    });
    qmp.Call("quit");
    std::ranges::sort(result.Machines, {}, &Machine::Name);
    if (!result.Find(L"itanium2-vpc"))
    {
        Fail(LFmt(IDS_MachineUnavailable, L"itanium2-vpc"));
    }
    for (auto& machine : result.Machines)
    {
        const auto video = Run(engine.Qemu, std::array{L"-machine"s, machine.Name, L"-vga"s, L"help"s});
        RequireSuccess(video, LText(IDS_VgaRead));
        ForLines(video, [&](const auto& line) {
            const auto [name, detail] = FirstWord(line);
            if (!name.empty())
            {
                machine.VgaTypes.push_back(name);
            }
            if (detail.ends_with(L"(default)"))
            {
                machine.DefaultVga = name;
            }
        });
    }

    const auto cpuHelp = Run(engine.Qemu, std::array{L"-cpu"s, L"help"s});
    RequireSuccess(cpuHelp, LText(IDS_CpuRead));
    ForLines(cpuHelp, [&](const std::wstring& line) {
        if (line.empty() || line.ends_with(L":"))
        {
            return;
        }
        const auto [name, detail] = FirstWord(line);
        if (detail.starts_with(L"(alias for ") && detail.ends_with(L")"))
        {
            result.CpuAliases.emplace(name, detail.substr(11, detail.size() - 12));
        }
        result.Cpus.push_back(name);
    });
    const auto accelHelp = Run(engine.Qemu, std::array{L"-accel"s, L"help"s});
    RequireSuccess(accelHelp, LText(IDS_AccelRead));
    ForLines(accelHelp, [&](const std::wstring& line) { result.Llvm |= line == L"tcg-llvm"; });
    for (const auto& [vga, device] :
         std::array{std::pair{L"ati", L"ati-vga,help"}, std::pair{L"quadro2", L"nvidia-quadro2,help"},
                    std::pair{L"std", L"VGA,help"}})
    {
        const auto help =
            Run(engine.Qemu, std::array{L"-machine"s, L"itanium2-vpc"s, L"-device"s, std::wstring(device)});
        if (help.ExitCode == 0)
        {
            result.VideoProperties.emplace(vga, Properties(help));
        }
    }
    return result;
}

Machine ProbeMachine(const Engine& engine, Machine machine)
{
    const auto options = Run(engine.Qemu, std::array{L"-machine"s, machine.Name + L",help"});
    RequireSuccess(options, LText(IDS_MachinePropertiesRead));
    machine.Properties = Properties(options);
    Listener listener;
    Process process(engine.Qemu,
                    std::array{L"-machine"s, machine.Name, L"-preconfig"s, L"-display"s, L"none"s, L"-serial"s,
                               L"none"s, L"-monitor"s, L"none"s, L"-qmp"s, QmpAddress(listener)});
    process.CloseInput();
    Qmp qmp(listener.Accept(process));
    for (const auto& prop : qmp.Call("qom-list", {{"path", "/machine"}}))
    {
        machine.Properties.emplace(Wide(prop.at("name").get<std::string>()));
    }
    for (const auto& [name, value] :
         {std::pair{L"alat", &machine.DefaultAlat}, std::pair{L"firmware-base", &machine.DefaultFirmwareBase}})
    {
        if (machine.Properties.contains(name))
        {
            *value = Wide(qmp.Call("qom-get", {{"path", "/machine"}, {"property", Utf8(name)}}).get<std::string>());
        }
    }
    ProbeDeviceModels(engine, machine);
    for (const auto name : {L"i8042", L"usb", L"pcie", L"firmware-ide-dma", L"usb1"})
    {
        if (machine.Properties.contains(name))
        {
            const auto value = qmp.Call("qom-get", {{"path", "/machine"}, {"property", Utf8(name)}});
            machine.BooleanDefaults.emplace(name, value.get<bool>());
        }
    }
    qmp.Call("quit");
    return machine;
}

std::vector<std::wstring> Buses(std::wstring_view machine)
{
    if (machine == L"itanium2-vpc" || machine == L"itanium2-vpc-iommu")
    {
        return {L"SCSI", L"AHCI"};
    }
    if (machine == L"hp-zx-pcie-test" || machine == L"x-ia64-zx6000-efi-test")
    {
        return {L"lsi53c895a", L"mptsas1068", L"virtio-scsi-pci", L"ich9-ahci"};
    }
    if (machine == L"hp-zx2000")
    {
        return {L"IDE"};
    }
    if (machine == L"hp-i2000" || machine == L"x-ia64-i2000-efi-test" || machine == L"hp-zx6000")
    {
        return {L"SCSI", L"IDE"};
    }
    return {L"SCSI"};
}

std::pair<unsigned, unsigned> RamRange(std::wstring_view machine)
{
    if (machine == L"hp-rx2660" || machine == L"hp-zx-pcie-test")
    {
        return {1024, 32768};
    }
    if (machine == L"hp-zx2000")
    {
        return {512, 8192};
    }
    if (machine == L"hp-i2000" || machine == L"x-ia64-i2000-efi-test")
    {
        return {2048, 16384};
    }
    if (machine == L"hp-zx6000" || machine == L"x-ia64-zx6000-efi-test")
    {
        return {512, 24576};
    }
    return {128, 1048576};
}

unsigned DefaultRam(std::wstring_view machine)
{
    if (machine == L"hp-rx2660" || machine == L"hp-zx-pcie-test")
    {
        return 8192;
    }
    return machine == L"hp-zx2000" ? 1024u : 2048u;
}

std::wstring_view EffectiveVga(const Profile& profile, const Machine& machine)
{
    return profile.Vga.empty() ? machine.DefaultVga : profile.Vga;
}

std::wstring_view DefaultAti(std::wstring_view machine)
{
    if (machine == L"hp-rx2660" || machine == L"hp-zx-pcie-test")
    {
        return L"es1000";
    }
    if (machine == L"hp-zx2000" || machine == L"hp-zx6000")
    {
        return L"rv100";
    }
    return L"rage128p";
}

unsigned DefaultVram(std::wstring_view adapter)
{
    return adapter == L"quadro2" ? 64 : 16;
}

unsigned MaximumVram(std::wstring_view adapter)
{
    // Quadro2 validates 16–128 MiB; ATI and std use vga_common_init's 512 MiB limit.
    return adapter == L"quadro2" ? 128 : 512;
}

unsigned MinimumVram(const Profile& profile, const Machine& machine)
{
    const auto adapter = EffectiveVga(profile, machine);
    const auto model = profile.AtiModel.empty() ? DefaultAti(machine.Name) : profile.AtiModel;
    const auto deviceMinimum = (adapter == L"quadro2" || (adapter == L"ati" && model != L"rage128p")) ? 16u : 1u;
    const auto frameMiB = [](unsigned width, unsigned height) {
        constexpr uint64_t MiB = 1024 * 1024;
        return static_cast<unsigned>((uint64_t{width} * height * 4 + MiB - 1) / MiB);
    };
    return std::bit_ceil(std::max({deviceMinimum,
                                   frameMiB(profile.Width ? profile.Width : DefaultDisplayWidth,
                                            profile.Height ? profile.Height : DefaultDisplayHeight),
                                   frameMiB(profile.MaxWidth, profile.MaxHeight)}));
}

void Drive::SelectImage(Fs::path path)
{
    if (Media != L"cdrom" && IsOpticalImage(path))
    {
        Media = L"cdrom";
    }
    File = std::move(path);
    Format.clear();
}

void Drive::UpdateImage(Fs::path path, bool newQcow2)
{
    if (path.empty())
    {
        File.clear();
        Format.clear();
    }
    else if (newQcow2)
    {
        if (path != File)
        {
            Media = L"disk";
        }
        File = std::move(path);
        Format = L"qcow2";
    }
    else if (path != File)
    {
        SelectImage(std::move(path));
    }
}

bool SameFilePath(const Fs::path& first, const Fs::path& second)
{
    if (first.empty() || second.empty())
    {
        return false;
    }
    const auto a = Fs::absolute(first).lexically_normal();
    const auto b = Fs::absolute(second).lexically_normal();
    std::error_code error;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL || Fs::equivalent(a, b, error);
}

Fs::path NvramPath(const Profile& profile)
{
    return profile.Nvram.empty() ? DataDirectory() / L"VMs" / profile.Id / L"nvram" : profile.Nvram;
}

void ValidateProfile(const Profile& profile, const Machine& machine, const Capabilities& capabilities,
                     const Engine& engine)
{
    ValidateDevices(profile, machine, capabilities, engine);
    if (Trim(profile.Name).empty() || profile.Name.size() > 100)
    {
        throw ProfileValidationError(ProfileSection::General, LText(IDS_NameLimit));
    }
    if (!profile.Sockets || !profile.Cores || !profile.Threads || profile.Sockets > machine.MaxCpus ||
        profile.Cores > machine.MaxCpus || profile.Threads > machine.MaxCpus ||
        uint64_t{profile.Sockets} * profile.Cores * profile.Threads > machine.MaxCpus)
    {
        throw ProfileValidationError(ProfileSection::Processor, LFmt(IDS_CpuLimit, machine.MaxCpus));
    }
    if (!profile.Cpu.empty() && std::ranges::find(capabilities.Cpus, profile.Cpu) == capabilities.Cpus.end())
    {
        throw ProfileValidationError(ProfileSection::Processor, LText(IDS_CpuUnavailable));
    }
    const auto [minRam, machineMaxRam] = RamRange(machine.Name);
    const auto maxRam = std::min(machineMaxRam, InstalledRamMiB());
    if (maxRam < minRam)
    {
        throw ProfileValidationError(ProfileSection::Memory, LFmt(IDS_HostRamTooSmall, minRam));
    }
    if (profile.RamMiB < minRam || profile.RamMiB > maxRam)
    {
        throw ProfileValidationError(ProfileSection::Memory, LFmt(IDS_RamRange, minRam, maxRam));
    }
    const auto buses = Buses(machine.Name);
    if (std::ranges::find(buses, profile.Bus) == buses.end())
    {
        throw ProfileValidationError(ProfileSection::Controller, LText(IDS_BusUnavailable));
    }
    if (profile.Optical.Connected() && !profile.Optical.File.empty() && !IsFile(profile.Optical.File))
    {
        throw ProfileValidationError(ProfileSection::Optical, LText(IDS_IsoMissing));
    }
    if (!profile.Nvram.empty())
    {
        const auto& path = profile.Nvram;
        if (!machine.Properties.contains(L"nvram"))
        {
            throw ProfileValidationError(ProfileSection::Advanced, LFmt(IDS_OptionUnavailable, L"NVRAM"));
        }
        const auto attributes = GetFileAttributesW(path.c_str());
        const DWORD attributeError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (!path.is_absolute() || !path.has_filename() || !Fs::is_directory(path.parent_path()) ||
            (attributes == INVALID_FILE_ATTRIBUTES
                 ? attributeError != ERROR_FILE_NOT_FOUND
                 : (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) != 0))
        {
            throw ProfileValidationError(ProfileSection::Advanced, LText(IDS_InvalidNvram));
        }
        if (SameFilePath(path, profile.Primary.File) || SameFilePath(path, profile.Optical.File) ||
            SameFilePath(path, engine.Firmware) || SameFilePath(path, engine.Qemu) ||
            SameFilePath(path, engine.ImageTool))
        {
            throw ProfileValidationError(ProfileSection::Advanced, LText(IDS_NvramConflict));
        }
    }
    if (!profile.Vga.empty() && std::ranges::find(machine.VgaTypes, profile.Vga) == machine.VgaTypes.end())
    {
        throw ProfileValidationError(ProfileSection::Display, LText(IDS_VgaUnavailable));
    }
    const auto vga = EffectiveVga(profile, machine);
    const auto props = capabilities.VideoProperties.find(std::wstring(vga));
    const auto available = [&](const std::wstring& key) {
        return props != capabilities.VideoProperties.end() && props->second.contains(key);
    };
    if ((profile.Width && !available(L"xres")) || (profile.Height && !available(L"yres")) ||
        (profile.MaxWidth && !available(L"xmax")) || (profile.MaxHeight && !available(L"ymax")) ||
        (profile.VramMiB && !available(L"vgamem_mb")) || (!profile.AtiModel.empty() && !available(L"model")))
    {
        throw ProfileValidationError(ProfileSection::Display, LText(IDS_VideoOptionsUnavailable));
    }
    if ((profile.Width == 0) != (profile.Height == 0) || (profile.MaxWidth == 0) != (profile.MaxHeight == 0) ||
        profile.Width % 8 || profile.MaxWidth % 8 || (profile.MaxWidth && profile.Width > profile.MaxWidth) ||
        (profile.MaxHeight && profile.Height > profile.MaxHeight) ||
        std::max(profile.Width, profile.MaxWidth) > MaxDisplayWidth ||
        std::max(profile.Height, profile.MaxHeight) > MaxDisplayHeight)
    {
        throw ProfileValidationError(ProfileSection::Display, LText(IDS_VideoSizeInvalid));
    }
    const auto minimumVram = MinimumVram(profile, machine);
    if (profile.VramMiB &&
        (!std::has_single_bit(profile.VramMiB) || profile.VramMiB < minimumVram || profile.VramMiB > MaximumVram(vga)))
    {
        throw ProfileValidationError(ProfileSection::Display, LFmt(IDS_VramRange, minimumVram, MaximumVram(vga)));
    }
    if (profile.Llvm && (!capabilities.Llvm || !profile.CacheMiB))
    {
        throw ProfileValidationError(ProfileSection::Advanced, LText(IDS_LlvmInvalid));
    }
    for (const auto& [name, value] : profile.MachineOptions)
    {
        if (!machine.Properties.contains(name))
        {
            throw ProfileValidationError(ProfileSection::Advanced, LFmt(IDS_OptionUnavailable, name));
        }
    }
}

std::vector<VmSnapshot> VmSnapshots(const Json& image)
{
    std::vector<VmSnapshot> result;
    if (const auto entries = image.find("snapshots"); entries != image.end())
    {
        for (const auto& item : *entries)
        {
            if (item.at("vm-state-size").get<uint64_t>() > 0)
            {
                result.push_back({item.at("name").get<std::string>(), item.at("id").get<std::string>(),
                                  item.at("date-sec").get<int64_t>(), item.at("date-nsec").get<int>()});
            }
        }
    }
    std::ranges::stable_sort(result, std::greater{},
                             [](const VmSnapshot& item) { return std::pair{item.Date, item.Nanoseconds}; });
    return result;
}

DiskInfo InspectDisk(const Engine& engine, const Fs::path& path)
{
    if (!IsFile(path))
    {
        Fail(LText(IDS_DiskMissing));
    }
    const auto result = Run(engine.ImageTool, std::array{L"info"s, L"--output=json"s, Fs::absolute(path).native()});
    RequireSuccess(result, LText(IDS_DiskRead));
    const auto image = Json::parse(result.Output);
    return {Wide(image.at("format").get<std::string>()), VmSnapshots(image)};
}

void CreateDisk(const Engine& engine, const Fs::path& path, unsigned mebibytes)
{
    if (path.empty() || !mebibytes || mebibytes > 65536u * 1024)
    {
        Fail(LText(IDS_DiskRange));
    }
    const auto target = Fs::absolute(path);
    Fs::create_directories(target.parent_path());
    const auto temporary = target.parent_path() / (L".disk-" + NewId() + L".qcow2");
    try
    {
        const std::array create{L"create"s, L"-f"s, L"qcow2"s, temporary.native(), std::format(L"{}M", mebibytes)};
        RequireSuccess(Run(engine.ImageTool, create, 60s), LText(IDS_DiskCreateOp));
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            WinError(LText(IDS_DiskSaveOp));
        }
    }
    catch (...)
    {
        std::error_code error;
        Fs::remove(temporary, error);
        throw;
    }
}

ProcessCommand LaunchCommand(const Engine& engine, const Profile& profile, const Machine& machine, const Listener& qmp,
                             const Listener* video)
{
    std::wstring configuration = machine.Name;
    if (machine.Properties.contains(L"nvram"))
    {
        if (video)
        {
            configuration += L",nvram=" + QemuValue(NvramPath(profile).native());
        }
        else
        {
            configuration += L",nvram=none";
        }
    }
    for (const auto& [property, value] : profile.MachineOptions)
    {
        configuration += std::format(L",{}={}", property, value ? L"on" : L"off");
    }

    if (!profile.Alat.empty())
    {
        configuration += L",alat=" + profile.Alat;
    }
    if (!profile.FirmwareBase.empty())
    {
        configuration += L",firmware-base=" + profile.FirmwareBase;
    }
    const auto cpus = uint64_t{profile.Sockets} * profile.Cores * profile.Threads;
    const auto smp =
        std::format(L"cpus={},sockets={},cores={},threads={}", cpus, profile.Sockets, profile.Cores, profile.Threads);
    auto accel = std::wstring(profile.Llvm ? L"tcg-llvm" : L"tcg") + (cpus > 1 ? L",thread=multi" : L",thread=single");
    if (profile.Llvm)
    {
        accel += std::format(L",cachesize={}M", profile.CacheMiB);
    }

    if (profile.Llvm && profile.LlvmThreads)
    {
        accel += std::format(L",llvm-thread={}", profile.LlvmThreads);
    }
    std::vector<std::wstring> args{L"-name",    QemuValue(profile.Name),
                                   L"-machine", configuration,
                                   L"-bios",    Fs::absolute(engine.Firmware).native(),
                                   L"-accel",   accel,
                                   L"-smp",     smp,
                                   L"-m",       std::format(L"{}M", profile.RamMiB),
                                   L"-display", L"none",
                                   L"-monitor", L"none",
                                   L"-qmp",     QmpAddress(qmp),
                                   L"-S"};
    const auto share = engine.Qemu.parent_path() / L"share";
    if (Fs::is_directory(share))
    {
        args.insert(args.end(), {L"-L", Fs::absolute(share).native()});
    }
    if (!profile.Cpu.empty())
    {
        args.insert(args.end(), {L"-cpu", profile.Cpu});
    }
    if (video && EffectiveVga(profile, machine) != L"none")
    {
        args.insert(args.end(), {L"-vnc", std::format(L"127.0.0.1:{},reverse=on", video->Port())});
    }

    AddDevices(args, profile, video == nullptr);
    AddVideo(args, profile, machine);
    return {engine.Qemu, args};
}

void ValidateBoot(const Engine& engine, const Profile& profile, const Machine& machine)
{
    Listener listener;
    Process process(LaunchCommand(engine, profile, machine, listener, nullptr));
    process.CloseInput();
    try
    {
        Qmp qmp(listener.Accept(process));
        qmp.Call("query-status");
        if (!profile.Network.Backend.empty() && profile.Network.Backend != L"none")
        {
            const auto network =
                qmp.Call("human-monitor-command", {{"command-line", "info network"}}).get<std::string>();
            if (!network.contains("\\ lan0:"))
            {
                Fail(LText(IDS_CableError));
            }
        }
        qmp.Call("quit");
    }
    catch (const std::exception& error)
    {
        const auto diagnostic = Trim(Wide(process.ReadErrors()));
        if (diagnostic.empty())
        {
            throw;
        }
        Fail(Wide(error.what()) + L"\n" + diagnostic);
    }
}
} // namespace Ia64
