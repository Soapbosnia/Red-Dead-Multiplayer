// Boot loading overlay: a black, click-through, non-activating window kept exactly over the game window's client
// area, with an animated "LOADING" indicator in the middle. It hides the game's boot splash (legal pages, Rockstar and
// Double Eleven logos - drawn by the PC port, not the game's UI) and the loading art, until the player is in the world.
// rdmp.ini [sandbox] loading_overlay=1.
#include "splash.h"

#include <windows.h>

#include <atomic>
#include <cmath>

#include "log.h"

namespace rdmp {
namespace {

std::atomic<HWND> g_overlay{nullptr};
std::atomic<bool> g_hideRequested{false};
std::atomic<ULONGLONG> g_hideAt{0};
HWND g_game = nullptr;
int g_frame = 0;

BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || h == g_overlay.load() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (lstrcmpW(cls, L"RdmpLoadingOverlay") == 0) return TRUE;
    RECT r{};
    GetClientRect(h, &r);
    if (r.right - r.left < 320 || r.bottom - r.top < 200) return TRUE;  // skip tiny helper windows
    *reinterpret_cast<HWND*>(out) = h;
    return FALSE;
}

void PlaceOverGame(HWND self) {
    if (!g_game || !IsWindow(g_game)) {
        g_game = nullptr;
        EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&g_game));
        if (!g_game) {
            ShowWindow(self, SW_HIDE);
            return;
        }
    }
    if (IsIconic(g_game)) {
        ShowWindow(self, SW_HIDE);
        return;
    }
    RECT r{};
    GetClientRect(g_game, &r);
    POINT tl{r.left, r.top};
    ClientToScreen(g_game, &tl);
    SetWindowPos(self, HWND_TOPMOST, tl.x, tl.y, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right, hgt = rc.bottom;
    // double buffer
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, hgt);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    const int cx = w / 2, cy = hgt / 2;
    // spinner: 12 dots on a circle, the "head" dot brightest, trailing ones fading
    const int radius = hgt / 22 + 12, dot = hgt / 140 + 3;
    for (int i = 0; i < 12; ++i) {
        const int age = (g_frame - i + 1200) % 12;  // 0 = head
        const int v = age < 8 ? 235 - age * 28 : 25;
        HBRUSH b = CreateSolidBrush(RGB(v, v * 3 / 4, v / 2));  // warm white -> dim amber
        HGDIOBJ ob = SelectObject(mem, b);
        HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
        const double a = i * 3.14159265 / 6.0 - 3.14159265 / 2.0;
        const int x = cx + static_cast<int>(std::cos(a) * radius), y = cy - radius / 3 + static_cast<int>(std::sin(a) * radius);
        Ellipse(mem, x - dot, y - dot, x + dot, y + dot);
        SelectObject(mem, op);
        SelectObject(mem, ob);
        DeleteObject(b);
    }
    // caption
    HFONT font = CreateFontW(-(hgt / 30 + 8), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ of = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(230, 225, 215));
    static const wchar_t* kDots[] = {L"LOADING", L"LOADING.", L"LOADING..", L"LOADING..."};
    RECT tr{0, cy + radius, w, cy + radius + hgt / 10};
    DrawTextW(mem, kDots[(g_frame / 6) % 4], -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    SelectObject(mem, of);
    DeleteObject(font);

    BitBlt(dc, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        ++g_frame;
        static const ULONGLONG started = GetTickCount64();
        if ((g_hideRequested.load() && GetTickCount64() >= g_hideAt.load()) || GetTickCount64() - started > 180000) {  // failsafe 3 min
            DestroyWindow(h);
            return 0;
        }
        PlaceOverGame(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_DESTROY:
        g_overlay = nullptr;
        Log("splash: loading overlay closed");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

DWORD WINAPI OverlayThread(LPVOID) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RdmpLoadingOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                             wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) {
        Log("splash: overlay window creation failed (%lu)", GetLastError());
        return 0;
    }
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);  // opaque; layered only so WS_EX_TRANSPARENT passes input through
    g_overlay = h;
    SetTimer(h, 1, 33, nullptr);  // ~30 fps animation + follow the game window
    Log("splash: loading overlay up");
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

}  // namespace

void StartLoadingOverlay() {
    HANDLE t = CreateThread(nullptr, 0, &OverlayThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void HideLoadingOverlay(unsigned delayMs) {
    g_hideAt = GetTickCount64() + delayMs;
    g_hideRequested = true;
}

}  // namespace rdmp
