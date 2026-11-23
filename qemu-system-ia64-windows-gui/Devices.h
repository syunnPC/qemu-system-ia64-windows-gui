// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Model.h"

namespace Ia64
{
std::vector<Drive*> AllDrives(Profile& profile);
std::vector<const Drive*> AllDrives(const Profile& profile);
const Drive* SnapshotDrive(const Profile& profile);
std::vector<VmSnapshot> InspectStorage(Profile& profile, const Engine& engine, bool newPrimary = false);
std::wstring ControllerModel(const Profile& profile, std::wstring_view id);
bool IsScsi(std::wstring_view model);
unsigned ControllerAddress(const Profile& profile, const StorageController& controller);
unsigned PortCount(std::wstring_view model);
std::wstring DriveBus(std::wstring_view machine, std::wstring_view model, std::wstring_view controller, unsigned slot);
Json DriveDevice(const Profile& profile, const Drive& drive);
std::vector<std::wstring> TapAdapterNames();
void ProbeDeviceModels(const Engine& engine, Machine& machine);
void ValidateDevices(const Profile& profile, const Machine& machine, const Capabilities& caps, const Engine& engine);
void AddDevices(std::vector<std::wstring>& args, const Profile& profile, bool validation);
bool ValidFirmwareBase(std::wstring_view text);
bool IsVpc(std::wstring_view name);
std::wstring_view DefaultNicModel(std::wstring_view machine);
} // namespace Ia64
