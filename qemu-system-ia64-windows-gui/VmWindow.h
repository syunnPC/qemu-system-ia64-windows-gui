// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "MonitorWindow.h"
#include <bitset>

namespace Ia64::Ui
{
inline constexpr UINT VmStateChanged = WM_APP + 2;
inline constexpr UINT ViewVmSettings = WM_APP + 5;
int StatusText(VmStatus status);

class VmWindow final : public Window
{
public:
    VmWindow(HWND manager, const InterfaceSettings& settings, Profile profile, const Machine& machine,
             const Capabilities& capabilities, ProcessCommand command, VmConnections connections,
             std::optional<VmSnapshot> resume, Engine engine, std::function<void(const Profile&)> saveProfile);
    const std::wstring& Id() const
    {
        return _profile.Id;
    }
    VmStatus Status() const
    {
        return _status;
    }
    void ViewSettings(HWND owner) const;
    void SetInterface(const InterfaceSettings& settings);
    bool UsesNvram(const Fs::path& path) const;
    void SetAppearance(Appearance appearance) override;

private:
    void Create() override;
    void Layout(int width, int height) override;
    SIZE PreferredSize(SIZE available) const override;
    void Paint(HDC dc, const RECT& area) override;
    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) override;
    void Tick();
    void LanguageChanged() override;
    DisplaySettings Display() const
    {
        return _profile.DisplayOverride.value_or(_defaultDisplay);
    }
    void ResizeToGuest();
    int LayoutToolbar(int width) const;
    RECT Letterbox() const;
    bool CursorOverGuest(POINT location) const;
    void RefreshHostCursor() const;
    void OpenMonitor();
    void OpenPort(Console console);
    void DevicesMenu(HMENU menu, std::vector<std::function<void()>>& actions, bool active);
    void ChangeMedia(const Drive& drive, bool eject);
    void AttachDisk(std::wstring controller);
    void DetachDisk(const Drive& drive);
    void SetInput(bool ps2);
    void Commit(Profile next);
    void HandleAction(int command);
    void OpenMenu(int group);
    void RunCommand(int command);
    void Close(bool saveSnapshot);
    void SendPointer(int x, int y, bool movement, unsigned wheel = 0);
    void SendKey(WPARAM key, LPARAM data, bool down);
    void UpdateKeyboardCapture();
    void DropKeyboardCapture();
    static LRESULT CALLBACK KeyboardEvent(int code, WPARAM message, LPARAM data);
    void CapturePointer();
    void ReleaseInput();

    HWND _manager;
    Profile _profile;
    Fs::path _nvram;
    Engine _engine;
    Machine _machine;
    Capabilities _capabilities;
    DisplaySettings _defaultDisplay;
    bool _closeOnShutdown;
    bool _pendingShutdownClose{};
    bool _resizePending = true;
    bool _inAction{};
    int _chromeHeight{};
    bool _hasPs2{}, _hasVideo{};
    std::function<void(const Profile&)> _saveProfile;
    Session _session;
    std::unique_ptr<MonitorWindow> _monitor, _serial, _debug;
    std::map<std::string, bool> _links;
    VmStatus _status{VmStatus::Starting};
    std::shared_ptr<const Frame> _frame;
    std::map<unsigned, KeyInput> _keys;
    unsigned _buttons{};
    bool _captured{};
    HHOOK _keyboardHook{};
    std::bitset<512> _intercepted;
    WORD _keyboardEpoch{};
    bool _closing{};
    bool _pendingFailure{};
    POINT _captureCenter{};
    POINT _savedPointer{};
    RECT _view{};
    int _width{}, _height{};
    HWND _toolbar{};
    HWND _runStatus{}, _resolution{}, _captureHint{};
};
} // namespace Ia64::Ui
