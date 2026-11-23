// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "MonitorWindow.h"
#include "Localization.h"

#include <algorithm>

namespace Ia64::Ui
{
namespace
{
enum Id
{
    Command = 206,
    Send,
    Log
};
constexpr int LogLimit = 200'000;
} // namespace

void MonitorWindow::Create()
{
    _log = Edit(Log, L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL);
    UseFont(_log, TextRole::Monospace);
    SendMessageW(_log, EM_SETLIMITTEXT, LogLimit, 0);
    _command = Edit(Command);
    UseFont(_command, TextRole::Monospace);
    if (_console == Console::Monitor)
    {
        EditCue(_command, LText(IDS_MonitorHint));
    }
    SendMessageW(_command, EM_SETLIMITTEXT, 4096, 0);
    _send = Button(Send, LText(IDS_Send), true);
    Enable(_send, _console != Console::Monitor && CanSend());
    Update();
}

void MonitorWindow::LanguageChanged()
{
    Set(_send, LText(IDS_Send));
    if (_console == Console::Monitor)
    {
        EditCue(_command, LText(IDS_MonitorHint));
    }
}

void MonitorWindow::Layout(int width, int height)
{
    const auto gap = _metrics.Gap;
    const int inputY = height - gap - _metrics.Control;
    const int buttonWidth = ButtonWidth(_send);
    Place(_log, gap, gap, width - gap * 2, inputY - gap * 2);
    Place(_command, gap, inputY, width - gap * 3 - buttonWidth, _metrics.Control);
    Place(_send, width - gap - buttonWidth, inputY, buttonWidth, _metrics.Control);
}

SIZE MonitorWindow::PreferredSize(SIZE available) const
{
    // Terminal output is naturally sized in character columns.
    constexpr int columns = 80;
    return {columns * _metrics.Character + _metrics.Gap * 2, available.cy / 3};
}

bool MonitorWindow::CanSend() const
{
    const auto status = _session.Status();
    return status == VmStatus::Running || status == VmStatus::Paused;
}

void MonitorWindow::Activate()
{
    Update();
    ShowWindow(_window, IsIconic(_window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(_window);
    SetFocus(CanSend() ? _command : _log);
}

void MonitorWindow::Update()
{
    const bool active = CanSend();
    if (bool(IsWindowEnabled(_command)) != active)
    {
        Enable(_command, active);
        Enable(_send, active && (_console != Console::Monitor || !Trim(Read(_command)).empty()));
        if (!active && IsWindowVisible(_window) && GetActiveWindow() == _window)
        {
            SetFocus(_log);
        }
    }
    auto output = _session.ConsumeLog(_console);
    if (output.empty())
    {
        return;
    }

    const int length = GetWindowTextLengthW(_log);
    DWORD first{}, last{};
    SendMessageW(_log, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
    SCROLLINFO scroll{sizeof(scroll), SIF_ALL};
    GetScrollInfo(_log, SB_VERT, &scroll);
    const bool follow = first == last && scroll.nPos + static_cast<int>(scroll.nPage) >= scroll.nMax;
    const int firstLine = static_cast<int>(SendMessageW(_log, EM_GETFIRSTVISIBLELINE, 0, 0));
    int removed = std::max(0, length + static_cast<int>(output.size()) - LogLimit);
    int removedLines = 0;
    if (removed)
    {
        removedLines = static_cast<int>(SendMessageW(_log, EM_LINEFROMCHAR, removed, 0));
        const int nextLine = static_cast<int>(SendMessageW(_log, EM_LINEINDEX, removedLines + 1, 0));
        if (nextLine >= 0)
        {
            removed = nextLine;
            ++removedLines;
        }
        SendMessageW(_log, EM_SETSEL, 0, removed);
        SendMessageW(_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    SendMessageW(_log, EM_SETSEL, length - removed, length - removed);
    SendMessageW(_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(output.c_str()));
    if (follow)
    {
        SendMessageW(_log, EM_SCROLLCARET, 0, 0);
    }
    else
    {
        SendMessageW(_log, EM_SETSEL, std::max(0, static_cast<int>(first) - removed),
                     std::max(0, static_cast<int>(last) - removed));
        const auto now = SendMessageW(_log, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(_log, EM_LINESCROLL, 0, std::max(0, firstLine - removedLines) - now);
    }
}

void MonitorWindow::SendCommand()
{
    const auto command = _console == Console::Monitor ? Trim(Read(_command)) : Read(_command);
    if (CanSend() && (_console != Console::Monitor || !command.empty()))
    {
        if (_console == Console::Monitor)
        {
            _session.Monitor(command);
        }
        else
        {
            _session.ConsoleInput(_console, command);
        }
        Set(_command, L"");
        SetFocus(_command);
    }
}

LRESULT MonitorWindow::Message(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == DM_GETDEFID && GetFocus() == _command)
    {
        return MAKELRESULT(Send, DC_HASDEFID);
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL))
    {
        ShowWindow(_window, SW_HIDE);
        return 0;
    }
    if (message == WM_COMMAND)
    {
        if (LOWORD(wparam) == Command && HIWORD(wparam) == EN_CHANGE)
        {
            Enable(_send, CanSend() && (_console != Console::Monitor || !Trim(Read(_command)).empty()));
        }
        if (LOWORD(wparam) == Send && HIWORD(wparam) == BN_CLICKED)
        {
            SendCommand();
        }
        return 0;
    }
    return Window::Message(message, wparam, lparam);
}
} // namespace Ia64::Ui
