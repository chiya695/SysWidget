#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "metrics.h"

#include <vector>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// NtQuerySystemInformation gives us every process's CPU time + working set in a
// single call. That is dramatically cheaper than Toolhelp snapshots or PDH, and
// it is the whole reason the top-5 feature costs almost nothing.
// ---------------------------------------------------------------------------
#define SW_SystemProcessInformation 5
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((LONG)0xC0000004L)
#endif

typedef struct _SW_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} SW_UNICODE_STRING;

// Layout matches the kernel's SYSTEM_PROCESS_INFORMATION up to the field we need.
// Identical on x64 and ARM64 (both LLP64, natural alignment). We walk entries via
// NextEntryOffset, so truncating after WorkingSetSize is safe.
typedef struct _SW_SYSTEM_PROCESS_INFORMATION {
    ULONG           NextEntryOffset;
    ULONG           NumberOfThreads;
    LARGE_INTEGER   WorkingSetPrivateSize;
    ULONG           HardFaultCount;
    ULONG           NumberOfThreadsHighWatermark;
    ULONGLONG       CycleTime;
    LARGE_INTEGER   CreateTime;
    LARGE_INTEGER   UserTime;
    LARGE_INTEGER   KernelTime;
    SW_UNICODE_STRING ImageName;
    LONG            BasePriority;
    HANDLE          UniqueProcessId;
    HANDLE          InheritedFromUniqueProcessId;
    ULONG           HandleCount;
    ULONG           SessionId;
    ULONG_PTR       UniqueProcessKey;
    SIZE_T          PeakVirtualSize;
    SIZE_T          VirtualSize;
    ULONG           PageFaultCount;
    SIZE_T          PeakWorkingSetSize;
    SIZE_T          WorkingSetSize;
} SW_SYSTEM_PROCESS_INFORMATION;

typedef LONG (WINAPI *PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);

static PFN_NtQuerySystemInformation g_NtQSI = nullptr;

// reusable buffer for the process snapshot (avoids re-alloc churn every tick)
static BYTE*  g_procBuf = nullptr;
static ULONG  g_procBufSize = 0;

// previous per-process CPU time, to turn cumulative time into a rate
struct PrevProc { DWORD pid; ULONGLONG created; ULONGLONG time; };
static std::vector<PrevProc> g_prev;
static ULONGLONG g_prevProcBase = 0;

// previous system totals for overall CPU%
static ULONGLONG g_prevIdle = 0, g_prevKernel = 0, g_prevUser = 0;

// cached IP so we don't hammer GetAdaptersAddresses every second
static wchar_t g_ipCache[64] = L"";

void Metrics_Init() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt) g_NtQSI = (PFN_NtQuerySystemInformation)GetProcAddress(nt, "NtQuerySystemInformation");
    g_prev.reserve(512);
}

void Metrics_Shutdown() {
    if (g_procBuf) { free(g_procBuf); g_procBuf = nullptr; g_procBufSize = 0; }
    g_prev.clear();
    g_prev.shrink_to_fit();
}

static ULONGLONG FtToU64(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static ULONGLONG LiToU64(const LARGE_INTEGER& li) {
    return (ULONGLONG)li.QuadPart;
}

// ---- overall CPU load ------------------------------------------------------
static void SampleCpu(Metrics& out) {
    FILETIME idle, kern, user;
    if (!GetSystemTimes(&idle, &kern, &user)) return;
    ULONGLONG i = FtToU64(idle), k = FtToU64(kern), u = FtToU64(user);

    ULONGLONG di = i - g_prevIdle;
    ULONGLONG dk = k - g_prevKernel;
    ULONGLONG du = u - g_prevUser;
    g_prevIdle = i; g_prevKernel = k; g_prevUser = u;

    // kernel time already includes idle; total capacity == dk + du
    ULONGLONG total = dk + du;
    if (total > 0) {
        double busy = (double)(total - di) / (double)total;
        if (busy < 0) busy = 0; if (busy > 1) busy = 1;
        out.cpu = busy * 100.0;
    }
}

// ---- system memory ---------------------------------------------------------
static void SampleMem(Metrics& out) {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out.memTotal   = ms.ullTotalPhys;
        out.memUsed    = ms.ullTotalPhys - ms.ullAvailPhys;
        out.memPercent = out.memTotal ? (double)out.memUsed * 100.0 / (double)out.memTotal : 0.0;
    }
}

// ---- primary local IPv4 ----------------------------------------------------
static void SampleIp(wchar_t* out, size_t cch) {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15 * 1024;
    IP_ADAPTER_ADDRESSES* buf = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (!buf) { wcscpy_s(out, cch, L"-"); return; }

    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, buf, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        free(buf);
        buf = (IP_ADAPTER_ADDRESSES*)malloc(size);
        if (!buf) { wcscpy_s(out, cch, L"-"); return; }
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, buf, &size);
    }

    wcscpy_s(out, cch, L"-");
    if (r == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = buf; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                sockaddr_in* si = (sockaddr_in*)ua->Address.lpSockaddr;
                BYTE* b = (BYTE*)&si->sin_addr;
                swprintf_s(out, cch, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                free(buf);
                return;
            }
        }
    }
    free(buf);
}

// ---- top-5 processes by CPU and by memory ----------------------------------
static ULONGLONG FindPrev(DWORD pid, ULONGLONG created) {
    auto found = std::lower_bound(g_prev.begin(), g_prev.end(), pid,
        [](const PrevProc& row, DWORD value) { return row.pid < value; });
    if (found != g_prev.end() && found->pid == pid && found->created == created)
        return found->time;
    return 0;
}

static void SampleProcs(const Config& cfg, Metrics& out) {
    out.cpuTopN = 0; out.memTopN = 0;
    if (!g_NtQSI) return;
    const bool needCpu = cfg.showCpu && cfg.showCpuTop;

    bool snapshotReady = false;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (g_procBufSize == 0) { g_procBufSize = 512 * 1024; g_procBuf = (BYTE*)malloc(g_procBufSize); }
        if (!g_procBuf) { g_procBufSize = 0; return; }
        ULONG needed = 0;
        LONG st = g_NtQSI(SW_SystemProcessInformation, g_procBuf, g_procBufSize, &needed);
        if (st == 0) { snapshotReady = true; break; }
        if (st == STATUS_INFO_LENGTH_MISMATCH) {
            free(g_procBuf);
            g_procBufSize = needed + 64 * 1024;
            g_procBuf = (BYTE*)malloc(g_procBufSize);
            continue;
        }
        return; // unexpected error
    }
    if (!snapshotReady) return;

    struct Row { DWORD pid; double cpu; ULONGLONG mem; const wchar_t* name; USHORT nameLen; };
    std::vector<Row> rows;
    rows.reserve(g_prev.size() ? g_prev.size() : 256);

    std::vector<PrevProc> cur;
    if (needCpu) cur.reserve(rows.capacity());

    // wall-clock capacity since last sample = (kernel+user) delta, already tracked
    // by SampleCpu; recompute a local denominator from GetSystemTimes so this works
    // even when the CPU line is hidden.
    FILETIME idle, kern, user;
    ULONGLONG denom = 0;
    if (needCpu && GetSystemTimes(&idle, &kern, &user)) {
        ULONGLONG nowBase = FtToU64(kern) + FtToU64(user);
        denom = (g_prevProcBase && nowBase > g_prevProcBase) ? (nowBase - g_prevProcBase) : 0;
        g_prevProcBase = nowBase;
    }

    BYTE* p = g_procBuf;
    for (;;) {
        SW_SYSTEM_PROCESS_INFORMATION* spi = (SW_SYSTEM_PROCESS_INFORMATION*)p;
        DWORD pid = (DWORD)(ULONG_PTR)spi->UniqueProcessId;
        if (pid != 0) {
            ULONGLONG t = LiToU64(spi->UserTime) + LiToU64(spi->KernelTime);
            ULONGLONG created = LiToU64(spi->CreateTime);
            ULONGLONG prev = needCpu ? FindPrev(pid, created) : 0;
            double cpu = 0.0;
            if (denom && prev && t >= prev)
                cpu = (double)(t - prev) * 100.0 / (double)denom;
            if (cpu > 100.0) cpu = 100.0;

            Row row;
            row.pid = pid;
            row.cpu = cpu;
            row.mem = (ULONGLONG)spi->WorkingSetSize;
            row.name = spi->ImageName.Buffer ? spi->ImageName.Buffer : L"";
            row.nameLen = spi->ImageName.Buffer ? (USHORT)(spi->ImageName.Length / sizeof(WCHAR)) : 0;
            rows.push_back(row);

            if (needCpu) cur.push_back({ pid, created, t });
        }
        if (spi->NextEntryOffset == 0) break;
        p += spi->NextEntryOffset;
    }
    std::sort(cur.begin(), cur.end(),
        [](const PrevProc& first, const PrevProc& second) { return first.pid < second.pid; });
    g_prev.swap(cur);
    if (!needCpu) g_prevProcBase = 0;

    auto fill = [](ProcInfo* dst, int& n, std::vector<Row>& rws, bool byCpu) {
        std::partial_sort(rws.begin(),
                          rws.begin() + (rws.size() < 5 ? rws.size() : 5),
                          rws.end(),
                          [byCpu](const Row& a, const Row& b) {
                              return byCpu ? (a.cpu > b.cpu) : (a.mem > b.mem);
                          });
        n = 0;
        for (size_t i = 0; i < rws.size() && n < 5; ++i) {
            ProcInfo& pi = dst[n];
            pi.pid = rws[i].pid;
            pi.cpu = rws[i].cpu;
            pi.mem = rws[i].mem;
            USHORT len = rws[i].nameLen; if (len > 47) len = 47;
            if (len && rws[i].name) { wmemcpy(pi.name, rws[i].name, len); pi.name[len] = 0; }
            else wcscpy_s(pi.name, 48, L"?");
            ++n;
        }
    };

    if (cfg.showCpu && cfg.showCpuTop) fill(out.cpuTop, out.cpuTopN, rows, true);
    if (cfg.showMem && cfg.showMemTop) fill(out.memTop, out.memTopN, rows, false);
}

void Metrics_ResetCpu() {
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        g_prevIdle = FtToU64(idle);
        g_prevKernel = FtToU64(kernel);
        g_prevUser = FtToU64(user);
    }
    g_prevProcBase = 0;
    g_prev.clear();
}

void Metrics_Update(const Config& cfg, Metrics& out, unsigned tick) {
    if (cfg.showCpu) SampleCpu(out);
    if (cfg.showMem) SampleMem(out);

    if (cfg.showIp) {
        // refresh at most every ~5 s; reuse cache otherwise
        if (tick == 0 || g_ipCache[0] == 0 || (tick % 5) == 0)
            SampleIp(g_ipCache, 64);
        wcscpy_s(out.ip, 64, g_ipCache);
    }

    if ((cfg.showCpu && cfg.showCpuTop) || (cfg.showMem && cfg.showMemTop))
        SampleProcs(cfg, out);
    else {
        out.cpuTopN = 0;
        out.memTopN = 0;
        g_prev.clear();
        g_prevProcBase = 0;
    }
}
