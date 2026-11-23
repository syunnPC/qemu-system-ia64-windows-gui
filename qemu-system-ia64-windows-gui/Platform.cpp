// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Platform.h"
#include "Localization.h"

#include <ShlObj.h>
#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Ia64
{
Handle::Handle(Handle&& other) noexcept : _value(std::exchange(other._value, nullptr))
{
}

Handle& Handle::operator=(Handle&& other) noexcept
{
    if (this != &other)
    {
        Reset(std::exchange(other._value, nullptr));
    }
    return *this;
}

void Handle::Reset(HANDLE value) noexcept
{
    if (*this)
    {
        CloseHandle(_value);
    }
    _value = value;
}

std::string Utf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const auto length = static_cast<int>(value.size());
    const int count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (!count)
    {
        WinError(LText(IDS_Utf8Conversion));
    }
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Wide(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    const auto length = static_cast<int>(value.size());
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), length, nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), length, result.data(), count);
    return result;
}

[[noreturn]] void Fail(std::wstring_view message)
{
    throw std::runtime_error(Utf8(message));
}

[[noreturn]] void WinError(std::wstring_view operation, DWORD error)
{
    LPWSTR message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    const auto description = message ? Trim(message) : LText(IDS_WinUnknown);
    LocalFree(message);
    Fail(LFmt(IDS_WinError, operation, description, error));
}

std::wstring NewId()
{
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
    {
        Fail(LText(IDS_IdError));
    }
    wchar_t value[39]{};
    StringFromGUID2(id, value, static_cast<int>(std::size(value)));
    return std::wstring(value + 1, 36);
}

Fs::path DataDirectory()
{
    PWSTR path = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(path, CoTaskMemFree);
    if (FAILED(result))
    {
        Fail(LText(IDS_AppDataError));
    }
    return Fs::path(path) / L"QEMU IA-64 Manager";
}

unsigned InstalledRamMiB()
{
    ULONGLONG kibibytes{};
    if (!GetPhysicallyInstalledSystemMemory(&kibibytes))
    {
        WinError(LText(IDS_HostRamRead));
    }
    return static_cast<unsigned>(std::min<ULONGLONG>(kibibytes / 1024, std::numeric_limits<int>::max()));
}

Fs::path SystemExecutable(std::wstring_view name)
{
    std::wstring path(MAX_PATH, L'\0');
    const auto size = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (!size || size >= path.size())
    {
        WinError(L"GetSystemDirectoryW");
    }
    path.resize(size);
    return Fs::path(path) / name;
}

bool IsFile(const Fs::path& path)
{
    std::error_code error;
    return !path.empty() && Fs::is_regular_file(path, error);
}

std::wstring QuoteArgument(std::wstring_view argument)
{
    std::wstring result(1, L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
        backslashes = 0;
        if (character == L'"')
        {
            result.push_back(L'\\');
        }
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring QemuValue(std::wstring_view value)
{
    std::wstring escaped;
    for (const auto character : value)
    {
        escaped.push_back(character);
        if (character == L',')
        {
            escaped.push_back(character);
        }
    }
    return escaped;
}

void WriteAll(HANDLE handle, std::string_view data)
{
    while (!data.empty())
    {
        DWORD written{};
        const auto count = static_cast<DWORD>(std::min<size_t>(data.size(), 64 * 1024));
        if (!WriteFile(handle, data.data(), count, &written, nullptr) || !written)
        {
            WinError(L"WriteFile");
        }
        data.remove_prefix(written);
    }
}

void WriteAtomic(const Fs::path& path, std::string_view data)
{
    Fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L"." + NewId() + L".tmp";
    try
    {
        Handle file(
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file)
        {
            WinError(LText(IDS_SettingsCreate));
        }
        WriteAll(file.Get(), data);
        if (!FlushFileBuffers(file.Get()))
        {
            WinError(LText(IDS_SettingsFlush));
        }
        file.Reset();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            WinError(LText(IDS_SettingsSave));
        }
    }
    catch (...)
    {
        std::error_code error;
        Fs::remove(temporary, error);
        throw;
    }
}

std::wstring Trim(std::wstring_view value)
{
    const auto start = value.find_first_not_of(L" \t\r\n");
    if (start == std::wstring_view::npos)
    {
        return {};
    }
    return std::wstring(value.substr(start, value.find_last_not_of(L" \t\r\n") - start + 1));
}

namespace
{
struct Pipe
{
    Handle Read;
    Handle Write;

    Pipe()
    {
        HANDLE reader{}, writer{};
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        if (!CreatePipe(&reader, &writer, &security, 0))
        {
            WinError(L"CreatePipe");
        }
        Read.Reset(reader);
        Write.Reset(writer);
    }
};

void ClearInheritance(const Handle& handle)
{
    if (!SetHandleInformation(handle.Get(), HANDLE_FLAG_INHERIT, 0))
    {
        WinError(L"SetHandleInformation");
    }
}

class ProcessAttributes
{
public:
    explicit ProcessAttributes(std::array<HANDLE, 3>& handles)
    {
        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        _buffer.resize(size);
        _list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(_buffer.data());
        if (!InitializeProcThreadAttributeList(_list, 1, 0, &size))
        {
            WinError(LText(IDS_ProcessAttrs));
        }
        if (!UpdateProcThreadAttribute(_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof(handles),
                                       nullptr, nullptr))
        {
            const auto error = GetLastError();
            DeleteProcThreadAttributeList(_list);
            WinError(LText(IDS_ProcessHandles), error);
        }
    }

    ~ProcessAttributes()
    {
        DeleteProcThreadAttributeList(_list);
    }
    LPPROC_THREAD_ATTRIBUTE_LIST Get() const
    {
        return _list;
    }

private:
    std::vector<std::byte> _buffer;
    LPPROC_THREAD_ATTRIBUTE_LIST _list{};
};
} // namespace

ProcessCommand::ProcessCommand(const Fs::path& executable, std::span<const std::wstring> arguments)
    : Application(Fs::absolute(executable)), Text(QuoteArgument(Application.native()))
{
    for (const auto& argument : arguments)
    {
        Text += L" " + QuoteArgument(argument);
    }
}

Process::Process(const Fs::path& executable, std::span<const std::wstring> arguments)
    : Process(ProcessCommand(executable, arguments))
{
}

Process::Process(const ProcessCommand& invocation)
{
    const auto& application = invocation.Application;
    auto command = invocation.Text; // CreateProcessW takes a writable command line.
    Pipe input, output, errors;
    ClearInheritance(input.Write);
    ClearInheritance(output.Read);
    ClearInheritance(errors.Read);
    std::array handles{input.Read.Get(), output.Write.Get(), errors.Write.Get()};
    ProcessAttributes attributes(handles);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0];
    startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError = handles[2];
    startup.lpAttributeList = attributes.Get();

    _job.Reset(CreateJobObjectW(nullptr, nullptr));
    if (!_job)
    {
        WinError(L"CreateJobObjectW");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(_job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        WinError(LText(IDS_ProcessLifetime));
    }
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        application.parent_path().c_str(), &startup.StartupInfo, &information))
    {
        WinError(application.native());
    }
    _process.Reset(information.hProcess);
    Handle thread(information.hThread);
    if (!AssignProcessToJobObject(_job.Get(), _process.Get()) || ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
    {
        const auto error = GetLastError();
        TerminateProcess(_process.Get(), 1);
        WinError(LText(IDS_ProcessStart), error);
    }
    _input = std::move(input.Write);
    _output = std::move(output.Read);
    _errors = std::move(errors.Read);
}

std::string Process::ReadAvailable(HANDLE pipe)
{
    DWORD available{};
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
    {
        const auto error = GetLastError();
        if (error == ERROR_BROKEN_PIPE)
        {
            return {};
        }
        WinError(LText(IDS_ProcessReadPipe), error);
    }
    if (!available)
    {
        return {};
    }
    std::string data(std::min<DWORD>(available, 64 * 1024), '\0');
    DWORD read{};
    if (!ReadFile(pipe, data.data(), static_cast<DWORD>(data.size()), &read, nullptr))
    {
        WinError(LText(IDS_ProcessRead));
    }
    data.resize(read);
    return data;
}

bool Process::Finished(std::chrono::milliseconds timeout) const
{
    const auto milliseconds = static_cast<DWORD>(std::clamp<int64_t>(timeout.count(), 0, INFINITE - 1));
    return WaitForSingleObject(_process.Get(), milliseconds) == WAIT_OBJECT_0;
}

DWORD Process::ExitCode() const
{
    DWORD code{};
    if (!GetExitCodeProcess(_process.Get(), &code))
    {
        WinError(LText(IDS_ProcessExitCode));
    }
    return code;
}

void Process::Stop() noexcept
{
    TerminateJobObject(_job.Get(), 1);
}

ProcessResult Run(const Fs::path& executable, std::span<const std::wstring> arguments,
                  std::chrono::milliseconds timeout)
{
    Process process(executable, arguments);
    process.CloseInput();
    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        const bool finished = process.Finished();
        auto output = process.ReadOutput();
        auto errors = process.ReadErrors();
        if (finished && output.empty() && errors.empty())
        {
            break;
        }
        result.Output += output;
        result.Errors += errors;
        if (result.Output.size() + result.Errors.size() > 8 * 1024 * 1024)
        {
            Fail(LText(IDS_OutputLimit));
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            Fail(LFmt(IDS_ProcessTimeout, executable.filename().native()));
        }
        if (output.empty() && errors.empty())
        {
            std::this_thread::sleep_for(10ms);
        }
    }
    result.ExitCode = process.ExitCode();
    return result;
}

void RequireSuccess(const ProcessResult& result, std::wstring_view operation)
{
    if (result.ExitCode != 0)
    {
        Fail(LFmt(IDS_CommandError, operation, result.ExitCode,
                  Trim(Wide(result.Errors.empty() ? result.Output : result.Errors))));
    }
}
} // namespace Ia64
