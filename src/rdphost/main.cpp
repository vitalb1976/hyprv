// hyprv-rdphost: out-of-process host for the MsTscAx.MsTscAx.9 ActiveX control.
//
// Spawned by hyprv.exe with `--pipe=<guid>`. Connects to that pipe, completes the
// Hello handshake, creates the frameless top-level window + ATL ActiveX host,
// sends HwndReady so the parent can SetParent + AttachThreadInput, then dispatches
// inbound IPC commands (Connect/Disconnect/Resize/UpdateSessionDisplaySettings/...)
// to the RdpHost helper that wraps the ActiveX.

#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <atlhost.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "RdpIpc.h"
#include "IpcClient.h"
#include "RdpHost.h"

// rdphost logging now travels back to the parent over IPC so we end up with
// one unified hyprv.log instead of two separate files. The parent prefixes
// each line with "[rdphost pid=N]" before writing.
//
// Set g_ipcLogger after the IPC handshake completes — anything that logs
// before that (or after Stop()) falls back to OutputDebugStringW so dev-
// build diagnostics aren't lost. Concurrency: Send() inside IpcClient is
// already mutex-protected, but we still guard the format buffer with our
// own lock so two threads can't interleave wide → utf8 conversion buffers.
static hyprv::rdphost::IpcClient* g_ipcLogger = nullptr;
static std::mutex                 g_logMutex;
void HyprvLog(const wchar_t* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    int wlen = _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (wlen <= 0) return;

    if (g_ipcLogger)
    {
        // UTF-16 → UTF-8 for the wire — keeps payload bytes small and matches
        // the rest of our string traffic.
        int nb = WideCharToMultiByte(CP_UTF8, 0, buf, wlen,
                                     nullptr, 0, nullptr, nullptr);
        if (nb <= 0) return;
        std::vector<uint8_t> payload(sizeof(hyprv::ipc::LogLine) + nb);
        auto* hdr = reinterpret_cast<hyprv::ipc::LogLine*>(payload.data());
        hdr->level    = 1;   // info — we don't categorize yet
        hdr->byteLen  = static_cast<uint32_t>(nb);
        WideCharToMultiByte(CP_UTF8, 0, buf, wlen,
            reinterpret_cast<char*>(payload.data() + sizeof(hyprv::ipc::LogLine)),
            nb, nullptr, nullptr);
        g_ipcLogger->Send(hyprv::ipc::C2P::LogLine,
            payload.data(), static_cast<uint32_t>(payload.size()));
    }
    else
    {
        // Pre-handshake / post-disconnect — debugger gets the message via
        // OutputDebugString, the user's hyprv.log gets nothing for this
        // window. The window is tiny (a few startup lines only).
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\n");
    }
}

// Static-linked ATL needs a module instance for AtlAxWinInit() to work.
class CHyprvRdphostModule : public ATL::CAtlExeModuleT<CHyprvRdphostModule> {};
static CHyprvRdphostModule _AtlModule;

using hyprv::rdphost::IpcClient;
using hyprv::rdphost::IpcMessage;
using hyprv::rdphost::RdpHost;
namespace ipc = hyprv::ipc;

namespace
{
    // Private window messages used to marshal IPC events onto the UI thread.
    constexpr UINT WM_HYPRV_IPC_RECEIVED      = WM_USER + 100;
    constexpr UINT WM_HYPRV_PIPE_DISCONNECTED = WM_USER + 101;

    HWND     g_top  = nullptr;
    RdpHost* g_host = nullptr;

    constexpr UINT WM_HYPRV_RELEASE_FOCUS = WM_USER + 102;
    constexpr UINT WM_HYPRV_MODIFIER_TRACE = WM_USER + 103;
    HHOOK g_keyboardHook = nullptr;
    uint32_t g_releaseShortcut = 7; // Ctrl+Alt+Shift, no terminal key.
    bool g_keys[256]{};
    bool g_releaseArmed = false;
    bool g_terminalConsumed = false;
    bool g_chordCancelled = false;

    // Focus may be reported on a XAML input sink in the owner process
    // because its input queue is attached to ours. A visible session in the
    // foreground owner chain is sufficient; hidden/background tabs must not act.
    bool SessionHasKeyboardFocus()
    {
        if (!g_top || !IsWindowVisible(g_top)) return false;
        HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        HWND owner = GetWindow(g_top, GW_OWNER);
        return foreground == g_top || (owner && foreground == GetAncestor(owner, GA_ROOT));
    }

    uint32_t ReleaseModifiers()
    {
        return ((g_keys[VK_LCONTROL] || g_keys[VK_RCONTROL] || g_keys[VK_CONTROL]) ? 1u : 0u)
            | ((g_keys[VK_LMENU] || g_keys[VK_RMENU] || g_keys[VK_MENU]) ? 2u : 0u)
            | ((g_keys[VK_LSHIFT] || g_keys[VK_RSHIFT] || g_keys[VK_SHIFT]) ? 4u : 0u);
    }

    LRESULT CALLBACK ReleaseKeyboardHook(int code, WPARAM message, LPARAM data)
    {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, message, data);
        const auto& key = *reinterpret_cast<KBDLLHOOKSTRUCT const*>(data);
        if (key.vkCode >= 256)
            return CallNextHookEx(nullptr, code, message, data);
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
        if (!down && !up) return CallNextHookEx(nullptr, code, message, data);
        g_keys[key.vkCode] = down;
        const auto mods = ReleaseModifiers();
        const auto wantedMods = g_releaseShortcut & 7u;
        const auto terminal = (g_releaseShortcut >> 8) & 255u;
        const bool modifier = key.vkCode == VK_CONTROL || key.vkCode == VK_LCONTROL || key.vkCode == VK_RCONTROL
            || key.vkCode == VK_MENU || key.vkCode == VK_LMENU || key.vkCode == VK_RMENU
            || key.vkCode == VK_SHIFT || key.vkCode == VK_LSHIFT || key.vkCode == VK_RSHIFT;
        const bool sessionActive = SessionHasKeyboardFocus();
        // Do not discard injected input: host-side remote-control software
        // can deliver legitimate shortcut keystrokes using SendInput.
        // Trace only modifier masks, never ordinary typed keys, outside the hook.
        if (modifier)
            PostMessageW(g_top, WM_HYPRV_MODIFIER_TRACE, mods,
                (sessionActive ? 1 : 0) | ((key.flags & LLKHF_INJECTED) ? 2 : 0));
        if (!sessionActive)
        {
            g_releaseArmed = false;
            g_terminalConsumed = false;
            g_chordCancelled = false;
            return CallNextHookEx(nullptr, code, message, data);
        }
        // Keep guest modifier key-ups intact. Only consume the optional terminal
        // key, and transfer focus after every key in the chord has been released.
        bool consume = terminal != 0 && key.vkCode == terminal && g_terminalConsumed;
        if (down && !modifier && key.vkCode != terminal) g_chordCancelled = true;
        if (down && (mods & ~wantedMods) != 0) g_chordCancelled = true;
        if (down && !g_chordCancelled && mods == wantedMods && wantedMods != 0
            && (terminal == 0 || (down && key.vkCode == terminal)))
        {
            g_releaseArmed = true;
            if (terminal != 0) { g_terminalConsumed = true; consume = true; }
        }
        if (mods == 0 && (terminal == 0 || !g_keys[terminal]))
        {
            if (g_releaseArmed && !g_chordCancelled)
                PostMessageW(g_top, WM_HYPRV_RELEASE_FOCUS, 0, 0);
            g_releaseArmed = false;
            g_terminalConsumed = false;
            g_chordCancelled = false;
        }
        return consume ? 1 : CallNextHookEx(nullptr, code, message, data);
    }

    // Decode the trailing UTF-8 server/domain/username bytes that follow an
    // RdpOptions payload (in that order). username is v3 — absent (userByteLen=0)
    // for VM-console connections, present for generic Remote Host connections.
    void ExtractRdpStrings(const ipc::RdpOptions& opt, const uint8_t* extra, size_t extraLen,
                           std::string& outServer, std::string& outDomain, std::string& outUser)
    {
        const auto need = static_cast<size_t>(opt.serverByteLen) + opt.domainByteLen
                        + opt.userByteLen;
        if (need > extraLen) return;
        outServer.assign(reinterpret_cast<const char*>(extra), opt.serverByteLen);
        outDomain.assign(reinterpret_cast<const char*>(extra + opt.serverByteLen),
                         opt.domainByteLen);
        outUser.assign(reinterpret_cast<const char*>(
            extra + opt.serverByteLen + opt.domainByteLen), opt.userByteLen);
    }

    void DispatchIpcMessage(IpcMessage& msg)
    {
        if (!g_host) return;
        const auto* payload = msg.payload.data();
        const auto  size    = msg.payload.size();

        switch (static_cast<ipc::P2C>(msg.header.type))
        {
        case ipc::P2C::SetReleaseShortcut:
            if (size == sizeof(uint32_t))
            {
                uint32_t shortcut = 0;
                memcpy(&shortcut, payload, sizeof(shortcut));
                if ((shortcut & ~0xFF07u) == 0 && (shortcut & 7u) != 0)
                {
                    g_releaseShortcut = shortcut;
                    HyprvLog(L"[focus] configured modifiers=%u key=%u", shortcut & 7u, shortcut >> 8);
                }
            }
            break;
        case ipc::P2C::Connect:
        {
            if (size < sizeof(ipc::Connect)) break;
            ipc::Connect cmd{};
            memcpy(&cmd, payload, sizeof(cmd));
            std::string server, domain, user;
            ExtractRdpStrings(cmd.options, payload + sizeof(cmd), size - sizeof(cmd),
                              server, domain, user);
            g_host->OnConnect(cmd, server, domain, user);
            break;
        }
        case ipc::P2C::ConnectLocalVm:
        {
            if (size < sizeof(ipc::ConnectLocalVm)) break;
            ipc::ConnectLocalVm cmd{};
            memcpy(&cmd, payload, sizeof(cmd));
            std::string server, domain, user;
            ExtractRdpStrings(cmd.options, payload + sizeof(cmd), size - sizeof(cmd),
                              server, domain, user);
            g_host->OnConnectLocalVm(cmd, server, domain);
            break;
        }
        case ipc::P2C::Disconnect:
            g_host->OnDisconnect();
            break;
        case ipc::P2C::Shutdown:
            PostQuitMessage(0);
            break;
        case ipc::P2C::UpdateSessionDisplaySettings:
        {
            if (size < sizeof(ipc::UpdateSessionDisplaySettings)) break;
            ipc::UpdateSessionDisplaySettings cmd{};
            memcpy(&cmd, payload, sizeof(cmd));
            g_host->OnUpdateSessionDisplaySettings(cmd);
            break;
        }
        case ipc::P2C::SetEnhanced:
        {
            if (size < sizeof(ipc::SetEnhanced)) break;
            ipc::SetEnhanced cmd{};
            memcpy(&cmd, payload, sizeof(cmd));
            g_host->OnSetEnhanced(cmd.enabled != 0);
            break;
        }
        case ipc::P2C::SendCtrlAltDel:
            g_host->OnSendCtrlAltDel();
            break;
        case ipc::P2C::Resize:
            // No-op when in enhanced session — parent will also send
            // UpdateSessionDisplaySettings. For non-enhanced sessions this is just
            // a hint; the AtlAxWin gets resized by WndProc's WM_SIZE handler anyway.
            break;
        case ipc::P2C::TypeText:
            // Parent handles TypeText directly via WMI (IMsvm_Keyboard). The control
            // doesn't expose a string-input API, and we don't pull WMI into rdphost.
            break;
        default:
            break;
        }
    }

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_SIZE:
        {
            const int newW = LOWORD(l);
            const int newH = HIWORD(l);
            HWND ax = GetWindow(h, GW_CHILD);
            HyprvLog(L"[wnd] WM_SIZE popup=%dx%d ax=%p", newW, newH, ax);
            if (ax)
            {
                MoveWindow(ax, 0, 0, newW, newH, TRUE);
                RECT axr{};
                GetWindowRect(ax, &axr);
                HyprvLog(L"[wnd]   AtlAxWin moved -> %dx%d (rect %d,%d-%d,%d)",
                    newW, newH, axr.left, axr.top, axr.right, axr.bottom);
            }
            return 0;
        }

        case WM_HYPRV_MODIFIER_TRACE:
            HyprvLog(L"[focus] modifiers=%u sessionActive=%d injected=%d",
                static_cast<unsigned>(w), (l & 1) != 0, (l & 2) != 0);
            return 0;
        case WM_HYPRV_RELEASE_FOCUS:
        {
            // A posted message keeps pipe I/O outside the low-level hook.
            // Ignore stale requests if focus has already left this session.
            const bool focused = SessionHasKeyboardFocus();
            HyprvLog(L"[focus] chord released; sessionFocused=%d", focused);
            if (focused && g_ipcLogger)
            {
                DWORD ownerPid = 0;
                GetWindowThreadProcessId(GetWindow(h, GW_OWNER), &ownerPid);
                ReleaseCapture();
                ClipCursor(nullptr);
                if (ownerPid) AllowSetForegroundWindow(ownerPid);
                const bool sent = g_ipcLogger->Send(ipc::C2P::ReleaseFocus, nullptr, 0);
                HyprvLog(L"[focus] ReleaseFocus IPC sent=%d", sent);
            }
            return 0;
        }
        case WM_SETFOCUS:
            if (HWND ax = GetWindow(h, GW_CHILD)) SetFocus(ax);
            return 0;

        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;

        case WM_HYPRV_IPC_RECEIVED:
        {
            std::unique_ptr<IpcMessage> msg(reinterpret_cast<IpcMessage*>(w));
            DispatchIpcMessage(*msg);
            return 0;
        }

        case WM_HYPRV_PIPE_DISCONNECTED:
            HyprvLog(L"[main] WM_HYPRV_PIPE_DISCONNECTED — PostQuitMessage(1)");
            PostQuitMessage(1);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    std::wstring ParsePipeArg(PCWSTR cmdLine)
    {
        if (!cmdLine) return {};
        std::wstring s = cmdLine;
        const auto p = s.find(L"--pipe=");
        if (p == std::wstring::npos) return {};
        const auto start = p + 7;
        const auto end = s.find(L' ', start);
        return (end == std::wstring::npos) ? s.substr(start) : s.substr(start, end - start);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR cmdLine, int)
{
    // No local log file — logging is forwarded over IPC to the parent's
    // unified hyprv.log after the handshake completes (set g_ipcLogger
    // below). Pre-handshake messages route to OutputDebugString.
    HyprvLog(L"[main] pid=%lu starting", GetCurrentProcessId());

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (!AtlAxWinInit()) return 2;

    const auto pipeId = ParsePipeArg(cmdLine);
    if (pipeId.empty())
    {
        HyprvLog(L"[main] missing --pipe=<id>");
        return 3;
    }

    IpcClient ipc(std::wstring(ipc::kPipeNamePrefix) + pipeId);
    if (!ipc.Connect(5000))   { HyprvLog(L"[main] pipe connect failed"); return 4; }
    if (!ipc.Handshake())     { HyprvLog(L"[main] handshake failed");    return 5; }
    // NOTE: don't set g_ipcLogger yet. The parent runs SpawnAndHandshake
    // SYNCHRONOUSLY — it reads HelloAck (just sent), then keeps reading
    // until HwndReady arrives. If we start sending LogLine frames between
    // those two messages, the parent's blocking ReadExact swallows them
    // as if they were HwndReady, sees a type mismatch, and bails with
    // "ConnectLocalVm setup failed". g_ipcLogger gets set below, AFTER
    // HwndReady has been sent and the parent has switched to its async
    // rx thread which knows how to dispatch LogLine.
    HyprvLog(L"[main] pipe connected, handshake ok");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"HyprvRdphostHost";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);

    g_top = CreateWindowExW(0, wc.lpszClassName, L"hyprv-rdphost",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1024, 768, nullptr, nullptr, hInstance, nullptr);
    if (!g_top) return 6;
    {
        RECT pr{};
        GetWindowRect(g_top, &pr);
        HyprvLog(L"[main] popup created hwnd=%p rect=%d,%d-%d,%d (%dx%d)",
            g_top, pr.left, pr.top, pr.right, pr.bottom,
            pr.right - pr.left, pr.bottom - pr.top);
    }

    RdpHost host(ipc);
    // Create AtlAxWin at the popup's actual client size — not the legacy
    // hardcoded 800x600. mstscax renders into the AtlAxWin's client area,
    // so AtlAxWin size IS the visible session render area. The popup was
    // just created at 1024x768 (or whatever the popup defaults to); the
    // parent's EmbedInto will only fire WM_SIZE if it changes the SIZE,
    // and the first EmbedInto typically preserves the popup size (same
    // 1024x768, just at a different screen position). Without matching
    // sizes here, AtlAxWin stays at 800x600 while popup is 1024x768 —
    // the "wonky offset" the user has been hitting.
    RECT clientRect{};
    GetClientRect(g_top, &clientRect);
    const int popupClientW = clientRect.right  - clientRect.left;
    const int popupClientH = clientRect.bottom - clientRect.top;
    HyprvLog(L"[main] activating AtlAxWin at popup client size %dx%d",
        popupClientW, popupClientH);
    if (!host.Activate(g_top, popupClientW, popupClientH))
    {
        HyprvLog(L"[main] RdpHost activation failed");
        return 7;
    }
    {
        // AtlAxWin is the first child of g_top after Activate.
        HWND ax = GetWindow(g_top, GW_CHILD);
        RECT axr{};
        if (ax) GetWindowRect(ax, &axr);
        HyprvLog(L"[main] AtlAxWin created hwnd=%p rect=%d,%d-%d,%d (%dx%d)",
            ax, axr.left, axr.top, axr.right, axr.bottom,
            axr.right - axr.left, axr.bottom - axr.top);
    }
    g_host = &host;
    HyprvLog(L"[main] RdpHost activated; sending HwndReady");

    // Do NOT ShowWindow here. The parent (hyprv.exe / test harness) re-parents
    // the popup into its own visible host and then calls ShowWindow itself. If
    // we show first, the WS_POPUP→WS_CHILD style flip mid-lifecycle has been
    // observed to confuse mstscax — events fire 27, 12, then stall.
    ipc::HwndReady ready{ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_top)) };
    ipc.Send(ipc::C2P::HwndReady, &ready, sizeof(ready));

    ipc.StartReceiveLoop(
        [](IpcMessage* msg) { PostMessageW(g_top, WM_HYPRV_IPC_RECEIVED, reinterpret_cast<WPARAM>(msg), 0); },
        []                  { PostMessageW(g_top, WM_HYPRV_PIPE_DISCONNECTED, 0, 0); });

    // Parent has completed SpawnAndHandshake (because it consumed HwndReady)
    // and its rx thread is now running, so it can dispatch LogLine frames.
    // Safe to flip logging over IPC from this point.
    g_ipcLogger = &ipc;

    for (int key = 0; key < 256; ++key)
        g_keys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    g_keys[VK_CONTROL] = g_keys[VK_MENU] = g_keys[VK_SHIFT] = false;
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, ReleaseKeyboardHook, hInstance, 0);
    if (!g_keyboardHook) HyprvLog(L"[focus] keyboard hook failed: %lu", GetLastError());
    else HyprvLog(L"[focus] keyboard hook installed");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Stop logging via IPC BEFORE we tear the pipe down — otherwise late
    // log calls during shutdown would dereference a half-destroyed IpcClient.
    // From here on logs route to OutputDebugStringW.
    if (g_keyboardHook) { UnhookWindowsHookEx(g_keyboardHook); g_keyboardHook = nullptr; }
    g_ipcLogger = nullptr;
    ipc.Stop();
    g_host = nullptr;
    if (g_top) { DestroyWindow(g_top); g_top = nullptr; }
    CoUninitialize();
    return 0;
}
