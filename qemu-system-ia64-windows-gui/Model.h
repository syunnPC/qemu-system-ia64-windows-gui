// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Protocol.h"
#include "Localization.h"
#include <map>
#include <set>
#include <stdexcept>

namespace Ia64
{
enum class Appearance
{
    System,
    Light,
    Dark
};
struct DisplaySettings
{
    bool FitToWindow = true;
    bool ResizeToGuest = false;
    bool operator==(const DisplaySettings&) const = default;
};
struct InterfaceSettings
{
    Appearance Theme = Appearance::System;
    UiLanguage Language = UiLanguage::System;
    DisplaySettings DefaultDisplay;
    bool CloseOnShutdown = true;
    bool operator==(const InterfaceSettings&) const = default;
};

struct Engine
{
    Fs::path Qemu;
    Fs::path ImageTool;
    Fs::path Firmware;
    std::string ReleaseTag;
};

struct Drive
{
    std::wstring Id;
    std::wstring Controller = L"onboard";
    std::wstring Media = L"disk";
    unsigned Slot{};
    Fs::path File;
    std::wstring Format;
    bool ReadOnly{}, Attached{true};
    std::wstring Cache, Discard, DetectZeroes;
    int BootIndex{-1};
    void SelectImage(Fs::path path);
    void UpdateImage(Fs::path path, bool newQcow2 = false);
    bool Locked() const
    {
        return Format == L"raw" || Media == L"cdrom";
    }
    bool Protected() const
    {
        return ReadOnly || Locked();
    }
    bool Connected() const
    {
        return Attached && (!File.empty() || Media == L"cdrom");
    }
};

struct StorageController
{
    std::wstring Id, Model;
    unsigned PciSlot{};
};

struct CharPort
{
    std::wstring Mode = L"none";
    Fs::path File;
    std::wstring Host = L"127.0.0.1";
    unsigned Port{};
};

struct Nic
{
    std::wstring Backend, Model, Mac, Interface, Host = L"127.0.0.1", Forward;
    unsigned Port{};
    bool BootRom{};
};

struct Profile
{
    std::wstring Id;
    std::wstring Name;
    std::wstring Machine = L"itanium2-vpc";
    std::wstring Cpu;
    unsigned Sockets = 1;
    unsigned Cores = 1;
    unsigned Threads = 1;
    unsigned RamMiB = 2048;
    Drive Primary{L"ia64-hd"};
    Drive Optical{L"ia64-cd", L"onboard", L"cdrom", 1};
    std::vector<Drive> Drives;
    std::vector<StorageController> Controllers;
    std::wstring Bus = L"SCSI";
    CharPort Serial, DebugPort;
    Nic Network;
    std::wstring Alat, FirmwareBase;
    bool ForcePs2{}, UsbMouse{}, NoReboot{};
    unsigned LlvmThreads{};
    Fs::path Nvram;
    bool SkipSnapshotPrompt{};
    std::optional<DisplaySettings> DisplayOverride;
    std::wstring Vga;
    std::wstring AtiModel;
    unsigned Width{};
    unsigned Height{};
    unsigned MaxWidth{};
    unsigned MaxHeight{};
    unsigned VramMiB{};
    std::map<std::wstring, bool> MachineOptions;
    bool Llvm{};
    unsigned CacheMiB = 1024;
};

struct State
{
    Engine Tools;
    InterfaceSettings Interface;
    std::vector<Profile> Profiles;
};

struct Machine
{
    std::wstring Name;
    std::wstring DefaultCpu;
    unsigned MaxCpus{};
    std::set<std::wstring> Properties;
    std::vector<std::wstring> VgaTypes;
    std::wstring DefaultVga;
    std::map<std::wstring, bool> BooleanDefaults;
    std::vector<std::wstring> NicModels, ControllerModels;
    std::wstring DefaultAlat, DefaultFirmwareBase;
};

inline constexpr unsigned MaxDisplayWidth = 16000;
inline constexpr unsigned MaxDisplayHeight = 12000;
inline constexpr unsigned DefaultDisplayWidth = 1280;
inline constexpr unsigned DefaultDisplayHeight = 1024;

struct Capabilities
{
    std::wstring Version;
    std::vector<Machine> Machines;
    std::vector<std::wstring> Cpus;
    std::map<std::wstring, std::wstring> CpuAliases;
    std::map<std::wstring, std::set<std::wstring>> VideoProperties;
    bool Llvm{};
    std::optional<unsigned> LlvmThreads;
    std::vector<std::wstring> NetBackends, TapAdapters;
    const Machine* Find(std::wstring_view name) const;
    std::wstring CanonicalCpu(std::wstring_view cpu) const;
};

State LoadState();
void SaveState(const State& state);
bool DiskUsedElsewhere(const State& state, const Profile& profile);
bool SameFilePath(const Fs::path& first, const Fs::path& second);
Fs::path NvramPath(const Profile& profile);
void SaveProfileRemoval(const State& previous, const State& updated, const Fs::path& disk);
State ParseSettings(std::string_view source);
std::string FormatSettings(const State& state);
void ValidateEngine(const Engine& engine);
Capabilities ProbeEngine(const Engine& engine);
Machine ProbeMachine(const Engine& engine, Machine machine);
std::vector<std::wstring> Buses(std::wstring_view machine);
std::pair<unsigned, unsigned> RamRange(std::wstring_view machine);
unsigned DefaultRam(std::wstring_view machine);
std::wstring_view EffectiveVga(const Profile& profile, const Machine& machine);
std::wstring_view DefaultAti(std::wstring_view machine);
unsigned DefaultVram(std::wstring_view adapter);
unsigned MaximumVram(std::wstring_view adapter);
unsigned MinimumVram(const Profile& profile, const Machine& machine);
enum class ProfileSection
{
    General,
    Processor,
    Memory,
    Controller,
    Optical,
    Display,
    Advanced,
    Ports,
    Network
};
struct ProfileValidationError : std::runtime_error
{
    ProfileValidationError(ProfileSection section, std::wstring_view message)
        : std::runtime_error(Utf8(message)), Section(section)
    {
    }
    ProfileSection Section;
};
void ValidateProfile(const Profile& profile, const Machine& machine, const Capabilities& capabilities,
                     const Engine& engine);
void ValidateBoot(const Engine& engine, const Profile& profile, const Machine& machine);
struct VmSnapshot
{
    std::string Tag, Id;
    int64_t Date{};
    int Nanoseconds{};
    bool operator==(const VmSnapshot&) const = default;
};
struct DiskInfo
{
    std::wstring Format;
    std::vector<VmSnapshot> Snapshots;
};
std::vector<VmSnapshot> VmSnapshots(const Json& image);
DiskInfo InspectDisk(const Engine& engine, const Fs::path& path);
void CreateDisk(const Engine& engine, const Fs::path& path, unsigned mebibytes);
ProcessCommand LaunchCommand(const Engine& engine, const Profile& profile, const Machine& machine, const Listener& qmp,
                             const Listener* video);
} // namespace Ia64
