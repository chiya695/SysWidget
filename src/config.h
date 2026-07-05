#pragma once
#include <windows.h>

enum class DisplayMode { Minimal = 0, Bars = 1 };

// Everything the widget remembers between runs. Plain struct, copied freely.
struct Config {
    DisplayMode mode        = DisplayMode::Minimal;
    int  opacity            = 92;      // 0..100
    int  refreshMs          = 1000;    // 500..5000
    int  x                  = 120;
    int  y                  = 120;
    bool topMost            = true;
    bool clickThrough       = false;
    bool autoStart          = false;
    bool hideOnFullscreen   = true;

    // per-item visibility
    bool showCpu            = true;
    bool showMem            = true;
    bool showIp             = true;
    bool showCpuTop         = false;
    bool showMemTop         = false;

    // colors (0x00RRGGBB)
    COLORREF colBg          = RGB(0x1c, 0x1c, 0x1e);
    COLORREF colText        = RGB(0xe6, 0xe6, 0xe6);
    COLORREF colAccent      = RGB(0x4a, 0xa3, 0xff);
    COLORREF colWarn        = RGB(0xff, 0x9f, 0x43);
};

// Path to config.ini sitting next to the running exe.
void  Config_Path(wchar_t* out, size_t cch);

// Load config.ini; if missing, seed it from config.ini.default (or built-in
// defaults) and write it out. Never fails hard — falls back to defaults.
void  Config_Load(Config& c);

// Persist current values back to config.ini.
void  Config_Save(const Config& c);
