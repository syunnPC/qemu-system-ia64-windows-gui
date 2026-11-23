// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ia64
{
namespace Fs = std::filesystem;
using namespace std::chrono_literals;

class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : _value(value)
    {
    }
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle()
    {
        Reset();
    }
    explicit operator bool() const noexcept
    {
        return _value && _value != INVALID_HANDLE_VALUE;
    }
    HANDLE Get() const noexcept
    {
        return _value;
    }
    void Reset(HANDLE value = nullptr) noexcept;

private:
    HANDLE _value{};
};

std::string Utf8(std::wstring_view value);
std::wstring Wide(std::string_view value);
[[noreturn]] void Fail(std::wstring_view message);
[[noreturn]] void WinError(std::wstring_view operation, DWORD error = GetLastError());
std::wstring NewId();
Fs::path DataDirectory();
unsigned InstalledRamMiB();
Fs::path SystemExecutable(std::wstring_view name);
bool IsFile(const Fs::path& path);
std::wstring QuoteArgument(std::wstring_view argument);
std::wstring QemuValue(std::wstring_view value);
void WriteAll(HANDLE handle, std::string_view data);
void WriteAtomic(const Fs::path& path, std::string_view data);
std::wstring Trim(std::wstring_view value);

struct ProcessCommand
{
    ProcessCommand(const Fs::path& executable, std::span<const std::wstring> arguments);
    Fs::path Application;
    std::wstring Text;
};

class Process
{
public:
    Process(const Fs::path& executable, std::span<const std::wstring> arguments);
    explicit Process(const ProcessCommand& command);
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    std::string ReadOutput()
    {
        return ReadAvailable(_output.Get());
    }
    std::string ReadErrors()
    {
        return ReadAvailable(_errors.Get());
    }
    void Write(std::string_view data)
    {
        WriteAll(_input.Get(), data);
    }
    void CloseInput()
    {
        _input.Reset();
    }
    bool Finished(std::chrono::milliseconds timeout = 0ms) const;
    DWORD ExitCode() const;
    void Stop() noexcept;

private:
    static std::string ReadAvailable(HANDLE pipe);
    Handle _job;
    Handle _process;
    Handle _input;
    Handle _output;
    Handle _errors;
};

struct ProcessResult
{
    DWORD ExitCode{};
    std::string Output;
    std::string Errors;
};

ProcessResult Run(const Fs::path& executable, std::span<const std::wstring> arguments,
                  std::chrono::milliseconds timeout = 15s);
void RequireSuccess(const ProcessResult& result, std::wstring_view operation);
} // namespace Ia64
