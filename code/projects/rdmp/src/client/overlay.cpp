/*
 * In-game overlay: see overlay.h. Threads:
 *   render thread  - PresentHook / ExecuteCommandListsHook (the game's own threads)
 *   main thread    - Frame() (from rdmp.dll's per-frame pump), RenderInit, ImGui Update via Instance::Update
 *   window thread  - WndProc (the thread that created the game window)
 */

#include <utils/safe_win32.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <MinHook.h>
#include <imgui.h>

#include <graphics/backend/d3d12.h>
#include <gui/manager.h>
#include <graphics/renderer.h>
#include <external/imgui/wrapper.h>

#include "client/fw_client.h"
#include "client/overlay.h"
#include "shared/fw_api.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace RDMP::Overlay {
    namespace {
        using PresentFn  = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT);
        using ExecuteFn  = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
        using DiStateFn  = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8W *, DWORD, LPVOID);
        using DiDataFn   = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8W *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

        FwClient *g_client               = nullptr;
        const rdmp::fw::Host *g_host     = nullptr;
        PresentFn g_origPresent          = nullptr;
        ExecuteFn g_origExecute          = nullptr;
        WNDPROC g_origWndProc            = nullptr;
        DiStateFn g_origDiState          = nullptr;
        DiDataFn g_origDiData            = nullptr;

        // What the render thread saw, for the main thread to bring the renderer up with.
        std::mutex g_captureMutex;
        IDXGISwapChain3 *g_swapChain     = nullptr;
        ID3D12CommandQueue *g_queue      = nullptr;
        HWND g_window                    = nullptr;
        int g_stablePresents             = 0; // Presents in a row on the same swapchain + queue
        constexpr int kStablePresents    = 120; // the game recreates its swapchain while starting up
        thread_local ID3D12CommandQueue *t_lastDirectQueue = nullptr;

        std::atomic_bool g_ready {false};
        std::atomic_bool g_failed {false};
        std::atomic_bool g_cursor {false};       // a script UI's cursor
        std::atomic_bool g_menu {false};         // the main menu is up
        std::atomic_bool g_captureEscape {false};
        std::atomic_bool g_escape {false};       // an Esc press for PollEscape
        std::atomic<float> g_width {0.0f};
        std::atomic<float> g_height {0.0f};

        std::mutex g_clickMutex;
        std::deque<Click> g_clicks;

        // ---- hooks (render thread) ----
        void STDMETHODCALLTYPE ExecuteCommandListsHook(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
            // The queue that presents is the direct queue last used on the presenting thread (copy / compute queues
            // run on the same thread too, and an ImGui list on one of those draws nothing).
            if (queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                t_lastDirectQueue = queue;
            }
            g_origExecute(queue, count, lists);
        }

        HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain3 *swapChain, UINT syncInterval, UINT flags) {
            if (!(flags & DXGI_PRESENT_TEST)) {
                if (g_ready.load()) {
                    auto *renderer = g_client->GetRenderer();
                    auto *backend  = renderer ? renderer->GetD3D12Backend() : nullptr;
                    if (backend && backend->GetSwapChain() != swapChain) {
                        backend->SetSwapChain(swapChain); // the game recreated it (mode change)
                    }
                    if (backend && g_client->GetImGUI()) {
                        // The D3D12 backend leaves the frame to the host: Begin (back buffer, barrier, render target),
                        // the ImGui draw data recorded into its list, End (barrier back, submit on the game's queue).
                        backend->Begin();
                        if (auto *web = g_client->GetWebManager()) {
                            web->Render(); // web view textures (no-op until the menu started CEF)
                        }
                        auto res = g_client->GetImGUI()->Render();
                        backend->End();
                        static bool first = true;
                        if (first) Log()->info("overlay: first frame: {}", res ? "drawn" : res.GetError().message);
                        first = false;
                    }
                }
                else if (!g_failed.load()) {
                    std::lock_guard lock(g_captureMutex);
                    // No references are taken: the game releases and recreates its swapchain (and checks that its
                    // Release frees it), so these stay borrowed and are refreshed on every Present until used.
                    if (t_lastDirectQueue) {
                        DXGI_SWAP_CHAIN_DESC desc {};
                        if (SUCCEEDED(swapChain->GetDesc(&desc))) {
                            g_stablePresents = (g_swapChain == swapChain && g_queue == t_lastDirectQueue) ? g_stablePresents + 1 : 0;
                            g_swapChain = swapChain;
                            g_queue     = t_lastDirectQueue;
                            g_window    = desc.OutputWindow;
                        }
                    }
                }
            }
            return g_origPresent(swapChain, syncInterval, flags);
        }

        // ---- DirectInput (the game reads keyboard and mouse through it, not window messages) ----
        bool TypingText() {
            auto *web = g_client->GetWebManager();
            return g_client->GetChatBox().IsInputActive() || Framework::External::ImGUI::Wrapper::WantsTextInput() ||
                   (web && web->IsAnyViewFocused());
        }
        bool KeyboardBlocked() {
            if (!g_ready.load() || !g_client) {
                return false;
            }
            return g_menu.load() || TypingText();
        }
        bool MouseBlocked() {
            return g_cursor.load() || (g_ready.load() && g_menu.load());
        }

        HRESULT STDMETHODCALLTYPE DiGetDeviceStateHook(IDirectInputDevice8W *device, DWORD size, LPVOID data) {
            const HRESULT hr = g_origDiState(device, size, data);
            if (SUCCEEDED(hr) && data) {
                const bool keyboard = size == 256;
                const bool mouse    = size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2);
                if ((keyboard && KeyboardBlocked()) || (mouse && (MouseBlocked() || KeyboardBlocked()))) {
                    std::memset(data, 0, size); // nothing pressed, no movement
                }
                else if (keyboard && g_captureEscape.load()) {
                    static_cast<uint8_t *>(data)[DIK_ESCAPE] = 0; // the game's pause menu never opens
                }
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE DiGetDeviceDataHook(IDirectInputDevice8W *device, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
            const HRESULT hr = g_origDiData(device, size, data, count, flags);
            if (SUCCEEDED(hr) && count && (KeyboardBlocked() || MouseBlocked()) && !(flags & DIGDD_PEEK)) {
                *count = 0; // buffered events are dropped while our UI holds the input
            }
            else if (SUCCEEDED(hr) && count && data && g_captureEscape.load()) {
                // Esc presses leave the buffer (no mouse or joystick offset is 1, so only the keyboard's Esc matches).
                DWORD kept = 0;
                for (DWORD i = 0; i < *count; ++i) {
                    auto *event = reinterpret_cast<LPDIDEVICEOBJECTDATA>(reinterpret_cast<uint8_t *>(data) + i * size);
                    if (event->dwOfs != DIK_ESCAPE) {
                        if (kept != i) {
                            std::memcpy(reinterpret_cast<uint8_t *>(data) + kept * size, event, size);
                        }
                        ++kept;
                    }
                }
                *count = kept;
            }
            return hr;
        }

        // Vtable slots of IDirectInputDevice8W from a throwaway keyboard device: GetDeviceState 9, GetDeviceData 10.
        bool HookDirectInput() {
            IDirectInput8W *di            = nullptr;
            IDirectInputDevice8W *keyboard = nullptr;
            if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void **>(&di), nullptr))) {
                return false;
            }
            bool ok = false;
            if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr))) {
                void **vtable = *reinterpret_cast<void ***>(keyboard);
                ok = MH_CreateHook(vtable[9], reinterpret_cast<void *>(&DiGetDeviceStateHook), reinterpret_cast<void **>(&g_origDiState)) == MH_OK &&
                     MH_CreateHook(vtable[10], reinterpret_cast<void *>(&DiGetDeviceDataHook), reinterpret_cast<void **>(&g_origDiData)) == MH_OK &&
                     MH_EnableHook(vtable[9]) == MH_OK && MH_EnableHook(vtable[10]) == MH_OK;
                keyboard->Release();
            }
            di->Release();
            return ok;
        }

        // ---- input (window thread) ----
        bool IsKeyboardMessage(UINT msg) {
            return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR;
        }
        bool IsMouseMessage(UINT msg) {
            return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        }

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            if (g_ready.load() && g_client && g_client->GetImGUI()) {
                (void)g_client->GetImGUI()->ProcessEvent(hwnd, msg, wParam, lParam);

                auto &chat = g_client->GetChatBox();
                if (auto *web = g_client->GetWebManager()) { // web views (the main menu) get the mouse and keys they want
                    if (IsMouseMessage(msg)) {
                        web->ProcessMouseEvent(hwnd, msg, wParam, lParam);
                    }
                    else if (IsKeyboardMessage(msg)) {
                        web->ProcessKeyboardEvent(hwnd, msg, wParam, lParam);
                    }
                }
                // Esc is ours: the in-game menu (unless a text field takes it), never the game's pause menu.
                if (g_captureEscape.load()) {
                    if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wParam == VK_ESCAPE) {
                        if (msg == WM_KEYDOWN && !(lParam & (1 << 30)) && !TypingText()) {
                            g_escape = true;
                        }
                        return 0;
                    }
                    if (msg == WM_INPUT) {
                        RAWINPUT raw {};
                        UINT rawSize = sizeof(raw);
                        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                            raw.header.dwType == RIM_TYPEKEYBOARD && raw.data.keyboard.VKey == VK_ESCAPE) {
                            return DefWindowProcW(hwnd, msg, wParam, lParam);
                        }
                    }
                }
                const bool typing = KeyboardBlocked();
                // T opens the chat (the character that follows is swallowed so it is not typed).
                static bool swallowChar = false;
                if (msg == WM_KEYDOWN && !typing && chat.IsSessionActive() && wParam == 'T' && !(lParam & (1 << 30))) {
                    chat.OpenInput();
                    swallowChar = true;
                    return 0;
                }
                if (msg == WM_CHAR && swallowChar) {
                    swallowChar = false;
                    return 0;
                }
                if (typing && (IsKeyboardMessage(msg) || msg == WM_INPUT)) {
                    if (msg == WM_INPUT) {
                        return DefWindowProcW(hwnd, msg, wParam, lParam); // raw keyboard/mouse: the game never sees it
                    }
                    return 0;
                }
                if (MouseBlocked()) {
                    if (!g_menu.load() && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)) {
                        Click c {};
                        c.button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
                        c.down   = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
                        c.x      = static_cast<float>(static_cast<short>(LOWORD(lParam)));
                        c.y      = static_cast<float>(static_cast<short>(HIWORD(lParam)));
                        std::lock_guard lock(g_clickMutex);
                        if (g_clicks.size() < 64) {
                            g_clicks.push_back(c);
                        }
                    }
                    if (IsMouseMessage(msg) || msg == WM_INPUT) {
                        if (msg == WM_INPUT) {
                            return DefWindowProcW(hwnd, msg, wParam, lParam);
                        }
                        return 0;
                    }
                }
            }
            return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
        }

        // ---- vtables from a throwaway device / queue / swapchain ----
        bool FindVtables(void **present, void **execute) {
            WNDCLASSEXW wc {sizeof(wc)};
            wc.lpfnWndProc   = DefWindowProcW;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"rdmp_overlay_probe";
            RegisterClassExW(&wc);
            HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
            bool ok   = false;

            ID3D12Device *device       = nullptr;
            ID3D12CommandQueue *queue  = nullptr;
            IDXGIFactory4 *factory     = nullptr;
            IDXGISwapChain1 *swapChain = nullptr;
            if (hwnd && SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                D3D12_COMMAND_QUEUE_DESC qd {};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                    DXGI_SWAP_CHAIN_DESC1 sd {};
                    sd.Width            = 64;
                    sd.Height           = 64;
                    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
                    sd.SampleDesc.Count = 1;
                    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    sd.BufferCount      = 2;
                    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                    if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swapChain))) {
                        *present = (*reinterpret_cast<void ***>(swapChain))[8];  // IDXGISwapChain::Present
                        *execute = (*reinterpret_cast<void ***>(queue))[10];     // ID3D12CommandQueue::ExecuteCommandLists
                        ok       = true;
                    }
                }
            }
            if (swapChain) swapChain->Release();
            if (factory) factory->Release();
            if (queue) queue->Release();
            if (device) device->Release();
            if (hwnd) DestroyWindow(hwnd);
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return ok;
        }

        void BringUp() {
            IDXGISwapChain3 *swapChain = nullptr;
            ID3D12CommandQueue *queue  = nullptr;
            HWND window                = nullptr;
            {
                std::lock_guard lock(g_captureMutex);
                if (g_stablePresents < kStablePresents) {
                    return;
                }
                swapChain = g_swapChain;
                queue     = g_queue;
                window    = g_window;
            }
            if (!swapChain || !queue || !window) {
                return;
            }
            Log()->info("overlay: game swapchain {:p} stable, reading its device", static_cast<void *>(swapChain));
            ID3D12Device *device = nullptr;
            if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
                g_failed = true;
                Log()->error("overlay: the swapchain has no D3D12 device");
                return;
            }
            device->Release(); // borrowed, like the swapchain and queue: the game keeps them alive
            auto &opts                           = g_client->GetOptions();
            opts.rendererOptions.backend         = Framework::Graphics::RendererBackend::BACKEND_D3D_12;
            opts.rendererOptions.platform        = Framework::Graphics::PlatformBackend::PLATFORM_WIN32;
            opts.rendererOptions.windowHandle    = window;
            opts.rendererOptions.d3d12.device    = device;
            opts.rendererOptions.d3d12.swapchain = swapChain;
            opts.rendererOptions.d3d12.commandQueue = queue;

            Log()->info("overlay: game swapchain {:p}, queue {:p}, window {:p}: starting the renderer", static_cast<void *>(swapChain),
                        static_cast<void *>(queue), static_cast<void *>(window));
            if (auto res = g_client->RenderInit(); !res || !g_client->GetImGUI()) {
                g_failed = true;
                Log()->error("overlay: renderer init failed: {}", res ? "no ImGui" : res.GetError().message);
                if (!ImGui::GetCurrentContext()) {
                    ImGui::CreateContext(nullptr);
                }
                return;
            }
            g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
            g_ready       = true;
            Log()->info("overlay: D3D12 renderer + ImGui up (window {:p})", static_cast<void *>(window));
        }
    } // namespace

    bool Install(FwClient *client, const rdmp::fw::Host *host) {
        g_client = client;
        g_host   = host;
        void *present = nullptr, *execute = nullptr;
        if (!FindVtables(&present, &execute)) {
            Log()->error("overlay: could not create a D3D12 probe swapchain; no overlay");
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            return false;
        }
        if (MH_CreateHook(present, reinterpret_cast<void *>(&PresentHook), reinterpret_cast<void **>(&g_origPresent)) != MH_OK
            || MH_CreateHook(execute, reinterpret_cast<void *>(&ExecuteCommandListsHook), reinterpret_cast<void **>(&g_origExecute)) != MH_OK
            || MH_EnableHook(present) != MH_OK || MH_EnableHook(execute) != MH_OK) {
            Log()->error("overlay: hooking Present / ExecuteCommandLists failed");
            return false;
        }
        if (!HookDirectInput()) {
            Log()->warn("overlay: DirectInput hooks failed: typing in the chat will reach the game");
        }
        Log()->info("overlay: hooks installed");
        return true;
    }

    void Frame(const std::function<void()> &draw) {
        if (!g_client) {
            return;
        }
        if (!g_ready.load()) {
            if (!g_failed.load()) {
                BringUp();
            }
            return;
        }
        auto *imgui = g_client->GetImGUI();
        if (!imgui) {
            return;
        }
        imgui->PushWidget([draw] {
            const ImGuiIO &io = ImGui::GetIO();
            g_width           = io.DisplaySize.x;
            g_height          = io.DisplaySize.y;
            if (draw) {
                draw();
            }
            if (auto *web = g_client->GetWebManager()) {
                web->SubmitImGuiDraws(); // web views on top (the main menu covers the HUD)
            }
        });
    }

    bool Ready() {
        return g_ready.load();
    }

    void ScreenSize(float &width, float &height) {
        width  = g_width.load();
        height = g_height.load();
    }

    void ShowCursor(bool show) {
        g_cursor = show;
        if (g_ready.load()) {
            Framework::External::ImGUI::Wrapper::ShowCursor(show || g_menu.load());
        }
    }

    void SetMenuActive(bool active) {
        g_menu = active;
        if (g_ready.load()) {
            Framework::External::ImGUI::Wrapper::ShowCursor(active || g_cursor.load());
        }
    }

    void CaptureEscape(bool capture) {
        g_captureEscape = capture;
    }

    bool PollEscape() {
        return g_escape.exchange(false);
    }

    void *Window() {
        return g_ready.load() ? static_cast<void *>(g_window) : nullptr;
    }

    bool UiHasKeyboard() {
        return KeyboardBlocked();
    }

    bool CursorShown() {
        return g_cursor.load();
    }

    bool PollClick(Click &out) {
        std::lock_guard lock(g_clickMutex);
        if (g_clicks.empty()) {
            return false;
        }
        out = g_clicks.front();
        g_clicks.pop_front();
        return true;
    }

    void Shutdown() {
        if (g_origWndProc && g_window) {
            SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        }
        MH_DisableHook(MH_ALL_HOOKS);
        g_ready = false;
    }
} // namespace RDMP::Overlay
