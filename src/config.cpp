#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// We use the classic private-profile API. It is old, but tiny and dependency-free,
// which is exactly what this project is about.

static bool FileExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void ExeDir(wchar_t* out, size_t cch) {
    GetModuleFileNameW(nullptr, out, (DWORD)cch);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
}

void Config_Path(wchar_t* out, size_t cch) {
    ExeDir(out, cch);
    wcsncat_s(out, cch, L"config.ini", _TRUNCATE);
}

static void DefaultPath(wchar_t* out, size_t cch) {
    ExeDir(out, cch);
    wcsncat_s(out, cch, L"config.ini.default", _TRUNCATE);
}

static COLORREF ParseHexColor(const wchar_t* s, COLORREF fallback) {
    if (!s || !*s) return fallback;
    unsigned r = 0, g = 0, b = 0;
    if (swscanf_s(s, L"%2x%2x%2x", &r, &g, &b) == 3)
        return RGB(r, g, b);
    return fallback;
}

static void FormatHexColor(COLORREF c, wchar_t* out, size_t cch) {
    swprintf_s(out, cch, L"%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
}

static int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool Config_Load(Config& c) {
    wchar_t ini[MAX_PATH];  Config_Path(ini, MAX_PATH);

    // First run: seed config.ini from the shipped default if we have one.
    if (!FileExists(ini)) {
        wchar_t def[MAX_PATH]; DefaultPath(def, MAX_PATH);
        if (FileExists(def))
            CopyFileW(def, ini, TRUE);
    }

    wchar_t buf[128];

    GetPrivateProfileStringW(L"general", L"mode", L"minimal", buf, 128, ini);
    c.mode = (_wcsicmp(buf, L"bars") == 0) ? DisplayMode::Bars : DisplayMode::Minimal;

    c.opacity          = ClampI(GetPrivateProfileIntW(L"general", L"opacity", 92, ini), 5, 100);
    c.refreshMs        = ClampI(GetPrivateProfileIntW(L"general", L"refresh_ms", 1000, ini), 500, 5000);
    c.x                = GetPrivateProfileIntW(L"general", L"x", 120, ini);
    c.y                = GetPrivateProfileIntW(L"general", L"y", 120, ini);
    GetPrivateProfileStringW(L"general", L"window_mode", L"", buf, 128, ini);
    if (_wcsicmp(buf, L"desktop") == 0) c.windowMode = WindowMode::Desktop;
    else if (_wcsicmp(buf, L"normal") == 0) c.windowMode = WindowMode::Normal;
    else if (_wcsicmp(buf, L"global") == 0) c.windowMode = WindowMode::Global;
    else c.windowMode = GetPrivateProfileIntW(L"general", L"topmost", 1, ini)
        ? WindowMode::Global : WindowMode::Normal;
    c.clickThrough     = GetPrivateProfileIntW(L"general", L"click_through", 0, ini) != 0;
    c.autoStart        = GetPrivateProfileIntW(L"general", L"autostart", 0, ini) != 0;
    c.hideOnFullscreen = GetPrivateProfileIntW(L"general", L"hide_on_fullscreen", 1, ini) != 0;

    c.showCpu    = GetPrivateProfileIntW(L"show", L"cpu",     1, ini) != 0;
    c.showMem    = GetPrivateProfileIntW(L"show", L"mem",     1, ini) != 0;
    c.showIp     = GetPrivateProfileIntW(L"show", L"ip",      1, ini) != 0;
    c.showCpuTop = GetPrivateProfileIntW(L"show", L"cpu_top", 0, ini) != 0;
    c.showMemTop = GetPrivateProfileIntW(L"show", L"mem_top", 0, ini) != 0;
    if (!c.showCpu) c.showCpuTop = false;
    if (!c.showMem) c.showMemTop = false;

    GetPrivateProfileStringW(L"colors", L"background", L"1c1c1e", buf, 128, ini);
    c.colBg = ParseHexColor(buf, c.colBg);
    GetPrivateProfileStringW(L"colors", L"text", L"e6e6e6", buf, 128, ini);
    c.colText = ParseHexColor(buf, c.colText);
    GetPrivateProfileStringW(L"colors", L"accent", L"4aa3ff", buf, 128, ini);
    c.colAccent = ParseHexColor(buf, c.colAccent);
    GetPrivateProfileStringW(L"colors", L"warn", L"ff9f43", buf, 128, ini);
    c.colWarn = ParseHexColor(buf, c.colWarn);
    return Config_Save(c);
}

static bool WriteInt(const wchar_t* sec, const wchar_t* key, int v, const wchar_t* ini) {
    wchar_t b[32]; _itow_s(v, b, 32, 10);
    return WritePrivateProfileStringW(sec, key, b, ini) != FALSE;
}

bool Config_Save(const Config& c) {
    wchar_t ini[MAX_PATH]; Config_Path(ini, MAX_PATH);

    bool saved = WritePrivateProfileStringW(L"general", L"mode",
        c.mode == DisplayMode::Bars ? L"bars" : L"minimal", ini) != FALSE;
    saved &= WriteInt(L"general", L"opacity",            c.opacity, ini);
    saved &= WriteInt(L"general", L"refresh_ms",         c.refreshMs, ini);
    saved &= WriteInt(L"general", L"x",                  c.x, ini);
    saved &= WriteInt(L"general", L"y",                  c.y, ini);
    saved &= WriteInt(L"general", L"topmost", c.windowMode == WindowMode::Global, ini);
    const wchar_t* windowMode = c.windowMode == WindowMode::Desktop ? L"desktop"
        : c.windowMode == WindowMode::Global ? L"global" : L"normal";
    saved &= WriteInt(L"general", L"click_through",      c.clickThrough, ini);
    saved &= WriteInt(L"general", L"autostart",          c.autoStart, ini);
    saved &= WriteInt(L"general", L"hide_on_fullscreen", c.hideOnFullscreen, ini);

    saved &= WriteInt(L"show", L"cpu",     c.showCpu, ini);
    saved &= WriteInt(L"show", L"mem",     c.showMem, ini);
    saved &= WriteInt(L"show", L"ip",      c.showIp, ini);
    saved &= WriteInt(L"show", L"cpu_top", c.showCpuTop, ini);
    saved &= WriteInt(L"show", L"mem_top", c.showMemTop, ini);

    wchar_t hex[16];
    FormatHexColor(c.colBg, hex, 16);     saved &= WritePrivateProfileStringW(L"colors", L"background", hex, ini) != FALSE;
    FormatHexColor(c.colText, hex, 16);   saved &= WritePrivateProfileStringW(L"colors", L"text", hex, ini) != FALSE;
    FormatHexColor(c.colAccent, hex, 16); saved &= WritePrivateProfileStringW(L"colors", L"accent", hex, ini) != FALSE;
    FormatHexColor(c.colWarn, hex, 16);   saved &= WritePrivateProfileStringW(L"colors", L"warn", hex, ini) != FALSE;
    saved &= WritePrivateProfileStringW(L"general", L"window_mode", windowMode, ini) != FALSE;
    return saved;
}
