#include "render.h"
#include <vector>
#include <string>
#include <stdio.h>
#include <string.h>
#include <cwctype>

// ---------------------------------------------------------------------------
// Layout is described as a flat list of "lines", built once per frame. Both the
// measure pass and the paint pass walk the same list, so they can never drift
// out of sync.
// ---------------------------------------------------------------------------

struct Line {
    int         indent;      // logical px
    std::wstring label;      // left text (may be empty)
    std::wstring value;      // right text
    bool        hasBar;      // draw a mini bar (bars mode)
    double      frac;        // 0..1 bar fill
    COLORREF    valueColor;
    bool        isSmall;     // smaller font (process rows); 'small' is a WinAPI macro
};

// logical (96-dpi) metrics; scaled by dpi at use
static const int PAD      = 10;
static const int LINE_H   = 22;
static const int SUB_H    = 17;
static const int GAP      = 14;   // min gap between label and value
static const int BAR_H    = 4;
static const int MIN_W    = 132;

static HFONT g_font = nullptr;
static HFONT g_fontSmall = nullptr;
static int   g_fontDpi = 0;

static int Scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

static void EnsureFonts(int dpi) {
    if (g_font && g_fontDpi == dpi) return;
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }
    g_fontDpi = dpi;

    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(11, dpi, 72);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_font = CreateFontIndirectW(&lf);

    lf.lfHeight = -MulDiv(9, dpi, 72);
    lf.lfWeight = FW_NORMAL;
    g_fontSmall = CreateFontIndirectW(&lf);
}

void Render_Init() { /* fonts are created lazily once we know the DPI */ }

void Render_Free() {
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }
    g_fontDpi = 0;
}

// pretty-print a byte count as "7.9 GB" / "345 MB"
static void FmtBytes(ULONGLONG b, wchar_t* out, size_t cch) {
    double gb = (double)b / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) swprintf_s(out, cch, L"%.1f GB", gb);
    else           swprintf_s(out, cch, L"%llu MB", b / (1024ull * 1024ull));
}

// trim a trailing ".exe" so process names read cleaner
static std::wstring PrettyName(const wchar_t* n) {
    std::wstring s = n ? n : L"?";
    size_t len = s.size();
    if (len > 4) {
        std::wstring tail = s.substr(len - 4);
        for (auto& c : tail) c = (wchar_t)towlower(c);
        if (tail == L".exe") s.erase(len - 4);
    }
    return s;
}

static void BuildLines(const Config& cfg, const Metrics& m, std::vector<Line>& out) {
    wchar_t buf[128];

    if (cfg.showCpu) {
        double f = m.cpu / 100.0;
        swprintf_s(buf, L"%.0f%%", m.cpu);
        COLORREF c = (m.cpu >= 85.0) ? cfg.colWarn : cfg.colAccent;
        out.push_back({ 0, L"CPU", buf, true, f, c, false });

        if (cfg.showCpuTop) {
            for (int i = 0; i < m.cpuTopN; ++i) {
                swprintf_s(buf, L"%.0f%%", m.cpuTop[i].cpu);
                out.push_back({ 12, PrettyName(m.cpuTop[i].name), buf, false, 0, cfg.colText, true });
            }
        }
    }

    if (cfg.showMem) {
        wchar_t used[32], tot[32];
        FmtBytes(m.memUsed, used, 32);
        FmtBytes(m.memTotal, tot, 32);
        swprintf_s(buf, L"%s / %s", used, tot);
        double f = m.memPercent / 100.0;
        COLORREF c = (m.memPercent >= 90.0) ? cfg.colWarn : cfg.colAccent;
        out.push_back({ 0, L"MEM", buf, true, f, c, false });

        if (cfg.showMemTop) {
            for (int i = 0; i < m.memTopN; ++i) {
                FmtBytes(m.memTop[i].mem, buf, 128);
                out.push_back({ 12, PrettyName(m.memTop[i].name), buf, false, 0, cfg.colText, true });
            }
        }
    }

    if (cfg.showIp) {
        out.push_back({ 0, L"IP", m.ip[0] ? m.ip : L"-", false, 0, cfg.colText, false });
    }
}

SIZE Render_Measure(const Config& cfg, const Metrics& m, int dpi) {
    EnsureFonts(dpi);
    std::vector<Line> lines; BuildLines(cfg, m, lines);

    HDC hdc = CreateCompatibleDC(nullptr);
    int maxContent = 0, height = Scale(PAD, dpi) * 2;

    for (const auto& ln : lines) {
        HFONT f = ln.isSmall ? g_fontSmall : g_font;
        HGDIOBJ old = SelectObject(hdc, f);
        SIZE ls = {}, vs = {};
        if (!ln.label.empty())
            GetTextExtentPoint32W(hdc, ln.label.c_str(), (int)ln.label.size(), &ls);
        GetTextExtentPoint32W(hdc, ln.value.c_str(), (int)ln.value.size(), &vs);
        SelectObject(hdc, old);

        int content = Scale(ln.indent, dpi) + ls.cx + Scale(GAP, dpi) + vs.cx;
        if (content > maxContent) maxContent = content;
        height += Scale(ln.isSmall ? SUB_H : LINE_H, dpi);
    }
    DeleteDC(hdc);

    if (lines.empty()) height = Scale(PAD, dpi) * 2 + Scale(LINE_H, dpi);

    int width = maxContent + Scale(PAD, dpi) * 2;
    int minw = Scale(MIN_W, dpi);
    if (width < minw) width = minw;

    SIZE s; s.cx = width; s.cy = height;
    return s;
}

void Render_Paint(HDC hdc, const RECT& client, const Config& cfg, const Metrics& m, int dpi) {
    EnsureFonts(dpi);
    std::vector<Line> lines; BuildLines(cfg, m, lines);

    const int W = client.right - client.left;
    const int H = client.bottom - client.top;
    const int pad = Scale(PAD, dpi);

    // background
    HBRUSH bg = CreateSolidBrush(cfg.colBg);
    RECT full = client;
    FillRect(hdc, &full, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);

    int y = pad;
    for (const auto& ln : lines) {
        HFONT f = ln.isSmall ? g_fontSmall : g_font;
        HGDIOBJ old = SelectObject(hdc, f);
        int lh = Scale(ln.isSmall ? SUB_H : LINE_H, dpi);

        // label (left)
        int x = pad + Scale(ln.indent, dpi);
        if (!ln.label.empty()) {
            SetTextColor(hdc, ln.isSmall ? cfg.colText : cfg.colText);
            RECT r{ x, y, W - pad, y + lh };
            DrawTextW(hdc, ln.label.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        // value (right aligned)
        SetTextColor(hdc, ln.valueColor);
        RECT rv{ x, y, W - pad, y + lh };
        DrawTextW(hdc, ln.value.c_str(), -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(hdc, old);

        // mini bar (bars mode only, for CPU/MEM lines)
        if (ln.hasBar && cfg.mode == DisplayMode::Bars) {
            int barY = y + lh - Scale(3, dpi);
            RECT track{ x, barY, W - pad, barY + Scale(BAR_H, dpi) };
            HBRUSH tb = CreateSolidBrush(RGB(
                GetRValue(cfg.colBg) + 30 > 255 ? 255 : GetRValue(cfg.colBg) + 30,
                GetGValue(cfg.colBg) + 30 > 255 ? 255 : GetGValue(cfg.colBg) + 30,
                GetBValue(cfg.colBg) + 30 > 255 ? 255 : GetBValue(cfg.colBg) + 30));
            FillRect(hdc, &track, tb);
            DeleteObject(tb);

            double frac = ln.frac; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
            RECT fillr = track;
            fillr.right = track.left + (int)((track.right - track.left) * frac);
            HBRUSH fb = CreateSolidBrush(ln.valueColor);
            FillRect(hdc, &fillr, fb);
            DeleteObject(fb);
        }

        y += lh;
    }
}
