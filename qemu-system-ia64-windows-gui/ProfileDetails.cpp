// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "ProfileDetails.h"
#include "Devices.h"
#include <format>
#include "Localization.h"

#include <commctrl.h>
#include <algorithm>

namespace Ia64::Ui
{
std::wstring NetworkDefault(std::wstring_view machine, const Capabilities& capabilities)
{
    const auto model = DefaultNicModel(machine);
    if (model.empty())
    {
        return machine == L"x-ia64-i2000-efi-test" ? std::format(L"i82559c · {}", LText(IDS_NetDisconnected))
                                                   : LText(IDS_NoNetworkAdapter);
    }
    const bool nat = std::ranges::contains(capabilities.NetBackends, std::wstring_view(L"user"));
    return std::format(L"{} · {}", nat ? L"NAT" : LText(IDS_NetDisconnected), model);
}

std::wstring VgaName(std::wstring_view value)
{
    return value == L"none" ? LText(IDS_NoVideo) : std::wstring(value);
}

void ShowProfileDetails(HWND list, const Profile& profile, const Capabilities* capabilities, bool review,
                        std::optional<unsigned> newDiskMiB, int unavailable)
{
    const auto machine = capabilities ? capabilities->Find(profile.Machine) : nullptr;
    ListView_DeleteAllItems(list);
    ListView_RemoveAllGroups(list);
    int groupId{};
    int row{};
    const auto group = [&](int title) {
        auto text = LText(title);
        LVGROUP entry{};
        entry.cbSize = sizeof(entry);
        entry.mask = LVGF_GROUPID | LVGF_HEADER;
        entry.iGroupId = groupId = title;
        entry.pszHeader = text.data();
        ListView_InsertGroup(list, -1, &entry);
    };
    const auto property = [&](std::wstring key, std::wstring value) {
        LVITEMW entry{};
        entry.mask = LVIF_TEXT | LVIF_GROUPID;
        entry.iGroupId = groupId;
        entry.iItem = row++;
        entry.pszText = key.data();
        const auto index = ListView_InsertItem(list, &entry);
        ListView_SetItemText(list, index, 1, value.data());
    };
    const auto field = [&](int label, std::wstring value) { property(LText(label), std::move(value)); };
    const auto model = [unavailable](const std::wstring& chosen, const std::wstring& machineDefault) {
        if (!chosen.empty())
        {
            return chosen;
        }
        return machineDefault.empty() ? LText(unavailable) : LFmt(IDS_DefaultValue, machineDefault);
    };

    group(IDS_Hardware);
    if (review)
    {
        field(IDS_Name, profile.Name);
    }
    field(IDS_Machine, profile.Machine);
    if (!profile.Nvram.empty())
    {
        field(IDS_NvramFile, profile.Nvram.native());
    }
    field(IDS_Cpu, model(profile.Cpu, machine ? machine->DefaultCpu : L""));
    const auto processors = static_cast<uint64_t>(profile.Sockets) * profile.Cores * profile.Threads;
    field(IDS_CpuTopology, LFmt(IDS_CpuTopologyValue, profile.Sockets, profile.Cores, profile.Threads, processors));
    field(IDS_Memory, LFmt(IDS_MibValue, profile.RamMiB));
    field(IDS_Accel, profile.Llvm ? L"TCG-LLVM" : L"TCG");

    group(IDS_Storage);
    std::vector<StorageController> controllers{{L"onboard", profile.Bus}};
    controllers.insert(controllers.end(), profile.Controllers.begin(), profile.Controllers.end());
    for (const auto& controller : controllers)
    {
        field(IDS_Bus, controller.Model);
        for (const auto drive : AllDrives(profile))
        {
            if (drive->Controller != controller.Id || !drive->Connected())
            {
                continue;
            }
            const auto name = LFmt(IDS_AttachmentPath,
                                   LText(drive->Media == L"cdrom" ? IDS_OpticalDrive : IDS_DiskFile), drive->Slot);
            auto value = drive->File.empty() ? LText(IDS_None) : drive->File.native();
            if (!drive->File.empty() && drive->Protected())
            {
                value += L" (" + LText(IDS_WriteProtected) + L")";
            }
            property(name, value);
        }
    }
    if (newDiskMiB)
    {
        field(IDS_DiskCapacity, LFmt(IDS_MibValue, *newDiskMiB));
    }
    group(IDS_NetworkAdapter);
    if (profile.Network.Backend.empty())
    {
        field(IDS_NetBackend,
              machine ? LFmt(IDS_DefaultValue, NetworkDefault(profile.Machine, *capabilities)) : LText(unavailable));
    }
    else
    {
        field(IDS_NetBackend,
              profile.Network.Backend == L"none" ? LText(IDS_NetDisconnected) : profile.Network.Backend);
        if (profile.Network.Backend != L"none")
        {
            field(IDS_NicModel, profile.Network.Model);
        }
    }
    if (profile.Serial.Mode != L"none" || profile.DebugPort.Mode != L"none")
    {
        group(IDS_Ports);
        for (const auto& [label, port] :
             {std::pair{IDS_Serial, &profile.Serial}, std::pair{IDS_DebugPort, &profile.DebugPort}})
        {
            if (port->Mode == L"window")
            {
                field(label, LText(IDS_ConsoleWindow));
            }
            else if (port->Mode == L"file")
            {
                field(label, port->File.native());
            }
            else if (port->Mode != L"none")
            {
                field(label, std::format(L"{}  {}:{}", LText(port->Mode == L"listen" ? IDS_TcpListen : IDS_TcpConnect),
                                         port->Host, port->Port));
            }
        }
    }
    group(IDS_Display);
    field(IDS_Vga, model(VgaName(profile.Vga), machine ? VgaName(machine->DefaultVga) : L""));
    if (!profile.AtiModel.empty())
    {
        field(IDS_Ati, profile.AtiModel);
    }
    if (profile.Width && profile.Height)
    {
        field(IDS_InitialResolution, LFmt(IDS_ResolutionValue, profile.Width, profile.Height));
    }
    if (profile.MaxWidth && profile.MaxHeight)
    {
        field(IDS_MaxResolution, LFmt(IDS_ResolutionValue, profile.MaxWidth, profile.MaxHeight));
    }
    if (profile.VramMiB)
    {
        field(IDS_VideoMemory, LFmt(IDS_MibValue, profile.VramMiB));
    }

    if (profile.Llvm || !profile.MachineOptions.empty() || !profile.Alat.empty() || !profile.FirmwareBase.empty() ||
        profile.NoReboot || profile.ForcePs2 || profile.UsbMouse)
    {
        group(IDS_Advanced);
        if (profile.Llvm)
        {
            field(IDS_Cache, std::to_wstring(profile.CacheMiB));
        }
        if (profile.Llvm && (profile.LlvmThreads || (capabilities && capabilities->LlvmThreads)))
        {
            field(IDS_LlvmThreads, profile.LlvmThreads ? std::to_wstring(profile.LlvmThreads)
                                                       : LFmt(IDS_DefaultValue, *capabilities->LlvmThreads));
        }
        if (!profile.Alat.empty())
        {
            field(IDS_Alat, profile.Alat);
        }
        if (!profile.FirmwareBase.empty())
        {
            field(IDS_FirmwareBase, profile.FirmwareBase);
        }
        if (profile.ForcePs2)
        {
            field(IDS_ForcePs2, LText(IDS_Enabled));
        }
        if (profile.UsbMouse)
        {
            field(IDS_UsbMouse, LText(IDS_Enabled));
        }
        if (profile.NoReboot)
        {
            field(IDS_NoReboot, LText(IDS_Enabled));
        }
        for (const auto& [key, enabled] : profile.MachineOptions)
        {
            const auto option =
                std::ranges::find_if(DeviceOptions, [&](const auto& item) { return item.first == key; });
            property(option == DeviceOptions.end() ? key : LText(option->second),
                     LText(enabled ? IDS_Enabled : IDS_Disabled));
        }
    }
}
} // namespace Ia64::Ui
