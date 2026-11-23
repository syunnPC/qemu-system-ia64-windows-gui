// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Session.h"
#include "UI.h"

namespace Ia64::Ui
{
class MonitorWindow final : public Window
{
public:
    MonitorWindow(Appearance appearance, Session& session, Console console = Console::Monitor)
        : Window(appearance), _session(session), _console(console)
    {
    }
    void Activate();
    void Update();

private:
    void Create() override;
    void LanguageChanged() override;
    void Layout(int width, int height) override;
    void Paint(HDC, const RECT&) override
    {
    }
    POINT MinimumSize() const override
    {
        return {_metrics.Character * 40, _metrics.Line * 10};
    }
    SIZE PreferredSize(SIZE available) const override;
    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override;
    void SendCommand();
    bool CanSend() const;
    Session& _session;
    Console _console;
    HWND _log{}, _command{}, _send{};
};
} // namespace Ia64::Ui
