// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Session.h"
#include "Devices.h"
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#include "Localization.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <utility>

namespace Ia64
{
namespace
{
void RunSnapshotJob(Qmp& qmp, const VmSnapshot* resume, const std::string& stateDrive)
{
    const auto blocks = qmp.Call("query-block");
    const Json* primary = nullptr;
    std::vector<std::string> devices;
    for (const auto& block : blocks)
    {
        const auto inserted = block.find("inserted");
        if (inserted == block.end() || inserted->at("ro").get<bool>())
        {
            continue;
        }
        devices.push_back(inserted->at("node-name").get<std::string>());
        if ((block.value("device", "") == stateDrive || inserted->value("node-name", "") == stateDrive) &&
            inserted->value("drv", "") == "qcow2")
        {
            primary = &*inserted;
        }
    }
    if (!primary)
    {
        Fail(LText(IDS_SnapshotDiskRequired));
    }

    const auto& image = primary->at("image");
    const auto entries = image.value("snapshots", Json::array());
    const auto named = [&](const std::string& tag) {
        return std::ranges::count_if(entries, [&](const Json& item) { return item.at("name") == tag; });
    };
    std::string tag;
    if (resume)
    {
        tag = resume->Tag;
        // QMP resolves tags by name. Check the selected entry again under QEMU's disk lock.
        if (named(tag) != 1 || !std::ranges::contains(VmSnapshots(image), *resume))
        {
            Fail(LText(IDS_SnapshotChanged));
        }
    }
    else
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        const auto stamp = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.wYear, time.wMonth, time.wDay,
                                       time.wHour, time.wMinute, time.wSecond);
        tag = stamp;
        for (unsigned index = 2; named(tag); ++index)
        {
            tag = std::format("{} ({})", stamp, index);
        }
    }

    const auto job = "snapshot-" + Utf8(NewId());
    // Large RAM images can keep the monitor busy during a single save or load operation.
    constexpr auto snapshotIoTimeout = 10min;
    qmp.Call(resume ? "snapshot-load" : "snapshot-save",
             {{"job-id", job}, {"tag", tag}, {"vmstate", primary->at("node-name")}, {"devices", devices}},
             snapshotIoTimeout);
    for (;;)
    {
        const auto jobs = qmp.Call("query-jobs", nullptr, snapshotIoTimeout);
        const auto found = std::ranges::find_if(jobs, [&](const Json& entry) { return entry.at("id") == job; });
        if (found == jobs.end())
        {
            Fail(LText(IDS_SnapshotJobLost));
        }
        if (found->at("status") == "concluded")
        {
            const auto error = found->value("error", "");
            qmp.Call("job-dismiss", {{"id", job}});
            if (!error.empty())
            {
                Fail(Wide(error));
            }
            return;
        }
        std::this_thread::sleep_for(75ms);
    }
}

class Rfb
{
public:
    Rfb(Socket socket, std::stop_token stop) : _socket(std::move(socket)), _stop(stop)
    {
        if (Read(12) != "RFB 003.008\n")
        {
            Fail(LText(IDS_RfbVersion));
        }
        _socket.Send("RFB 003.008\n");
        const auto count = U8(Read(1), 0);
        const auto methods = Read(count);
        if (methods.find('\1') == std::string::npos)
        {
            Fail(LText(IDS_RfbSecurity));
        }
        _socket.Send(std::string_view("\1", 1));
        if (U32(Read(4), 0))
        {
            Fail(LText(IDS_RfbRejected));
        }
        _socket.Send(std::string_view("\1", 1));
        const auto server = Read(24);
        Resize(U16(server, 0), U16(server, 2));
        const auto nameLength = U32(server, 20);
        if (nameLength > 65536)
        {
            Fail(LText(IDS_RfbName));
        }
        Read(nameLength);

        const std::array<unsigned char, 20> format{0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0};
        _socket.Send(std::string_view(reinterpret_cast<const char*>(format.data()), format.size()));

        std::string encodings("\2\0", 2);
        Put16(encodings, 4);
        for (const int encoding : {0, -223, -257, -258})
        {
            Put32(encodings, std::bit_cast<uint32_t>(encoding));
        }
        _socket.Send(encodings);
        Request(false);
    }

    bool Readable() const
    {
        return _socket.Ready(16ms);
    }
    const Frame& Image() const
    {
        return _image;
    }
    bool Absolute() const
    {
        return _absolute;
    }

    bool Update()
    {
        switch (U8(Read(1), 0))
        {
        case 0: {
            const auto rectangles = U16(Read(3), 1);
            bool changed = false;
            bool resized = false;
            for (unsigned index = 0; index < rectangles; ++index)
            {
                const auto header = Read(12);
                const unsigned x = U16(header, 0);
                const unsigned y = U16(header, 2);
                const unsigned width = U16(header, 4);
                const unsigned height = U16(header, 6);
                const auto encoding = std::bit_cast<int32_t>(U32(header, 8));
                switch (encoding)
                {
                case 0:
                    if (x + width > _image.Width || y + height > _image.Height || !width || !height)
                    {
                        Fail(LText(IDS_RfbRect));
                    }
                    for (unsigned row = y; row < y + height; ++row)
                    {
                        auto destination = _image.Pixels.data() + size_t{row} * _image.Width + x;
                        _socket.ReadExact({reinterpret_cast<char*>(destination), size_t{width} * 4}, _stop);
                    }
                    changed = true;
                    break;
                case -223:
                    Resize(width, height);
                    changed = false;
                    resized = true;
                    break;
                case -257:
                    _absolute = x != 0;
                    break;
                case -258:
                    _extendedKeys = true;
                    break;
                default:
                    Fail(LText(IDS_RfbEncoding));
                }
            }
            // QEMU may send pseudo-rectangles asynchronously, independently of a pending update.
            if (changed || resized || !rectangles)
            {
                Request(!resized);
            }
            return changed;
        }
        case 2:
            return false;
        case 3: {
            const auto length = U32(Read(7), 3);
            if (length > 1024 * 1024)
            {
                Fail(LText(IDS_RfbClipboard));
            }
            Read(length);
            return false;
        }
        default:
            Fail(LText(IDS_RfbMessage));
        }
    }

    void Send(const PointerInput& input)
    {
        std::string message(1, '\5');
        message.push_back(static_cast<char>(input.Buttons));
        if (_absolute)
        {
            Put16(message, std::clamp(input.X, 0, static_cast<int>(_image.Width) - 1));
            Put16(message, std::clamp(input.Y, 0, static_cast<int>(_image.Height) - 1));
        }
        else
        {
            Put16(message, 0x7fff + std::clamp(input.Dx, -0x7fff, 0x7fff));
            Put16(message, 0x7fff + std::clamp(input.Dy, -0x7fff, 0x7fff));
        }
        _socket.Send(message);
    }

    void Send(const KeyInput& input)
    {
        std::string message;
        if (_extendedKeys && input.ScanCode)
        {
            message.assign("\xff\0", 2);
            Put16(message, input.Down);
            Put32(message, input.KeySym);
            Put32(message, input.ScanCode);
        }
        else
        {
            if (!input.KeySym)
            {
                return;
            }
            message.assign("\4", 1);
            message.push_back(input.Down ? '\1' : '\0');
            Put16(message, 0);
            Put32(message, input.KeySym);
        }
        _socket.Send(message);
    }

private:
    static unsigned U8(std::string_view data, size_t offset)
    {
        return static_cast<unsigned char>(data[offset]);
    }

    static uint16_t U16(std::string_view data, size_t offset)
    {
        return static_cast<uint16_t>(U8(data, offset) << 8 | U8(data, offset + 1));
    }

    static uint32_t U32(std::string_view data, size_t offset)
    {
        return uint32_t{U16(data, offset)} << 16 | U16(data, offset + 2);
    }

    static void Put16(std::string& data, unsigned value)
    {
        data.push_back(static_cast<char>(value >> 8));
        data.push_back(static_cast<char>(value));
    }

    static void Put32(std::string& data, uint32_t value)
    {
        Put16(data, value >> 16);
        Put16(data, value & 0xffff);
    }

    std::string Read(size_t length)
    {
        std::string data(length, '\0');
        _socket.ReadExact(data, _stop);
        return data;
    }

    void Resize(unsigned width, unsigned height)
    {
        if (!width || !height || width > MaxDisplayWidth || height > MaxDisplayHeight ||
            size_t{width} * height > 512ull * 1024 * 1024 / sizeof(uint32_t))
        {
            Fail(LText(IDS_RfbSize));
        }
        _image = {width, height, std::vector<uint32_t>(size_t{width} * height)};
    }

    void Request(bool incremental)
    {
        std::string request(1, '\3');
        request.push_back(incremental ? '\1' : '\0');
        Put16(request, 0);
        Put16(request, 0);
        Put16(request, _image.Width);
        Put16(request, _image.Height);
        _socket.Send(request);
    }

    Socket _socket;
    std::stop_token _stop;
    Frame _image;
    bool _absolute{};
    bool _extendedKeys{};
};
} // namespace

Session::Session(ProcessCommand command, VmConnections connections, std::optional<VmSnapshot> resume,
                 const Profile& profile, bool video)
    : _video(video), _ports{profile.Serial.Mode == L"window", profile.DebugPort.Mode == L"window"},
      _resumeDrive(SnapshotDrive(profile) ? Utf8(SnapshotDrive(profile)->Id) : ""), _command(std::move(command)),
      _status(resume ? VmStatus::Restoring : VmStatus::Starting),
      _worker([this, connections = std::move(connections), resume = std::move(resume)](std::stop_token stop) mutable {
          RunVm(stop, connections, resume);
      })
{
}

Session::~Session()
{
    Stop();
}

void Session::Stop()
{
    if (!_worker.joinable())
    {
        return;
    }
    _worker.request_stop();
    _worker.join();
}

void Session::Enqueue(std::string command, Json arguments, bool displayResult)
{
    std::scoped_lock lock(_mutex);
    if (_status == VmStatus::Running || _status == VmStatus::Paused)
    {
        _commands.push_back({std::move(command), std::move(arguments), displayResult});
    }
}

void Session::Monitor(std::wstring_view command)
{
    if (command.size() > 4096)
    {
        Fail(LText(IDS_MonitorLimit));
    }
    Append(L"(qemu) " + std::wstring(command));
    Enqueue("human-monitor-command", {{"command-line", Utf8(command)}}, true);
}

Json Session::DeviceCall(std::string command, Json arguments)
{
    std::future<Json> result;
    {
        std::scoped_lock lock(_mutex);
        if (_status != VmStatus::Running && _status != VmStatus::Paused)
        {
            Fail(LText(IDS_SnapshotVmInactive));
        }
        Command request{std::move(command), std::move(arguments)};
        result = request.Reply.emplace().get_future();
        _commands.push_back(std::move(request));
    }
    return result.get();
}

void Session::ConsoleInput(Console console, std::wstring_view text)
{
    Enqueue("ringbuf-write",
            {{"device", console == Console::Serial ? "serial0" : "debug0"}, {"data", Utf8(text) + "\r"}});
}

void Session::SendCtrlAltDelete()
{
    Json keys = Json::array();
    for (const auto key : {"ctrl", "alt", "delete"})
    {
        keys.push_back({{"type", "qcode"}, {"data", key}});
    }
    Enqueue("send-key", {{"keys", std::move(keys)}});
}

void Session::SaveAndQuit(std::wstring drive)
{
    std::future<void> result;
    {
        std::scoped_lock lock(_mutex);
        if (_status != VmStatus::Running && _status != VmStatus::Paused)
        {
            Fail(LText(IDS_SnapshotVmInactive));
        }
        _saveDrive = Utf8(drive);
        result = _saveRequest.emplace().get_future();
        _status = VmStatus::Saving;
    }
    result.get();
}

void Session::Pointer(PointerInput input)
{
    std::scoped_lock lock(_mutex);
    if (input.Movement && !_input.empty())
    {
        if (auto last = std::get_if<PointerInput>(&_input.back());
            last && last->Movement && last->Buttons == input.Buttons)
        {
            input.Dx = std::clamp(last->Dx + input.Dx, -32767, 32767);
            input.Dy = std::clamp(last->Dy + input.Dy, -32767, 32767);
            *last = input;
            return;
        }
    }
    if (_input.size() < 512)
    {
        _input.emplace_back(input);
    }
}

void Session::Key(KeyInput input)
{
    std::scoped_lock lock(_mutex);
    if (_input.size() < 512 || !input.Down)
    {
        _input.emplace_back(input);
    }
}

std::wstring Session::ConsumeLog(Console console)
{
    std::scoped_lock lock(_mutex);
    return std::exchange(_logs[static_cast<size_t>(console)], {});
}

void Session::Append(std::wstring_view line)
{
    std::scoped_lock lock(_mutex);
    auto& _log = _logs[0];
    _log.append(line);
    if (!line.ends_with(L"\n"))
    {
        _log += L"\r\n";
    }
    if (_log.size() > 100'000)
    {
        _log.erase(0, _log.size() - 100'000);
    }
}

void Session::HandleEvent(const Json& event)
{
    const auto name = event.at("event").get<std::string>();
    if (name == "STOP")
    {
        auto expected = VmStatus::Running;
        _status.compare_exchange_strong(expected, VmStatus::Paused);
    }
    else if (name == "RESUME")
    {
        auto expected = VmStatus::Paused;
        _status.compare_exchange_strong(expected, VmStatus::Running);
    }
    else if (name == "RESET")
    {
        Append(LText(IDS_ResetNotice));
    }
    else if (name == "SHUTDOWN")
    {
        Append(LText(IDS_ShutdownNotice));
    }
}

Json Session::Execute(Qmp& qmp, Command& command)
{
    auto value = qmp.Call(command.Name, std::move(command.Arguments));
    if (command.DisplayResult && !value.is_null())
    {
        Append(value.is_string() ? Wide(value.get<std::string>()) : Wide(value.dump()));
    }
    return value;
}

void Session::ReadConsole(Qmp& qmp, size_t index)
{
    const auto base64 =
        qmp.Call("ringbuf-read", {{"device", index == 0 ? "serial0" : "debug0"}, {"size", 65536}, {"format", "base64"}})
            .get<std::string>();
    if (base64.empty())
    {
        return;
    }
    DWORD size{};
    if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr, &size,
                              nullptr, nullptr))
    {
        WinError(L"CryptStringToBinary");
    }
    auto& bytes = _incomplete[index];
    auto offset = bytes.size();
    bytes.resize(offset + size);
    if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(bytes.data() + offset), &size, nullptr, nullptr))
    {
        WinError(L"CryptStringToBinary");
    }
    size_t ready = bytes.size();
    if (ready)
    {
        size_t lead = ready - 1;
        while (lead && (static_cast<unsigned char>(bytes[lead]) & 0xc0) == 0x80)
        {
            --lead;
        }
        auto b = static_cast<unsigned char>(bytes[lead]);
        size_t needed = b >= 0xf0 && b <= 0xf4 ? 4 : b >= 0xe0 && b <= 0xef ? 3 : b >= 0xc2 && b <= 0xdf ? 2 : 1;
        if (ready - lead < needed)
        {
            ready = lead;
        }
    }
    if (!ready)
    {
        return;
    }
    // A serial line can contain arbitrary bytes. Windows replaces malformed UTF-8 with U+FFFD.
    std::wstring wide(ready, L'\0');
    auto count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(ready), wide.data(),
                                     static_cast<int>(wide.size()));
    wide.resize(count);
    bytes.erase(0, ready);
    std::wstring normalized;
    for (auto c : wide)
    {
        if (c == L'\r')
        {
            normalized += L"\r\n";
            _lastCr[index] = true;
            continue;
        }
        if (c == L'\n')
        {
            if (!_lastCr[index])
            {
                normalized += L"\r\n";
            }
        }
        else if (c)
        {
            normalized += c;
        }
        _lastCr[index] = false;
    }
    std::scoped_lock lock(_mutex);
    auto& log = _logs[index + 1];
    log += normalized;
    if (log.size() > 200000)
    {
        log.erase(0, log.size() - 200000);
    }
}

void Session::RunVm(std::stop_token stop, VmConnections& connections, const std::optional<VmSnapshot>& resume)
{
    std::optional<std::promise<void>> save;
    std::exception_ptr failure;
    bool snapshotSaved = false;
    auto finalStatus = VmStatus::Stopped;
    try
    {
        connections.Control.Listen();
        if (_video)
        {
            connections.Video.Listen();
        }
        Process process(_command);
        process.CloseInput();
        std::string diagnostics;
        bool quitRequested = false;
        Qmp qmp(connections.Control.Accept(process, stop, &diagnostics), [this, &quitRequested](const Json& event) {
            if (event.at("event") == "SHUTDOWN")
            {
                quitRequested = true;
            }
            HandleEvent(event);
        });
        std::jthread displayThread;
        if (_video)
        {
            auto display = connections.Video.Accept(process, stop, &diagnostics);
            displayThread = std::jthread([this, display = std::move(display)](std::stop_token token) mutable {
                RunDisplay(token, std::move(display));
            });
        }
        if (!diagnostics.empty())
        {
            Append(Wide(diagnostics));
        }

        // Finish an accepted disk restore before closing QEMU, even if the window closes meanwhile.
        if (resume && !stop.stop_requested())
        {
            RunSnapshotJob(qmp, &*resume, _resumeDrive);
        }
        if (!stop.stop_requested())
        {
            qmp.Call("cont");
            _status = VmStatus::Running;
        }
        auto nextConsole = std::chrono::steady_clock::now();
        while (!stop.stop_requested() && !process.Finished())
        {
            if (const auto output = process.ReadErrors(); !output.empty())
            {
                Append(Wide(output));
            }
            if (const auto output = process.ReadOutput(); !output.empty())
            {
                Append(Wide(output));
            }

            // QMP can disconnect before QEMU finishes flushing disks. Wait for the process exit code.
            if (quitRequested)
            {
                process.Finished(16ms);
                continue;
            }
            if (std::chrono::steady_clock::now() >= nextConsole)
            {
                try
                {
                    for (size_t i = 0; i < _ports.size(); ++i)
                    {
                        if (_ports[i])
                        {
                            ReadConsole(qmp, i);
                        }
                    }
                }
                catch (...)
                {
                    // The guest can power off between checking its process and polling a UART.
                    if (!quitRequested && !process.Finished(200ms))
                    {
                        throw;
                    }
                    continue;
                }
                nextConsole = std::chrono::steady_clock::now() + 100ms;
            }
            std::deque<Command> commands;
            {
                std::scoped_lock lock(_mutex);
                commands.swap(_commands);
                if (_saveRequest)
                {
                    save.swap(_saveRequest);
                }
            }
            for (auto& command : commands)
            {
                const bool quit = command.Name == "quit";
                try
                {
                    auto result = Execute(qmp, command);
                    quitRequested |= quit;
                    if (command.Reply)
                    {
                        command.Reply->set_value(std::move(result));
                    }
                }
                catch (const std::exception& error)
                {
                    if (command.Reply)
                    {
                        command.Reply->set_exception(std::current_exception());
                    }
                    else
                    {
                        Append(Wide(error.what()));
                    }
                }
            }
            if (save && !quitRequested)
            {
                bool pausedForSave = false;
                try
                {
                    if (qmp.Call("query-status").at("running").get<bool>())
                    {
                        qmp.Call("stop");
                        pausedForSave = true;
                    }
                    RunSnapshotJob(qmp, nullptr, _saveDrive);
                    qmp.Call("quit");
                    snapshotSaved = true;
                    quitRequested = true;
                }
                catch (...)
                {
                    const auto error = std::current_exception();
                    try
                    {
                        if (pausedForSave)
                        {
                            qmp.Call("cont");
                        }
                    }
                    catch (const std::exception& restart)
                    {
                        Append(Wide(restart.what()));
                    }
                    _status = qmp.Call("query-status").at("running").get<bool>() ? VmStatus::Running : VmStatus::Paused;
                    save->set_exception(error);
                    save.reset();
                }
            }
            if (!(quitRequested || process.Finished()))
            {
                try
                {
                    qmp.CheckEvents();
                }
                catch (const std::exception&)
                {
                    if (!quitRequested && !process.Finished(150ms))
                    {
                        throw;
                    }
                }
            }
            std::this_thread::sleep_for(16ms);
        }

        if (stop.stop_requested() && !process.Finished())
        {
            try
            {
                qmp.Call("quit", nullptr, 1s);
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (!process.Finished() && std::chrono::steady_clock::now() < deadline)
                {
                    process.ReadOutput();
                    if (const auto errors = process.ReadErrors(); !errors.empty())
                    {
                        Append(Wide(errors));
                    }
                    std::this_thread::sleep_for(16ms);
                }
            }
            catch (const std::exception&)
            {
            }
        }
        const auto errors = process.ReadErrors();
        if (!errors.empty())
        {
            Append(Wide(errors));
        }
        if (process.Finished() && process.ExitCode() != 0)
        {
            Fail(LFmt(IDS_ExitNotice, process.ExitCode()));
        }
    }
    catch (const std::exception& error)
    {
        failure = std::current_exception();
        Append(Wide(error.what()));
        finalStatus = stop.stop_requested() ? VmStatus::Stopped : VmStatus::Failed;
    }
    std::scoped_lock lock(_mutex);
    _status = finalStatus;
    _commands.clear();
    // The process can exit between the UI submitting a request and the worker dequeuing it.
    if (_saveRequest)
    {
        save.swap(_saveRequest);
    }
    if (save)
    {
        if (!failure && snapshotSaved && !stop.stop_requested())
        {
            save->set_value();
        }
        else
        {
            save->set_exception(
                failure ? failure : std::make_exception_ptr(std::runtime_error(Utf8(LText(IDS_SnapshotVmInactive)))));
        }
    }
}

void Session::RunDisplay(std::stop_token stop, Socket socket)
{
    try
    {
        Rfb display(std::move(socket), stop);
        while (!stop.stop_requested())
        {
            if (display.Readable())
            {
                if (display.Update())
                {
                    _frame.store(std::make_shared<const Frame>(display.Image()));
                }
                _absolutePointer = display.Absolute();
            }

            decltype(_input) input;
            {
                std::scoped_lock lock(_mutex);
                input.swap(_input);
            }
            for (const auto& event : input)
            {
                std::visit([&](const auto& value) { display.Send(value); }, event);
            }
        }
    }
    catch (const std::exception& error)
    {
        if (!stop.stop_requested())
        {
            Append(LFmt(IDS_DisplayError, Wide(error.what())));
        }
    }
}
} // namespace Ia64
