// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Model.h"
#include <array>
#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>

namespace Ia64
{
struct Frame
{
    unsigned Width{};
    unsigned Height{};
    std::vector<uint32_t> Pixels;
};

struct PointerInput
{
    int X{};
    int Y{};
    int Dx{};
    int Dy{};
    unsigned Buttons{};
    bool Movement{};
};

struct KeyInput
{
    unsigned ScanCode{};
    unsigned KeySym{};
    bool Down{};
};

enum class Console
{
    Monitor,
    Serial,
    Debug
};
enum class VmStatus
{
    Starting,
    Restoring,
    Running,
    Paused,
    Saving,
    Stopped,
    Failed
};

class Session
{
public:
    Session(ProcessCommand command, VmConnections connections, std::optional<VmSnapshot> resume, const Profile& profile,
            bool video);
    ~Session();
    void Stop();
    void Enqueue(std::string command, Json arguments = nullptr, bool displayResult = false);
    void Monitor(std::wstring_view command);
    Json DeviceCall(std::string command, Json arguments = nullptr);
    void ConsoleInput(Console console, std::wstring_view text);
    void SendCtrlAltDelete();
    void SaveAndQuit(std::wstring drive);
    void Pointer(PointerInput input);
    void Key(KeyInput input);
    std::shared_ptr<const Frame> CurrentFrame() const
    {
        return _frame.load();
    }
    VmStatus Status() const
    {
        return _status.load();
    }
    bool AbsolutePointer() const
    {
        return _absolutePointer.load();
    }
    const std::wstring& CommandLine() const
    {
        return _command.Text;
    }
    std::wstring ConsumeLog(Console console = Console::Monitor);

private:
    struct Command
    {
        std::string Name;
        Json Arguments;
        bool DisplayResult{};
        std::optional<std::promise<Json>> Reply;
    };
    void RunVm(std::stop_token stop, VmConnections& connections, const std::optional<VmSnapshot>& resume);
    void RunDisplay(std::stop_token stop, Socket socket);
    void Append(std::wstring_view line);
    void HandleEvent(const Json& event);
    Json Execute(Qmp& qmp, Command& command);
    void ReadConsole(Qmp& qmp, size_t index);
    const bool _video;
    const std::array<bool, 2> _ports;
    std::array<std::string, 2> _incomplete;
    std::array<bool, 2> _lastCr{};
    std::string _saveDrive;
    const std::string _resumeDrive;

    const ProcessCommand _command;
    std::mutex _mutex;
    std::deque<Command> _commands;
    std::optional<std::promise<void>> _saveRequest;
    std::deque<std::variant<PointerInput, KeyInput>> _input;
    std::array<std::wstring, 3> _logs;
    std::atomic<std::shared_ptr<const Frame>> _frame;
    std::atomic<VmStatus> _status{VmStatus::Starting};
    std::atomic<bool> _absolutePointer{true};
    std::jthread _worker;
};
} // namespace Ia64
