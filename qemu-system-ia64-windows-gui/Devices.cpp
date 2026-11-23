// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Devices.h"
#include "Localization.h"

#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <format>
#include <sstream>

namespace Ia64
{
using namespace std::string_literals;
namespace
{
template <class P> auto DrivesOf(P& profile)
{
    std::vector<decltype(&profile.Primary)> drives{&profile.Primary, &profile.Optical};
    for (auto& drive : profile.Drives)
    {
        drives.push_back(&drive);
    }
    return drives;
}

bool Onboard(std::wstring_view model)
{
    return model == L"SCSI" || model == L"SCSI 2" || model == L"IDE" || model == L"AHCI";
}

bool ParallelIde(std::wstring_view model)
{
    return model == L"IDE" || model == L"cmd646-ide" || model == L"cmd649-ide";
}

bool ValidId(const std::wstring& id)
{
    return !id.empty() && id.size() < 64 && id.front() >= L'a' && id.front() <= L'z' &&
           id.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == id.npos;
}

bool SafeHost(const std::wstring& host)
{
    return !host.empty() &&
           host.find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.:-_") == host.npos;
}

[[noreturn]] void DeviceError(int label, ProfileSection section = ProfileSection::Controller)
{
    throw ProfileValidationError(section, LText(label));
}

void AddPort(std::vector<std::wstring>& args, std::wstring id, const CharPort& port, bool test)
{
    const bool serial = id == L"serial0";
    if (port.Mode == L"none")
    {
        if (serial)
        {
            args.insert(args.end(), {L"-serial", L"none"});
        }
        return;
    }
    std::wstring backend;
    if (port.Mode == L"window")
    {
        backend = L"ringbuf,size=1048576";
    }
    else if (test)
    {
        backend = L"null";
    }
    else if (port.Mode == L"file")
    {
        backend = L"file,path=" + QemuValue(Fs::absolute(port.File).native()) + L",append=on";
    }
    else
    {
        backend = std::format(L"socket,host={},port={},server={}", port.Host, port.Port,
                              port.Mode == L"listen" ? L"on" : L"off");
        backend += port.Mode == L"listen" ? L",wait=off" : L",reconnect-ms=1000";
    }
    args.insert(args.end(),
                {L"-chardev", backend + L",id=" + id, serial ? L"-serial" : L"-debug-port", L"chardev:" + id});
}

void AddNic(std::vector<std::wstring>& args, const Profile& profile, bool validation)
{
    const auto& nic = profile.Network;
    if (nic.Backend.empty())
    {
        return;
    }
    if (nic.Backend == L"none")
    {
        args.insert(args.end(), {L"-nic", L"none"});
        return;
    }
    std::wstring net;
    if (validation && nic.Backend != L"user")
    {
        net = L"socket,listen=127.0.0.1:0";
    }
    else if (nic.Backend == L"user")
    {
        net = L"user";
        std::wistringstream rules(nic.Forward);
        for (std::wstring rule; std::getline(rules, rule, L';');)
        {
            if (!(rule = Trim(rule)).empty())
            {
                net += L",hostfwd=" + QemuValue(rule);
            }
        }
    }
    else if (nic.Backend == L"tap")
    {
        net = L"tap,ifname=" + QemuValue(nic.Interface);
    }
    else
    {
        net = std::format(L"socket,{}={}:{}", nic.Backend, nic.Host, nic.Port);
    }
    if (!nic.Model.empty())
    {
        net += L",model=" + nic.Model;
        if (IsVpc(profile.Machine) && !nic.BootRom)
        {
            args.insert(args.end(), {L"-global", nic.Model + L".romfile="});
        }
    }
    if (!nic.Mac.empty())
    {
        net += L",mac=" + nic.Mac;
    }
    args.insert(args.end(), {L"-nic", net + L",id=lan0"});
}
} // namespace

bool IsVpc(std::wstring_view name)
{
    return name.starts_with(L"itanium") || name == L"ia64-vpc";
}

std::wstring_view DefaultNicModel(std::wstring_view machine)
{
    if (IsVpc(machine) || machine == L"hp-zx2000")
    {
        return L"e1000";
    }
    if (machine == L"hp-zx6000")
    {
        return L"i82550";
    }
    if (machine == L"hp-i2000")
    {
        return L"i82559c";
    }
    if (machine == L"hp-rx2660")
    {
        return L"bcm5704";
    }
    return {};
}
std::vector<Drive*> AllDrives(Profile& profile)
{
    return DrivesOf(profile);
}
std::vector<const Drive*> AllDrives(const Profile& profile)
{
    return DrivesOf(profile);
}

const Drive* SnapshotDrive(const Profile& profile)
{
    for (const auto drive : AllDrives(profile))
    {
        if (drive->Connected() && drive->Format == L"qcow2" && !drive->Protected())
        {
            return drive;
        }
    }
    return nullptr;
}

std::vector<VmSnapshot> InspectStorage(Profile& profile, const Engine& engine, bool newPrimary)
{
    std::vector<VmSnapshot> snapshots;
    bool foundStateDrive = false;
    for (auto drive : AllDrives(profile))
    {
        if (!drive->Connected() || drive->File.empty())
        {
            drive->Format.clear();
            continue;
        }
        DiskInfo info =
            newPrimary && drive == &profile.Primary ? DiskInfo{L"qcow2", {}} : InspectDisk(engine, drive->File);
        if (info.Format != L"raw" && info.Format != L"qcow2")
        {
            DeviceError(IDS_BadFormat);
        }
        drive->Format = std::move(info.Format);
        if (!foundStateDrive && drive->Format == L"qcow2" && !drive->Protected())
        {
            snapshots = std::move(info.Snapshots);
            foundStateDrive = true;
        }
    }
    return snapshots;
}

std::wstring ControllerModel(const Profile& profile, std::wstring_view id)
{
    if (id == L"onboard")
    {
        return profile.Bus;
    }
    const auto it = std::ranges::find(profile.Controllers, id, &StorageController::Id);
    if (it == profile.Controllers.end())
    {
        DeviceError(IDS_BadDrive);
    }
    return it->Model;
}

bool IsScsi(std::wstring_view model)
{
    return !ParallelIde(model) && model != L"AHCI" && model != L"ich9-ahci";
}

unsigned ControllerAddress(const Profile& profile, const StorageController& controller)
{
    std::set<unsigned> occupied;
    if (!Onboard(profile.Bus))
    {
        occupied.insert(16);
        if (controller.Id == L"onboard")
        {
            return 16;
        }
    }
    for (const auto& c : profile.Controllers)
    {
        if (!Onboard(c.Model) && c.PciSlot && (c.PciSlot > 31 || !occupied.insert(c.PciSlot).second))
        {
            DeviceError(IDS_BadDrive);
        }
    }
    if (controller.PciSlot)
    {
        return controller.PciSlot;
    }
    // Firmware assigns the onboard devices to low slots. Keep expansion HBAs at stable BDFs.
    for (const auto& c : profile.Controllers)
    {
        if (Onboard(c.Model) || c.PciSlot)
        {
            continue;
        }
        unsigned slot = 16;
        while (slot < 32 && occupied.contains(slot))
        {
            ++slot;
        }
        if (slot == 32)
        {
            slot = 8;
            while (slot < 16 && occupied.contains(slot))
            {
                ++slot;
            }
            if (slot == 16)
            {
                DeviceError(IDS_BadDrive);
            }
        }
        occupied.insert(slot);
        if (c.Id == controller.Id)
        {
            return slot;
        }
    }
    DeviceError(IDS_BadDrive);
}

unsigned PortCount(std::wstring_view model)
{
    if (ParallelIde(model))
    {
        return 4;
    }
    if (model == L"lsi53c810")
    {
        return 8;
    }
    if (!IsScsi(model))
    {
        return 6;
    }
    return 16;
}

std::wstring DriveBus(std::wstring_view machine, std::wstring_view model, std::wstring_view controller, unsigned slot)
{
    if (model == L"SCSI")
    {
        return (machine == L"hp-i2000" || machine == L"x-ia64-i2000-efi-test") ? L"isp12160-scsi.0" : L"scsi.0";
    }
    if (model == L"SCSI 2")
    {
        return L"scsi.1";
    }
    if (model == L"IDE")
    {
        return std::format(L"ide.{}", slot / 2);
    }
    if (model == L"AHCI")
    {
        return std::format(L"ide.{}", slot);
    }
    if (ParallelIde(model))
    {
        return std::format(L"{}.{}", controller, slot / 2);
    }
    return std::format(L"{}.{}", controller, IsScsi(model) ? 0 : slot);
}

Json DriveDevice(const Profile& profile, const Drive& drive)
{
    const auto model = ControllerModel(profile, drive.Controller);
    auto driver = std::wstring(IsScsi(model) ? L"scsi-" : L"ide-") + (drive.Media == L"cdrom" ? L"cd" : L"hd");
    Json device{{"driver", Utf8(driver)},
                {"id", Utf8(L"dev-" + drive.Id)},
                {"drive", Utf8(drive.Id)},
                {"bus", Utf8(DriveBus(profile.Machine, model, drive.Controller, drive.Slot))}};
    if (IsScsi(model))
    {
        device["scsi-id"] = drive.Slot;
    }
    else
    {
        device["unit"] = ParallelIde(model) ? drive.Slot % 2 : 0;
    }
    if (drive.BootIndex >= 0)
    {
        device["bootindex"] = drive.BootIndex;
    }
    return device;
}

bool ValidFirmwareBase(std::wstring_view text)
{
    if (text.empty() || text == L"auto")
    {
        return true;
    }
    const bool hex = text.starts_with(L"0x") || text.starts_with(L"0X");
    const auto value = Utf8(text.substr(hex ? 2 : 0));
    uint64_t address{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), address, hex ? 16 : 10);
    return ec == std::errc{} && end == value.data() + value.size() && address >= 0x100000 && address % 8192 == 0;
}

std::vector<std::wstring> TapAdapterNames()
{
    ULONG bytes{};
    constexpr ULONG flags = GAA_FLAG_INCLUDE_ALL_INTERFACES | GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST |
                            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    std::vector<unsigned char> storage;
    ULONG status;
    do
    {
        storage.resize(bytes);
        status = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()), &bytes);
    } while (status == ERROR_BUFFER_OVERFLOW);
    std::vector<std::wstring> names;
    if (status != NO_ERROR || storage.empty())
    {
        return names;
    }
    for (auto adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()); adapter; adapter = adapter->Next)
    {
        if (!adapter->AdapterName || !adapter->FriendlyName)
        {
            continue;
        }
        const auto device = Wide(adapter->AdapterName) + L".tap";
        std::array<wchar_t, 1024> target{};
        if (QueryDosDeviceW(device.c_str(), target.data(), static_cast<DWORD>(target.size())))
        {
            names.emplace_back(adapter->FriendlyName);
        }
    }
    std::ranges::sort(names);
    return names;
}

void ProbeDeviceModels(const Engine& engine, Machine& machine)
{
    const auto result = Run(engine.Qemu, std::array{L"-machine"s, machine.Name, L"-device"s, L"help"s});
    RequireSuccess(result, LText(IDS_HardwareLoading));
    std::wistringstream input(Wide(result.Output + "\n" + result.Errors));
    bool network = false;
    const bool pci = IsVpc(machine.Name) || machine.Name == L"hp-zx2000" || machine.Name == L"hp-zx6000" ||
                     machine.Name == L"hp-i2000" || machine.Name == L"hp-rx2660" ||
                     machine.Name == L"hp-zx-pcie-test" || machine.Name == L"x-ia64-i2000-efi-test" ||
                     machine.Name == L"x-ia64-zx6000-efi-test";
    for (std::wstring line; std::getline(input, line);)
    {
        line = Trim(line);
        if (line.ends_with(L":"))
        {
            network = line == L"Network devices:";
            continue;
        }
        if (!line.starts_with(L"name \"") || !line.contains(L", bus PCI"))
        {
            continue;
        }
        const auto model = line.substr(6, line.find(L'"', 6) - 6);
        if (pci && (model == L"lsi53c895a" || model == L"lsi53c810" || model == L"lsi53c1030" ||
                    model == L"mptsas1068" || model == L"virtio-scsi-pci" || model == L"virtio-scsi-pci-transitional" ||
                    model == L"virtio-scsi-pci-non-transitional" || model == L"ich9-ahci" || ParallelIde(model)))
        {
            machine.ControllerModels.push_back(model);
        }
        if (network && (IsVpc(machine.Name) || (machine.Name == L"hp-zx2000" && model == L"e1000") ||
                        (machine.Name == L"hp-i2000" && model == L"i82559c") ||
                        (machine.Name == L"hp-zx6000" && (model == L"i82550" || model == L"bcm5701")) ||
                        (machine.Name == L"hp-rx2660" && model == L"bcm5704")))
        {
            machine.NicModels.push_back(model);
        }
    }
}

void ValidateDevices(const Profile& p, const Machine& machine, const Capabilities& caps, const Engine& engine)
{
    const auto unsupported = [&](std::wstring_view name) {
        throw ProfileValidationError(ProfileSection::Advanced, LFmt(IDS_UnavailableProperty, name));
    };
    if (p.DebugPort.Mode != L"none" && !IsVpc(machine.Name))
    {
        unsupported(L"-debug-port");
    }
    if (!p.Alat.empty() && (!machine.Properties.contains(L"alat") || (p.Alat != L"zero" && p.Alat != L"full")))
    {
        unsupported(L"alat");
    }
    if (!p.FirmwareBase.empty() && !machine.Properties.contains(L"firmware-base"))
    {
        unsupported(L"firmware-base");
    }
    if (!ValidFirmwareBase(p.FirmwareBase))
    {
        DeviceError(IDS_BadFirmwareBase, ProfileSection::Advanced);
    }
    if (p.Llvm && p.LlvmThreads && !caps.LlvmThreads)
    {
        unsupported(L"llvm-thread");
    }
    if (p.ForcePs2 && machine.BooleanDefaults.contains(L"i8042") &&
        !(p.MachineOptions.contains(L"i8042") ? p.MachineOptions.at(L"i8042") : machine.BooleanDefaults.at(L"i8042")))
    {
        DeviceError(IDS_Ps2Needed, ProfileSection::Advanced);
    }

    std::set<std::wstring> ids{L"onboard"}, native{p.Bus};
    auto onboard = Buses(machine.Name);
    if (machine.Name == L"hp-zx6000")
    {
        onboard.push_back(L"SCSI 2");
    }
    for (const auto& c : p.Controllers)
    {
        if (!ValidId(c.Id) || !ids.insert(c.Id).second)
        {
            DeviceError(IDS_BadDrive);
        }
        if (Onboard(c.Model))
        {
            if (!std::ranges::contains(onboard, c.Model) || !native.insert(c.Model).second)
            {
                DeviceError(IDS_BadDrive);
            }
        }
        else
        {
            if (!std::ranges::contains(machine.ControllerModels, c.Model))
            {
                DeviceError(IDS_BusUnavailable);
            }
            ControllerAddress(p, c);
        }
    }
    std::set<std::pair<std::wstring, unsigned>> slots;
    std::set<unsigned> boot;
    std::vector<const Drive*> seen;
    for (const auto d : AllDrives(p))
    {
        if (!ValidId(d->Id) || !ids.insert(d->Id).second)
        {
            DeviceError(IDS_BadDrive);
        }
        const auto model = ControllerModel(p, d->Controller);
        if (d->Media != L"disk" && d->Media != L"cdrom")
        {
            DeviceError(IDS_BadDrive);
        }
        if (d->Slot >= PortCount(model) || (IsScsi(model) && d->Slot == 7))
        {
            DeviceError(IDS_BadDrive);
        }
        if (!d->Connected())
        {
            continue;
        }
        if (!slots.emplace(d->Controller, d->Slot).second)
        {
            DeviceError(IDS_DuplicateSlot);
        }
        if (d->BootIndex >= 0 && !boot.insert(d->BootIndex).second)
        {
            DeviceError(IDS_BadDrive);
        }
        if (!IsScsi(model) && d->Media == L"disk" && d->Protected())
        {
            DeviceError(IDS_IdeReadOnly);
        }
        if ((d->DetectZeroes == L"unmap" && d->Discard != L"unmap") ||
            !std::ranges::contains(std::array{L"", L"writeback", L"writethrough", L"none", L"directsync", L"unsafe"},
                                   d->Cache) ||
            !std::ranges::contains(std::array{L"", L"ignore", L"unmap"}, d->Discard) ||
            !std::ranges::contains(std::array{L"", L"off", L"on", L"unmap"}, d->DetectZeroes))
        {
            DeviceError(IDS_BadDrive);
        }
        if (SameFilePath(d->File, NvramPath(p)))
        {
            DeviceError(IDS_NvramConflict);
        }
        for (const auto other : seen)
        {
            if (SameFilePath(d->File, other->File) && (!d->Protected() || !other->Protected()))
            {
                DeviceError(IDS_DuplicateDisk);
            }
        }
        seen.push_back(d);
    }
    const auto portError = [&](int message) { DeviceError(message, ProfileSection::Ports); };
    for (const auto port : {&p.Serial, &p.DebugPort})
    {
        if (port->Mode == L"file")
        {
            if (!port->File.is_absolute() || !Fs::is_directory(port->File.parent_path()) ||
                !port->File.has_filename() || Fs::is_directory(port->File) || SameFilePath(port->File, NvramPath(p)) ||
                SameFilePath(port->File, engine.Firmware) || SameFilePath(port->File, engine.Qemu) ||
                SameFilePath(port->File, engine.ImageTool))
            {
                portError(IDS_BadLogFile);
            }
            for (const auto d : seen)
            {
                if (SameFilePath(port->File, d->File))
                {
                    portError(IDS_BadLogFile);
                }
            }
        }
        else if (port->Mode == L"listen" || port->Mode == L"connect")
        {
            if (!SafeHost(port->Host) || !port->Port || port->Port > 65535)
            {
                portError(IDS_BadEndpoint);
            }
        }
        else if (port->Mode != L"none" && port->Mode != L"window")
        {
            portError(IDS_BadEndpoint);
        }
    }
    if (p.Serial.Mode == L"file" && p.DebugPort.Mode == L"file" && SameFilePath(p.Serial.File, p.DebugPort.File))
    {
        portError(IDS_BadLogFile);
    }
    if (p.Serial.Mode == L"listen" && p.DebugPort.Mode == L"listen" && p.Serial.Port == p.DebugPort.Port &&
        p.Serial.Host == p.DebugPort.Host)
    {
        portError(IDS_BadEndpoint);
    }
    const auto& n = p.Network;
    if (n.Backend.empty() || n.Backend == L"none")
    {
        return;
    }
    const bool socket = n.Backend == L"listen" || n.Backend == L"connect";
    if (!std::ranges::contains(caps.NetBackends, socket ? L"socket"s : n.Backend) ||
        !std::ranges::contains(machine.NicModels, n.Model) ||
        (!socket && n.Backend != L"tap" && n.Backend != L"user") ||
        (n.Backend == L"tap" && Trim(n.Interface).empty()) ||
        (socket && (!SafeHost(n.Host) || !n.Port || n.Port > 65535)))
    {
        DeviceError(IDS_BadNetwork, ProfileSection::Network);
    }
    if (!n.Mac.empty())
    {
        if (n.Mac.size() != 17)
        {
            DeviceError(IDS_BadNetwork, ProfileSection::Network);
        }
        for (size_t i = 0; i < n.Mac.size(); ++i)
        {
            if (i % 3 == 2 ? n.Mac[i] != L':' : !std::iswxdigit(n.Mac[i]))
            {
                DeviceError(IDS_BadNetwork, ProfileSection::Network);
            }
        }
    }
}

void AddDevices(std::vector<std::wstring>& args, const Profile& p, bool validation)
{
    if (p.ForcePs2)
    {
        args.emplace_back(L"-force-ps2-input");
    }
    if (p.UsbMouse)
    {
        args.emplace_back(L"-use-usb-mouse-instead-of-tablet");
    }
    if (p.NoReboot)
    {
        args.emplace_back(L"-no-reboot");
    }
    AddPort(args, L"serial0", p.Serial, validation);
    AddPort(args, L"debug0", p.DebugPort, validation);
    AddNic(args, p, validation);
    if (!Onboard(p.Bus))
    {
        args.insert(args.end(), {L"-device", p.Bus + L",id=onboard,addr=0x10,romfile="});
    }
    for (const auto& c : p.Controllers)
    {
        if (!Onboard(c.Model))
        {
            args.insert(args.end(), {L"-device", std::format(L"{},id={},addr=0x{:x},romfile=", c.Model, c.Id,
                                                             ControllerAddress(p, c))});
        }
    }
    for (auto d : AllDrives(p))
    {
        if (!d->Connected())
        {
            continue;
        }
        std::wstring value = L"if=none,id=" + d->Id + L",media=" + d->Media;
        if (!d->File.empty())
        {
            value += L",format=" + d->Format + L",file=" + QemuValue(Fs::absolute(d->File).native());
        }
        if (d->Protected())
        {
            value += L",readonly=on";
        }
        if (!d->Cache.empty())
        {
            value += L",cache=" + d->Cache;
        }
        if (!d->Discard.empty())
        {
            value += L",discard=" + d->Discard;
        }
        if (!d->DetectZeroes.empty())
        {
            value += L",detect-zeroes=" + d->DetectZeroes;
        }
        auto dev = DriveDevice(p, *d);
        auto spec = Wide(dev.at("driver").get<std::string>());
        for (const auto& [key, prop] : dev.items())
        {
            if (key != "driver")
            {
                spec += L"," + Wide(key) + L"=" + Wide(prop.is_string() ? prop.get<std::string>() : prop.dump());
            }
        }
        args.insert(args.end(), {L"-drive", value, L"-device", spec});
    }
}
} // namespace Ia64
