#pragma once
#include <windows.h>
#include "config.h"

struct ProcInfo {
    DWORD   pid;
    double  cpu;        // percent of whole machine, 0..100
    ULONGLONG mem;      // working set, bytes
    wchar_t name[48];
};

struct Metrics {
    double     cpu = 0.0;          // overall CPU load, 0..100
    ULONGLONG  memUsed = 0;        // bytes
    ULONGLONG  memTotal = 0;       // bytes
    double     memPercent = 0.0;

    wchar_t    ip[64] = L"";       // primary local IPv4, or "-"

    ProcInfo   cpuTop[5];
    int        cpuTopN = 0;
    ProcInfo   memTop[5];
    int        memTopN = 0;
};

// Call once at startup.
void Metrics_Init();

// Sample only what `cfg` asks to display. `tick` is a monotonically increasing
// counter (one per refresh) used to rate-limit the cheaper-but-not-free IP lookup.
void Metrics_Update(const Config& cfg, Metrics& out, unsigned tick);

// Release any cached buffers.
void Metrics_Shutdown();
