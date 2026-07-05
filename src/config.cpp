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

void Config_Load(Config& c) {
    wchar_t ini[MAX_PATH];  Config_Path(ini, MAX_PATH);

    // First run: seed config.ini from the shipped default if we have one.
    if (!FileExists(ini)) {
        wchar_t def[MAX_PATH]; DefaultPath(def, MAX_PATH);
        if (FileExists(def))
            CopyFileW(def, ini, TRUE);
        // If neither exists, WritePrivateProfile below will create it from defaults.
    }

    wchar_t buf[128];

    GetPrivateProfileStringW(L"general", L"mode", L"minimal", buf, 128, ini);
    c.mode = (_wcsicmp(buf, L"bars") == 0) ? DisplayMode::Bars : DisplayMode::Minimal;

    c.opacity          = ClampI(GetPrivateProfileIntW(L"general", L"opacity", 92, ini), 5, 100);
    c.refreshMs        = ClampI(GetPrivateProfileIntW(L"general", L"refresh_ms", 1000, ini), 500, 5000);
    c.x                = GetPrivateProfileIntW(L"general", L"x", 120, ini);
    c.y                = GetPrivateProfileIntW(L"general", L"y", 120, ini);
    c.topMost          = GetPrivateProfileIntW(L"general", L"topmost", 1, ini) != 0;
    c.clickThrough     = GetPrivateProfileIntW(L"general", L"click_through", 0, ini) != 0;
    c.autoStart        = GetPrivateProfileIntW(L"general", L"autostart", 0, ini) != 0;
    c.hideOnFullscreen = GetPrivateProfileIntW(L"general", L"hide_on_fullscreen", 1, ini) != 0;

    c.showCpu    = GetPrivateProfileIntW(L"show", L"cpu",     1, ini) != 0;
    c.showMem    = GetPrivateProfileIntW(L"show", L"mem",     1, ini) != 0;
    c.showIp     = GetPrivateProfileIntW(L"show", L"ip",      1, ini) != 0;
    c.showCpuTop = GetPrivateProfileIntW(L"show", L"cpu_top", 0, ini) != 0;
    c.showMemTop = GetPrivateProfileIntW(L"show", L"mem_top", 0, ini) != 0;

    GetPrivateProfileStringW(L"colors", L"background", L"1c1c1e", buf, 128, ini);
    c.colBg = ParseHexColor(buf, c.colBg);
    GetPrivateProfileStringW(L"colors", L"text", L"e6e6e6", buf, 128, ini);
    c.colText = ParseHexColor(buf, c.colText);
    GetPrivateProfileStringW(L"colors", L"accent", L"4aa3ff", buf, 128, ini);
    c.colAccent = ParseHexColor(buf, c.colAccent);
    GetPrivateProfileStringW(L"colors", L"warn", L"ff9f43", buf, 128, ini);
    c.colWarn = ParseHexColor(buf, c.colWarn);
}

static void WriteInt(const wchar_t* sec, const wchar_t* key, int v, const wchar_t* ini) {
    wchar_t b[32]; _itow_s(v, b, 32, 10);
    WritePrivateProfileStringW(sec, key, b, ini);
}

void Config_Save(const Config& c) {
    wchar_t ini[MAX_PATH]; Config_Path(ini, MAX_PATH);

    WritePrivateProfileStringW(L"general", L"mode",
        c.mode == DisplayMode::Bars ? L"bars" : L"minimal", ini);
    WriteInt(L"general", L"opacity",            c.opacity, ini);
    WriteInt(L"general", L"refresh_ms",         c.refreshMs, ini);
    WriteInt(L"general", L"x",                  c.x, ini);
    WriteInt(L"general", L"y",                  c.y, ini);
    WriteInt(L"general", L"topmost",            c.topMost, ini);
    WriteInt(L"general", L"click_through",      c.clickThrough, ini);
    WriteInt(L"general", L"autostart",          c.autoStart, ini);
    WriteInt(L"general", L"hide_on_fullscreen", c.hideOnFullscreen, ini);

    WriteInt(L"show", L"cpu",     c.showCpu, ini);
    WriteInt(L"show", L"mem",     c.showMem, ini);
    WriteInt(L"show", L"ip",      c.showIp, ini);
    WriteInt(L"show", L"cpu_top", c.showCpuTop, ini);
    WriteInt(L"show", L"mem_top", c.showMemTop, ini);

    wchar_t hex[16];
    FormatHexColor(c.colBg, hex, 16);     WritePrivateProfileStringW(L"colors", L"background", hex, ini);
    FormatHexColor(c.colText, hex, 16);   WritePrivateProfileStringW(L"colors", L"text", hex, ini);
    FormatHexColor(c.colAccent, hex, 16); WritePrivateProfileStringW(L"colors", L"accent", hex, ini);
    FormatHexColor(c.colWarn, hex, 16);   WritePrivateProfileStringW(L"colors", L"warn", hex, ini);
}
