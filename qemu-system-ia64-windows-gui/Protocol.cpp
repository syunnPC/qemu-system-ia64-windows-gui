// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Protocol.h"
#include "Localization.h"

#include <WS2tcpip.h>
#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace Ia64
{
Network::Network()
{
    WSADATA data{};
    if (const auto error = WSAStartup(MAKEWORD(2, 2), &data); error)
    {
        WinError(L"WSAStartup", static_cast<DWORD>(error));
    }
}

Network::~Network()
{
    WSACleanup();
}

Socket::Socket(Socket&& other) noexcept : _value(std::exchange(other._value, INVALID_SOCKET))
{
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _value = std::exchange(other._value, INVALID_SOCKET);
    }
    return *this;
}

Socket::~Socket()
{
    Reset();
}

void Socket::Reset() noexcept
{
    if (*this)
    {
        closesocket(std::exchange(_value, INVALID_SOCKET));
    }
}

bool Socket::Ready(std::chrono::milliseconds timeout) const
{
    fd_set sockets{};
    FD_SET(_value, &sockets);
    const auto milliseconds = static_cast<long>(timeout.count());
    timeval time{milliseconds / 1000, milliseconds % 1000 * 1000};
    const int result = select(0, &sockets, nullptr, nullptr, &time);
    if (result == SOCKET_ERROR)
    {
        WinError(LText(IDS_SocketWait), static_cast<DWORD>(WSAGetLastError()));
    }
    return result > 0;
}

size_t Socket::Receive(std::span<char> buffer) const
{
    const auto size = static_cast<int>(std::min<size_t>(buffer.size(), 64 * 1024));
    const int count = recv(_value, buffer.data(), size, 0);
    if (count == SOCKET_ERROR)
    {
        WinError(LText(IDS_QemuRead), static_cast<DWORD>(WSAGetLastError()));
    }
    if (!count)
    {
        Fail(LText(IDS_QemuDisconnected));
    }
    return static_cast<size_t>(count);
}

void Socket::Send(std::string_view data) const
{
    while (!data.empty())
    {
        const auto size = static_cast<int>(std::min<size_t>(data.size(), 64 * 1024));
        const int sent = send(_value, data.data(), size, 0);
        if (sent == SOCKET_ERROR || !sent)
        {
            WinError(LText(IDS_QemuWrite), static_cast<DWORD>(WSAGetLastError()));
        }
        data.remove_prefix(static_cast<size_t>(sent));
    }
}

void Socket::ReadExact(std::span<char> buffer, std::stop_token stop, std::chrono::milliseconds timeout) const
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!buffer.empty())
    {
        if (stop.stop_requested())
        {
            Fail(LText(IDS_ConnectionClosed));
        }
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining <= 0ms)
        {
            Fail(LText(IDS_ConnectionReadTimeout));
        }
        if (Ready(std::min(remaining, 100ms)))
        {
            buffer = buffer.subspan(Receive(buffer));
        }
    }
}

Listener::Listener(Mode mode) : _socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
{
    if (!_socket)
    {
        WinError(LText(IDS_SocketCreate), static_cast<DWORD>(WSAGetLastError()));
    }
    const BOOL exclusive = TRUE;
    if (setsockopt(_socket.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive)) != 0)
    {
        WinError(LText(IDS_SocketOwnership), static_cast<DWORD>(WSAGetLastError()));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(_socket.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        WinError(LText(IDS_SocketListen), static_cast<DWORD>(WSAGetLastError()));
    }

    int length = sizeof(address);
    if (getsockname(_socket.Get(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        WinError(LText(IDS_SocketAddress), static_cast<DWORD>(WSAGetLastError()));
    }
    _port = ntohs(address.sin_port);
    if (mode == Mode::Listen)
    {
        Listen();
    }
}

void Listener::Listen()
{
    if (listen(_socket.Get(), 1) != 0)
    {
        WinError(LText(IDS_SocketListen), static_cast<DWORD>(WSAGetLastError()));
    }
}

Socket Listener::Accept(Process& process, std::stop_token stop, std::string* errors)
{
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    std::string diagnostics;
    do
    {
        if (_socket.Ready(50ms))
        {
            Socket client(accept(_socket.Get(), nullptr, nullptr));
            if (!client)
            {
                WinError(LText(IDS_SocketAccept), static_cast<DWORD>(WSAGetLastError()));
            }
            _socket.Reset();
            const DWORD timeout = 2000;
            setsockopt(client.Get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            if (errors)
            {
                *errors += diagnostics;
            }
            return client;
        }
        if (diagnostics.size() < 64 * 1024)
        {
            diagnostics += process.ReadErrors();
        }
        if (process.Finished())
        {
            Fail(LFmt(IDS_QemuExited, process.ExitCode(), Trim(Wide(diagnostics))));
        }
    } while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline);
    Fail(LFmt(IDS_QemuUnreachable, Trim(Wide(diagnostics))));
}

Qmp::Qmp(Socket socket, EventHandler events) : _socket(std::move(socket)), _events(std::move(events))
{
    const auto greeting = Next(10s);
    if (!greeting || !greeting->contains("QMP"))
    {
        Fail(LText(IDS_NoGreeting));
    }
    Call("qmp_capabilities");
}

std::optional<Json> Qmp::Next(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 8192> buffer{};
    for (;;)
    {
        const auto newline = _pending.find('\n');
        if (newline != std::string::npos)
        {
            auto packet = Json::parse(_pending.begin(), _pending.begin() + newline);
            _pending.erase(0, newline + 1);
            return packet;
        }
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (!_socket.Ready(std::max(0ms, remaining)))
        {
            return std::nullopt;
        }
        const auto count = _socket.Receive(buffer);
        _pending.append(buffer.data(), count);
        if (_pending.size() > 4 * 1024 * 1024)
        {
            Fail(LText(IDS_QmpOversized));
        }
    }
}

Json Qmp::Call(std::string_view command, Json arguments, std::chrono::milliseconds timeout)
{
    const auto sequence = ++_sequence;
    Json request{{"execute", command}, {"id", sequence}};
    if (!arguments.is_null())
    {
        request["arguments"] = std::move(arguments);
    }
    _socket.Send(request.dump() + "\n");

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto response = Next(remaining);
        if (!response)
        {
            break;
        }
        if (response->contains("event"))
        {
            if (_events)
            {
                _events(*response);
            }
            continue;
        }
        if (response->value("id", 0u) != sequence)
        {
            continue;
        }
        if (response->contains("error"))
        {
            Fail(Wide(response->at("error").value("desc", Utf8(LText(IDS_QmpError)))));
        }
        return response->at("return");
    }
    Fail(LFmt(IDS_QmpTimeout, Wide(command)));
}

void Qmp::CheckEvents()
{
    for (int count = 0; count < 16; ++count)
    {
        auto response = Next(0ms);
        if (!response)
        {
            break;
        }
        if (response->contains("event") && _events)
        {
            _events(*response);
        }
    }
}

std::wstring QmpAddress(const Listener& listener)
{
    return std::format(L"tcp:127.0.0.1:{},server=off", listener.Port());
}
} // namespace Ia64
