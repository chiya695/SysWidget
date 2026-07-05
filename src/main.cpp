#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "metrics.h"
#include "render.h"
#include "resource.h"

// ---------------------------------------------------------------------------
// Globals. A widget like this is naturally a small pile of global state; keeping
// it here (rather than a class) keeps the code and the binary tiny.
// ---------------------------------------------------------------------------
static const wchar_t* kClass = L"SysWidgetWindow";
static const UINT     WM_TRAY = WM_APP + 1;
static const UINT     TIMER_ID = 1;

static HWND     g_hwnd = nullptr;
static HINSTANCE g_hInst = nullptr;
static Config   g_cfg;
static Metrics  g_metrics;
static unsigned g_tick = 0;
static bool     g_hiddenByFullscreen = false;
static NOTIFYICONDATAW g_nid = {};

// opacity / refresh presets live in these menu-id ranges
static const UINT IDM_OPACITY_BASE = 41000;   // + percentage
static const UINT IDM_REFRESH_BASE = 42000;   // + (ms / 100)

// --- forward decls ---------------------------------------------------------
static void RefreshNow();
static void ApplyWindowFlags();
static void ApplyOpacity();
static void HandleCommand(UINT id);

// Hand idle pages back to the OS so the number Task Manager shows stays small.
// The pages fault back in on demand; for a mostly-idle widget that never happens.
static void TrimWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

// ---------------------------------------------------------------------------
// autostart via the per-user Run key (no admin, no services, no drivers)
// ---------------------------------------------------------------------------
static void ExePath(wchar_t* out, size_t cch) {
    GetModuleFileNameW(nullptr, out, (DWORD)cch);
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH]; ExePath(path, MAX_PATH);
        wchar_t quoted[MAX_PATH + 2];
        swprintf_s(quoted, L"\"%s\"", path);
        RegSetValueExW(k, L"SysWidget", 0, REG_SZ,
            (const BYTE*)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"SysWidget");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// is a real fullscreen app (game / video) in the foreground?
// ---------------------------------------------------------------------------
static bool FullscreenActive() {
    QUERY_USER_NOTIFICATION_STATE s;
    if (SHQueryUserNotificationState(&s) == S_OK) {
        if (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// window helpers
// ---------------------------------------------------------------------------
static int Dpi() {
    UINT d = GetDpiForWindow(g_hwnd);
    return d ? (int)d : 96;
}

static void ApplyOpacity() {
    BYTE a = (BYTE)(g_cfg.opacity * 255 / 100);
    if (a < 12) a = 12;   // never let it vanish completely
    SetLayeredWindowAttributes(g_hwnd, 0, a, LWA_ALPHA);
}

static void ApplyWindowFlags() {
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (g_cfg.clickThrough) ex |= WS_EX_TRANSPARENT;
    else                    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(g_hwnd, g_cfg.topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// clamp a proposed position so the widget stays reachable on some monitor
static void ClampToWorkArea(int& x, int& y, int w, int h) {
    POINT pt{ x + w / 2, y + h / 2 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        RECT r = mi.rcWork;
        if (x < r.left) x = r.left;
        if (y < r.top)  y = r.top;
        if (x + w > r.right)  x = r.right - w;
        if (y + h > r.bottom) y = r.bottom - h;
    }
}

// re-measure for current data, resize + round the window, repaint
static void RefreshNow() {
    Metrics_Update(g_cfg, g_metrics, g_tick++);

    int dpi = Dpi();
    SIZE want = Render_Measure(g_cfg, g_metrics, dpi);

    RECT wr; GetWindowRect(g_hwnd, &wr);
    int x = g_cfg.x, y = g_cfg.y;
    ClampToWorkArea(x, y, want.cx, want.cy);
    g_cfg.x = x; g_cfg.y = y;

    SetWindowPos(g_hwnd, nullptr, x, y, want.cx, want.cy, SWP_NOZORDER | SWP_NOACTIVATE);

    int r = MulDiv(10, dpi, 96);
    HRGN rgn = CreateRoundRectRgn(0, 0, want.cx + 1, want.cy + 1, r, r);
    SetWindowRgn(g_hwnd, rgn, TRUE);   // window takes ownership of rgn

    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// tray icon + popup menu
// ---------------------------------------------------------------------------
static void TrayAdd() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    // our own icon at the tray's small-icon size for a crisp result
    g_nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"SysWidget");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayRemove() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) { DestroyIcon(g_nid.hIcon); g_nid.hIcon = nullptr; }
}

static void AppendCheck(HMENU m, UINT id, const wchar_t* text, bool checked) {
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), id, text);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();

    AppendCheck(m, IDM_TOGGLE_CPU,    L"显示 CPU",        g_cfg.showCpu);
    AppendCheck(m, IDM_TOGGLE_MEM,    L"显示 内存",       g_cfg.showMem);
    AppendCheck(m, IDM_TOGGLE_IP,     L"显示 IP",         g_cfg.showIp);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendCheck(m, IDM_TOGGLE_CPUTOP, L"展开 CPU 占用前5", g_cfg.showCpuTop);
    AppendCheck(m, IDM_TOGGLE_MEMTOP, L"展开 内存占用前5", g_cfg.showMemTop);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // display mode
    HMENU mode = CreatePopupMenu();
    AppendCheck(mode, IDM_MODE_MINIMAL, L"极简数字",  g_cfg.mode == DisplayMode::Minimal);
    AppendCheck(mode, IDM_MODE_BARS,    L"数字 + 条", g_cfg.mode == DisplayMode::Bars);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mode, L"显示样式");

    // opacity presets
    HMENU op = CreatePopupMenu();
    const int presets[] = { 100, 90, 75, 60, 45, 30 };
    for (int p : presets) {
        wchar_t t[16]; swprintf_s(t, L"%d%%", p);
        AppendCheck(op, IDM_OPACITY_BASE + p, t, g_cfg.opacity == p);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)op, L"透明度");

    // refresh interval presets
    HMENU rf = CreatePopupMenu();
    const int ms[] = { 500, 1000, 2000, 3000, 5000 };
    for (int v : ms) {
        wchar_t t[24]; swprintf_s(t, L"%.1f 秒", v / 1000.0);
        AppendCheck(rf, IDM_REFRESH_BASE + v / 100, t, g_cfg.refreshMs == v);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rf, L"刷新间隔");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendCheck(m, IDM_TOP_MOST,      L"窗口置顶",       g_cfg.topMost);
    AppendCheck(m, IDM_CLICK_THROUGH, L"鼠标点击穿透",   g_cfg.clickThrough);
    AppendCheck(m, IDM_AUTOSTART,     L"开机自启",       g_cfg.autoStart);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    POINT pt; GetCursorPos(&pt);

    // A tool window with WS_EX_NOACTIVATE can't normally grab the foreground, and
    // TrackPopupMenu closes instantly if its owner isn't foreground. Briefly
    // attaching to the current foreground thread's input queue lets the call
    // succeed, so the menu stays open until the user picks something.
    HWND fgWnd = GetForegroundWindow();
    DWORD fgThread = fgWnd ? GetWindowThreadProcessId(fgWnd, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();
    if (fgThread && fgThread != myThread) AttachThreadInput(myThread, fgThread, TRUE);
    SetForegroundWindow(g_hwnd);

    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, g_hwnd, nullptr);

    if (fgThread && fgThread != myThread) AttachThreadInput(myThread, fgThread, FALSE);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (cmd) HandleCommand(cmd);
}

// ---------------------------------------------------------------------------
static void HandleCommand(UINT id) {
    bool relayout = false, saveCfg = true;

    if (id >= IDM_OPACITY_BASE && id <= IDM_OPACITY_BASE + 100) {
        g_cfg.opacity = (int)(id - IDM_OPACITY_BASE);
        ApplyOpacity();
    } else if (id >= IDM_REFRESH_BASE && id <= IDM_REFRESH_BASE + 100) {
        g_cfg.refreshMs = (int)(id - IDM_REFRESH_BASE) * 100;
        SetTimer(g_hwnd, TIMER_ID, g_cfg.refreshMs, nullptr);
    } else switch (id) {
        case IDM_TOGGLE_CPU:    g_cfg.showCpu    = !g_cfg.showCpu;    relayout = true; break;
        case IDM_TOGGLE_MEM:    g_cfg.showMem    = !g_cfg.showMem;    relayout = true; break;
        case IDM_TOGGLE_IP:     g_cfg.showIp     = !g_cfg.showIp;     relayout = true; break;
        case IDM_TOGGLE_CPUTOP: g_cfg.showCpuTop = !g_cfg.showCpuTop; relayout = true; break;
        case IDM_TOGGLE_MEMTOP: g_cfg.showMemTop = !g_cfg.showMemTop; relayout = true; break;
        case IDM_MODE_MINIMAL:  g_cfg.mode = DisplayMode::Minimal;    relayout = true; break;
        case IDM_MODE_BARS:     g_cfg.mode = DisplayMode::Bars;       relayout = true; break;
        case IDM_TOP_MOST:      g_cfg.topMost = !g_cfg.topMost;       ApplyWindowFlags(); break;
        case IDM_CLICK_THROUGH: g_cfg.clickThrough = !g_cfg.clickThrough; ApplyWindowFlags(); break;
        case IDM_AUTOSTART:     g_cfg.autoStart = !g_cfg.autoStart;   SetAutostart(g_cfg.autoStart); break;
        case IDM_EXIT:          DestroyWindow(g_hwnd); return;
        default: saveCfg = false; break;
    }

    if (relayout) RefreshNow();
    if (saveCfg)  Config_Save(g_cfg);
}

// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_ID) {
            if (g_cfg.hideOnFullscreen) {
                bool fs = FullscreenActive();
                if (fs && !g_hiddenByFullscreen) { ShowWindow(hwnd, SW_HIDE); g_hiddenByFullscreen = true; }
                else if (!fs && g_hiddenByFullscreen) { ShowWindow(hwnd, SW_SHOWNOACTIVATE); g_hiddenByFullscreen = false; }
                if (g_hiddenByFullscreen) return 0;   // skip sampling while hidden
            }
            RefreshNow();
            if ((g_tick % 30) == 0) TrimWorkingSet();   // ~every 30 refreshes
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;   // painted fully in WM_PAINT, no flicker

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT c; GetClientRect(hwnd, &c);
        int w = c.right, h = c.bottom;

        // double buffer
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ ob = SelectObject(mem, bmp);

        Render_Paint(mem, c, g_cfg, g_metrics, Dpi());
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
        // whole surface acts as a drag handle
        return HTCAPTION;

    case WM_EXITSIZEMOVE: {
        RECT r; GetWindowRect(hwnd, &r);
        g_cfg.x = r.left; g_cfg.y = r.top;
        Config_Save(g_cfg);
        return 0;
    }

    case WM_CONTEXTMENU:
        ShowMenu();
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowMenu();
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            // double-click tray = show/hide the widget
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return 0;

    case WM_COMMAND:
        HandleCommand(LOWORD(wp));
        return 0;

    case WM_DPICHANGED:
        RefreshNow();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        TrayRemove();
        Config_Save(g_cfg);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // single instance
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"SysWidget_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    Config_Load(g_cfg);
    Metrics_Init();
    Render_Init();

    g_hInst = hInst;

    HICON hBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = hBig;
    wc.hIconSm = hSmall;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_hwnd = CreateWindowExW(ex, kClass, L"SysWidget", WS_POPUP,
                             g_cfg.x, g_cfg.y, 160, 90,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // make autostart match config (in case the exe was moved)
    SetAutostart(g_cfg.autoStart);

    ApplyOpacity();
    ApplyWindowFlags();

    // first sample + layout before showing, so it appears at the right size
    RefreshNow();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    TrayAdd();
    SetTimer(g_hwnd, TIMER_ID, g_cfg.refreshMs, nullptr);
    TrimWorkingSet();   // trim once the startup allocations have settled

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Render_Free();
    Metrics_Shutdown();
    if (mtx) CloseHandle(mtx);
    return 0;
}
