// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <WinSock2.h>
#include "Platform.h"
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>

namespace Ia64
{
using Json = nlohmann::json;

class Network
{
public:
    Network();
    ~Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
};

class Socket
{
public:
    Socket() = default;
    explicit Socket(SOCKET value) : _value(value)
    {
    }
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket();
    SOCKET Get() const
    {
        return _value;
    }
    explicit operator bool() const
    {
        return _value != INVALID_SOCKET;
    }
    void Reset() noexcept;
    bool Ready(std::chrono::milliseconds timeout) const;
    size_t Receive(std::span<char> buffer) const;
    void Send(std::string_view data) const;
    void ReadExact(std::span<char> buffer, std::stop_token stop, std::chrono::milliseconds timeout = 10s) const;

private:
    SOCKET _value{INVALID_SOCKET};
};

class Listener
{
public:
    enum class Mode
    {
        Listen,
        Reserve
    };
    explicit Listener(Mode mode = Mode::Listen);
    void Listen();
    unsigned short Port() const
    {
        return _port;
    }
    Socket Accept(Process& process, std::stop_token stop = {}, std::string* errors = nullptr);

private:
    Socket _socket;
    unsigned short _port{};
};

struct VmConnections
{
    Listener Control{Listener::Mode::Reserve};
    Listener Video{Listener::Mode::Reserve};
};

class Qmp
{
public:
    using EventHandler = std::function<void(const Json&)>;
    explicit Qmp(Socket socket, EventHandler events = {});
    Json Call(std::string_view command, Json arguments = nullptr, std::chrono::milliseconds timeout = 8s);
    void CheckEvents();

private:
    std::optional<Json> Next(std::chrono::milliseconds timeout);
    Socket _socket;
    std::string _pending;
    EventHandler _events;
    unsigned _sequence{};
};

std::wstring QmpAddress(const Listener& listener);
} // namespace Ia64
