#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
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
static const UINT     STATE_TIMER_ID = 2;
static const UINT     WM_SYNC_STATE = WM_APP + 2;

static HWND     g_hwnd = nullptr;
static HINSTANCE g_hInst = nullptr;
static Config   g_cfg;
static Metrics  g_metrics;
static unsigned g_tick = 0;
static bool     g_hiddenByFullscreen = false;
static bool     g_userVisible = true;
static bool     g_samplingPaused = false;
static bool     g_applyingLayer = false;
static bool     g_desktopRaised = false;
static bool     g_dragging = false;
static HWND     g_desktopHost = nullptr;
static HWND     g_lastForeground = nullptr;
static HWINEVENTHOOK g_foregroundHook = nullptr;
static UINT     g_taskbarCreated = 0;
static NOTIFYICONDATAW g_nid = {};

// opacity / refresh presets live in these menu-id ranges
static const UINT IDM_OPACITY_BASE = 41000;   // + percentage
static const UINT IDM_REFRESH_BASE = 42000;   // + (ms / 100)

// --- forward decls ---------------------------------------------------------
static void RefreshNow();
static void ApplyWindowFlags();
static void ApplyOpacity();
static void HandleCommand(UINT id);
static void ApplyWindowLayer();
static void SyncWindowState(bool forceLayer = false);

static void SaveConfig() {
    if (!Config_Save(g_cfg))
        MessageBoxW(g_hwnd, L"无法保存 config.ini。请将程序放到可写目录，并检查配置文件是否为只读。",
                    L"SysWidget", MB_OK | MB_ICONWARNING);
}

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
static bool ShellWindow(HWND hwnd) {
    if (!hwnd) return false;
    DWORD process = 0, shellProcess = 0;
    GetWindowThreadProcessId(hwnd, &process);
    GetWindowThreadProcessId(GetShellWindow(), &shellProcess);
    if (!shellProcess || process != shellProcess) return false;
    wchar_t name[64] = {};
    GetClassNameW(hwnd, name, 64);
    return wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0
        || wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0;
}

static BOOL CALLBACK FindDesktopHost(HWND hwnd, LPARAM value) {
    if (ShellWindow(hwnd) && FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
        *(HWND*)value = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND DesktopHost() {
    if (!IsWindow(g_desktopHost)
        || !FindWindowExW(g_desktopHost, nullptr, L"SHELLDLL_DefView", nullptr)) {
        g_desktopHost = nullptr;
        EnumWindows(FindDesktopHost, (LPARAM)&g_desktopHost);
    }
    return g_desktopHost;
}

static bool FullscreenActive() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == g_hwnd || ShellWindow(foreground)
        || !IsWindowVisible(foreground) || IsIconic(foreground)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(foreground, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked) return false;
    if (IsZoomed(foreground)) {
        LONG_PTR style = GetWindowLongPtrW(foreground, GWL_STYLE);
        if ((style & WS_CAPTION) == WS_CAPTION || (style & WS_THICKFRAME)) return false;
    }
    RECT window = {};
    if (FAILED(DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &window, sizeof(window)))
        && !GetWindowRect(foreground, &window)) return false;
    MONITORINFO monitor = { sizeof(monitor) };
    if (!GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    return window.left <= monitor.rcMonitor.left && window.top <= monitor.rcMonitor.top
        && window.right >= monitor.rcMonitor.right && window.bottom >= monitor.rcMonitor.bottom;
}

static void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_SYNC_STATE, 1, 0);
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

    ApplyWindowLayer();
}

static void ApplyWindowLayer() {
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    g_applyingLayer = true;
    if (g_cfg.windowMode == WindowMode::Global) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0, flags);
    } else if (g_cfg.windowMode == WindowMode::Desktop) {
        HWND host = DesktopHost();
        bool hostTopmost = host && (GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST);
        SetWindowPos(g_hwnd, hostTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        HWND before = host ? GetWindow(host, GW_HWNDPREV) : nullptr;
        if (before == g_hwnd) before = GetWindow(g_hwnd, GW_HWNDPREV);
        HWND layer = before ? before : hostTopmost
            ? HWND_TOPMOST : host ? HWND_TOP : HWND_BOTTOM;
        SetWindowPos(g_hwnd, layer, 0, 0, 0, 0, flags);
    } else {
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    }
    g_applyingLayer = false;
}

static void SyncWindowState(bool forceLayer) {
    if (!g_hwnd || g_dragging) return;
    g_hiddenByFullscreen = g_cfg.hideOnFullscreen && FullscreenActive();
    bool visible = g_userVisible && !g_hiddenByFullscreen;
    if (!visible) {
        if (IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
        g_samplingPaused = true;
        return;
    }
    if (g_samplingPaused) {
        Metrics_ResetCpu();
        g_metrics.cpu = 0;
        g_metrics.cpuTopN = 0;
        g_samplingPaused = false;
        RefreshNow();
        forceLayer = true;
    }
    bool managed = g_cfg.windowMode != WindowMode::Normal;
    if ((!IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) && (managed || forceLayer)) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        forceLayer = true;
    }
    HWND host = DesktopHost();
    bool raised = host && (GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST);
    HWND foreground = GetForegroundWindow();
    if (raised != g_desktopRaised || foreground != g_lastForeground) forceLayer = true;
    g_desktopRaised = raised;
    g_lastForeground = foreground;
    if (g_cfg.windowMode == WindowMode::Desktop && host
        && GetWindow(g_hwnd, GW_HWNDNEXT) != host) forceLayer = true;
    if (g_cfg.windowMode == WindowMode::Global
        && !(GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) forceLayer = true;
    if (g_cfg.windowMode == WindowMode::Global && host) {
        for (HWND above = GetWindow(g_hwnd, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
            if (above == host) { forceLayer = true; break; }
        }
    }
    if (forceLayer && managed) ApplyWindowLayer();
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
    if (!g_nid.hIcon) g_nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
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
    if (!g_cfg.showCpu) EnableMenuItem(m, IDM_TOGGLE_CPUTOP, MF_BYCOMMAND | MF_GRAYED);
    if (!g_cfg.showMem) EnableMenuItem(m, IDM_TOGGLE_MEMTOP, MF_BYCOMMAND | MF_GRAYED);
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

    HMENU layer = CreatePopupMenu();
    AppendCheck(layer, IDM_WINDOW_NORMAL, L"不置顶", g_cfg.windowMode == WindowMode::Normal);
    AppendCheck(layer, IDM_WINDOW_GLOBAL, L"全局置顶", g_cfg.windowMode == WindowMode::Global);
    AppendCheck(layer, IDM_WINDOW_DESKTOP, L"仅桌面", g_cfg.windowMode == WindowMode::Desktop);
    CheckMenuRadioItem(layer, IDM_WINDOW_NORMAL, IDM_WINDOW_DESKTOP,
                      IDM_WINDOW_NORMAL + (UINT)g_cfg.windowMode, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)layer, L"窗口层级");
    AppendCheck(m, IDM_HIDE_FULLSCREEN, L"全屏时自动隐藏", g_cfg.hideOnFullscreen);
    AppendCheck(m, IDM_SHOW_HIDE, L"显示组件", g_userVisible);
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

    if (IsWindow(fgWnd) && GetForegroundWindow() == g_hwnd) SetForegroundWindow(fgWnd);

    if (cmd) HandleCommand(cmd);
    SyncWindowState(true);
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
        case IDM_TOGGLE_CPU:
            g_cfg.showCpu = !g_cfg.showCpu;
            if (!g_cfg.showCpu) g_cfg.showCpuTop = false;
            Metrics_ResetCpu(); relayout = true; break;
        case IDM_TOGGLE_MEM:
            g_cfg.showMem = !g_cfg.showMem;
            if (!g_cfg.showMem) g_cfg.showMemTop = false;
            relayout = true; break;
        case IDM_TOGGLE_IP:     g_cfg.showIp     = !g_cfg.showIp;     relayout = true; break;
        case IDM_TOGGLE_CPUTOP:
            if (g_cfg.showCpu) { g_cfg.showCpuTop = !g_cfg.showCpuTop; Metrics_ResetCpu(); relayout = true; }
            break;
        case IDM_TOGGLE_MEMTOP:
            if (g_cfg.showMem) { g_cfg.showMemTop = !g_cfg.showMemTop; relayout = true; }
            break;
        case IDM_MODE_MINIMAL:  g_cfg.mode = DisplayMode::Minimal;    relayout = true; break;
        case IDM_MODE_BARS:     g_cfg.mode = DisplayMode::Bars;       relayout = true; break;
        case IDM_WINDOW_NORMAL: g_cfg.windowMode = WindowMode::Normal; ApplyWindowFlags(); break;
        case IDM_WINDOW_GLOBAL: g_cfg.windowMode = WindowMode::Global; ApplyWindowFlags(); break;
        case IDM_WINDOW_DESKTOP: g_cfg.windowMode = WindowMode::Desktop; ApplyWindowFlags(); break;
        case IDM_HIDE_FULLSCREEN: g_cfg.hideOnFullscreen = !g_cfg.hideOnFullscreen; break;
        case IDM_SHOW_HIDE: g_userVisible = !g_userVisible; saveCfg = false; break;
        case IDM_CLICK_THROUGH: g_cfg.clickThrough = !g_cfg.clickThrough; ApplyWindowFlags(); break;
        case IDM_AUTOSTART:     g_cfg.autoStart = !g_cfg.autoStart;   SetAutostart(g_cfg.autoStart); break;
        case IDM_EXIT:          DestroyWindow(g_hwnd); return;
        default: saveCfg = false; break;
    }

    if (relayout && !g_samplingPaused) RefreshNow();
    SyncWindowState(true);
    if (saveCfg) SaveConfig();
}

// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        g_desktopHost = nullptr;
        TrayAdd();
        SyncWindowState(true);
        return 0;
    }
    switch (msg) {
    case WM_SYNC_STATE:
        SyncWindowState(wp != 0);
        return 0;
    case WM_TIMER:
        if (wp == STATE_TIMER_ID) { SyncWindowState(); return 0; }
        if (wp == TIMER_ID) {
            if (g_dragging) return 0;
            SyncWindowState();
            if (g_samplingPaused) return 0;
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

    case WM_WINDOWPOSCHANGING:
        if (g_cfg.windowMode == WindowMode::Desktop && !g_applyingLayer)
            ((WINDOWPOS*)lp)->flags |= SWP_NOZORDER;
        break;

    case WM_ENTERSIZEMOVE:
        g_dragging = true;
        return 0;

    case WM_DISPLAYCHANGE:
        g_desktopHost = nullptr;
        if (!g_samplingPaused) RefreshNow();
        SyncWindowState(true);
        return 0;

    case WM_EXITSIZEMOVE: {
        RECT r; GetWindowRect(hwnd, &r);
        g_cfg.x = r.left; g_cfg.y = r.top;
        g_dragging = false;
        SaveConfig();
        SyncWindowState(true);
        return 0;
    }

    case WM_CONTEXTMENU:
        ShowMenu();
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowMenu();
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            HandleCommand(IDM_SHOW_HIDE);
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
        KillTimer(hwnd, STATE_TIMER_ID);
        if (g_foregroundHook) UnhookWinEvent(g_foregroundHook);
        g_foregroundHook = nullptr;
        TrayRemove();
        SaveConfig();
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

    if (!Config_Load(g_cfg))
        MessageBoxW(nullptr, L"无法创建或更新 config.ini。请将程序放到可写目录，并检查配置文件是否为只读。",
                    L"SysWidget", MB_OK | MB_ICONWARNING);
    Metrics_Init();
    Render_Init();

    g_hInst = hInst;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

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
    SyncWindowState(true);

    TrayAdd();
    SetTimer(g_hwnd, TIMER_ID, g_cfg.refreshMs, nullptr);
    SetTimer(g_hwnd, STATE_TIMER_ID, 500, nullptr);
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
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
