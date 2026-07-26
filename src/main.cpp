// PlainAmp - a small, offline, no-nonsense audio player for Windows.
#include "common.h"
#include "audio.h"
#include "playlist.h"
#include "config.h"
#include "theme.h"
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>

#define GET_X_LPARAM_(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM_(lp) ((int)(short)HIWORD(lp))

// ---------------------------------------------------------------------------
// ids
// ---------------------------------------------------------------------------
enum Hit {
    H_NONE = 0,
    H_PREV, H_RW, H_PLAY, H_STOP, H_FF, H_NEXT,
    H_SEEK, H_VOL, H_BAL, H_TIME,
    H_OPEN, H_FOLDER, H_SAVE, H_CLEAR,
    H_SHUFFLE, H_REPEAT, H_EQ, H_VIS, H_THEME, H_EXCL, H_DEVICE,
    H_EQON, H_EQFLAT, H_EQPRE,
    H_PLIST, H_SCROLL,
    H_EQBAND = 100                 // +band
};

enum DragKind { D_NONE, D_SEEK, D_VOL, D_BAL, D_EQ, D_SCROLL };

struct Chip { RECT r; int id; wstr label; bool active; bool wide; };
struct Btn { RECT r; int id; int glyph; };   // glyph: 0 prev 1 rw 2 play 3 pause 4 stop 5 ff 6 next

// ---------------------------------------------------------------------------
// app state
// ---------------------------------------------------------------------------
struct App {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    Config cfg;
    Player player;
    Playlist pl;
    MetaCache meta;
    Spectrum spec;

    float  scaleF = 1.0f;
    int    dpi = 96;
    int    cw = 0, chh = 0;

    HDC     memDC = nullptr;
    HBITMAP memBmp = nullptr;
    int     memW = 0, memH = 0;
    HFONT   fBody = nullptr, fBold = nullptr, fSmall = nullptr, fTiny = nullptr;

    // layout
    RECT rcVis{}, rcInfo{}, rcSeek{}, rcTimeL{}, rcTimeR{},
        rcVol{}, rcBal{}, rcEqPanel{}, rcPlist{}, rcStatus{}, rcScroll{};
    std::vector<Btn>  btns;
    std::vector<Chip> chips;
    RECT rcEqBand[EQ_BANDS]{}, rcEqPre{};

    // interaction
    int  hot = H_NONE;
    DragKind drag = D_NONE;
    int  dragBand = -1;
    double seekPreview = -1.0;
    int  scrollTop = 0;
    int  anchor = -1;
    std::vector<char> selected;
    int  scrollGrabDy = 0;

    // vis
    float visSamples[VIS_FFT] = { 0 };
    float bars[VIS_BARS] = { 0 };

    wstr status;
    int  failCount = 0;
    int  timerMs = 33;
    Player::Stats st{};
    ULONGLONG lastStatTick = 0;
    ULONGLONG lastDevReset = 0;
    bool      plDirty = false;
    ULONGLONG plDirtyAt = 0;
    ULONGLONG lastSessionSave = 0;
};
static App g;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static int S(int v) { return (int)(v * g.scaleF + 0.5f); }
static bool In(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
static RECT MakeR(int l, int t, int r, int b) { RECT x{ l,t,r,b }; return x; }

static void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void RoundBox(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH hb = CreateSolidBrush(fill);
    HPEN   hp = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, hb), op = SelectObject(dc, hp);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(hb); DeleteObject(hp);
}

static void Text(HDC dc, const RECT& r, const wstr& s, COLORREF c, HFONT f, UINT fmt) {
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, f);
    RECT rr = r;
    DrawTextW(dc, s.c_str(), -1, &rr, fmt);
    SelectObject(dc, of);
}

static int TextW(HDC dc, const wstr& s, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

static void Tri(HDC dc, POINT a, POINT b, POINT c, COLORREF col) {
    POINT p[3] = { a,b,c };
    HBRUSH hb = CreateSolidBrush(col);
    HPEN hp = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ ob = SelectObject(dc, hb), op = SelectObject(dc, hp);
    Polygon(dc, p, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(hb); DeleteObject(hp);
}

static void Glyph(HDC dc, const RECT& r, int kind, COLORREF col) {
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int h = (r.bottom - r.top) / 2 - S(6); if (h < 3) h = 3;
    int w = h;
    switch (kind) {
    case 2: // play
        Tri(dc, { cx - w / 2, cy - h }, { cx - w / 2, cy + h }, { cx + w, cy }, col); break;
    case 3: { // pause
        RECT a = MakeR(cx - w, cy - h, cx - w + S(4), cy + h);
        RECT b = MakeR(cx + w - S(4), cy - h, cx + w, cy + h);
        Fill(dc, a, col); Fill(dc, b, col); break;
    }
    case 4: { RECT a = MakeR(cx - h, cy - h, cx + h, cy + h); Fill(dc, a, col); break; }
    case 0: { // prev
        RECT bar = MakeR(cx - w - S(2), cy - h, cx - w + S(1), cy + h);
        Fill(dc, bar, col);
        Tri(dc, { cx + w, cy - h }, { cx + w, cy + h }, { cx - w + S(2), cy }, col); break;
    }
    case 6: { // next
        RECT bar = MakeR(cx + w - S(1), cy - h, cx + w + S(2), cy + h);
        Fill(dc, bar, col);
        Tri(dc, { cx - w, cy - h }, { cx - w, cy + h }, { cx + w - S(2), cy }, col); break;
    }
    case 1: // rewind
        Tri(dc, { cx, cy - h }, { cx, cy + h }, { cx - w, cy }, col);
        Tri(dc, { cx + w, cy - h }, { cx + w, cy + h }, { cx, cy }, col); break;
    case 5: // fast forward
        Tri(dc, { cx - w, cy - h }, { cx - w, cy + h }, { cx, cy }, col);
        Tri(dc, { cx, cy - h }, { cx, cy + h }, { cx + w, cy }, col); break;
    }
}

static void MakeFonts() {
    if (g.fBody) { DeleteObject(g.fBody); DeleteObject(g.fBold); DeleteObject(g.fSmall); DeleteObject(g.fTiny); }
    auto mk = [](int pt, int weight) {
        return CreateFontW(-S(pt), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        };
    g.fBody = mk(13, FW_NORMAL);
    g.fBold = mk(17, FW_SEMIBOLD);
    g.fSmall = mk(12, FW_NORMAL);
    g.fTiny = mk(11, FW_NORMAL);
}

static void SetDarkTitleBar(HWND h, bool dark) {
    BOOL v = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &v, sizeof(v))))
        DwmSetWindowAttribute(h, 19, &v, sizeof(v));
}

// ---------------------------------------------------------------------------
// playlist labels
// ---------------------------------------------------------------------------
static wstr RowLabel(int i) {
    const Track& t = g.pl.items[i];
    Meta m = g.meta.get(t.path);
    if (!m.resolved) return FileNameOf(t.path);
    wstr title = m.title.empty() ? FileNameOf(t.path) : m.title;
    if (!m.artist.empty()) return m.artist + L" \x2014 " + title;
    return title;
}

static double RowDur(int i) {
    Meta m;
    if (g.meta.peek(g.pl.items[i].path, m) && m.resolved) return m.duration;
    return 0.0;
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
static void ApplyEqFromCfg() {
    g.player.eq().setEnabled(g.cfg.eqOn);
    g.player.eq().setPreamp(g.cfg.preamp);
    for (int i = 0; i < EQ_BANDS; ++i) g.player.eq().setGain(i, g.cfg.eqGain[i]);
}

static void UpdateTitle() {
    wstr t = L"PlainAmp";
    if (g.pl.current >= 0 && g.pl.current < g.pl.size())
        t = RowLabel(g.pl.current) + L"  \x2014  PlainAmp";
    SetWindowTextW(g.hwnd, t.c_str());
}

static void EnsureVisible(int idx) {
    if (idx < 0) return;
    int rowH = S(21);
    int vis = (g.rcPlist.bottom - g.rcPlist.top) / (rowH > 0 ? rowH : 1);
    if (vis < 1) return;
    if (idx < g.scrollTop) g.scrollTop = idx;
    else if (idx >= g.scrollTop + vis) g.scrollTop = idx - vis + 1;
    if (g.scrollTop < 0) g.scrollTop = 0;
}

static void PlayIndex(int i, bool autoAdvance = false) {
    if (i < 0 || i >= g.pl.size()) { g.player.stop(); return; }
    g.pl.current = i;
    g.selected.assign(g.pl.items.size(), 0);
    if (i < (int)g.selected.size()) g.selected[i] = 1;

    if (!g.player.open(g.pl.items[i].path)) {
        g.status = L"Cannot play: " + FileNameOf(g.pl.items[i].path);
        if (++g.failCount < 8) {
            int n = g.pl.nextIndex(false);
            if (n >= 0 && n != i) { PlayIndex(n, true); return; }
        }
        g.failCount = 0;
        return;
    }
    g.failCount = 0;
    g.player.setVolume(g.cfg.volume);
    g.player.setBalance(g.cfg.balance);
    ApplyEqFromCfg();
    g.player.play();
    g.status.clear();
    EnsureVisible(i);
    UpdateTitle();
    (void)autoAdvance;
}

// Rebuild the audio stream on the currently selected device, keeping the
// position and whether we were playing. Used for device switch / device loss.
static void ReopenAudio() {
    if (g.pl.current < 0 || g.pl.current >= g.pl.size()) return;
    double pos = g.player.position();
    bool wasPlaying = (g.player.state() == PlayState::Playing);
    if (!g.player.open(g.pl.items[g.pl.current].path)) {
        g.status = L"Output device unavailable";
        return;
    }
    g.player.setVolume(g.cfg.volume);
    g.player.setBalance(g.cfg.balance);
    ApplyEqFromCfg();
    if (pos > 0.25) g.player.seek(pos);
    if (wasPlaying) g.player.play();
}

static void ShowDeviceMenu(POINT screenPt) {
    std::vector<AudioDevice> devs = g.player.listDevices();
    HMENU m = CreatePopupMenu();
    if (!m) return;

    AppendMenuW(m, MF_STRING | (g.cfg.deviceId.empty() ? MF_CHECKED : 0), 1,
        L"Default device (follow Windows)");
    if (!devs.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < devs.size(); ++i) {
        wstr label = devs[i].name;
        if (devs[i].isDefault) label += L"   \x2014  default";
        AppendMenuW(m, MF_STRING | (g.cfg.deviceId == devs[i].id ? MF_CHECKED : 0),
            (UINT)(2 + i), label.c_str());
    }

    SetForegroundWindow(g.hwnd);
    int sel = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
        screenPt.x, screenPt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (sel <= 0) return;

    wstr id = (sel == 1) ? wstr() : devs[(size_t)sel - 2].id;
    if (id == g.cfg.deviceId) return;
    g.cfg.deviceId = id;
    g.player.setDeviceId(id);
    g.status.clear();
    ReopenAudio();
}

static void PlayNext(bool manual) {
    int n = g.pl.nextIndex(manual);
    if (n < 0) { g.player.stop(); g.status = L"End of playlist"; return; }
    if (g.pl.repeat == 2 && !manual && n == g.pl.current) { g.player.seek(0); g.player.play(); return; }
    PlayIndex(n);
}

static void PlayPrev() {
    if (g.player.hasTrack() && g.player.position() > 3.0) { g.player.seek(0); return; }
    int p = g.pl.prevIndex();
    if (p >= 0) PlayIndex(p);
}

// The playlist is written back 1.5 s after the last change (see WM_TIMER), so
// it survives a crash or a kill from Task Manager, not just a clean exit.
static void TouchPlaylist() {
    g.plDirty = true;
    g.plDirtyAt = GetTickCount64();
}

// Playlist file + the "carry on where I left off" marker.
static void SaveSession() {
    g.pl.saveM3U(Config::playlistPath());
    g.cfg.resumeIndex = g.pl.current;
    g.cfg.resumePos = g.player.hasTrack() ? g.player.position() : 0.0;
    g.plDirty = false;
}

// Open a track and park it at 'seekTo' without starting playback.
static void CueIndex(int i, double seekTo) {
    if (i < 0 || i >= g.pl.size()) return;
    g.pl.current = i;
    g.selected.assign(g.pl.items.size(), 0);
    g.selected[i] = 1;
    if (!g.player.open(g.pl.items[i].path)) {
        g.status = L"Cannot open: " + FileNameOf(g.pl.items[i].path);
        g.pl.current = -1;
        return;
    }
    g.player.setVolume(g.cfg.volume);
    g.player.setBalance(g.cfg.balance);
    ApplyEqFromCfg();
    if (seekTo > 0.25) g.player.seek(seekTo);
    EnsureVisible(i);
    UpdateTitle();
}

static void AddPaths(const std::vector<wstr>& paths, bool replace) {
    if (replace) { g.pl.clear(); g.scrollTop = 0; }
    for (const auto& p : paths) g.pl.addPath(p);
    g.selected.assign(g.pl.items.size(), 0);
    wchar_t buf[80];
    swprintf(buf, 80, L"%d track%s in playlist", g.pl.size(), g.pl.size() == 1 ? L"" : L"s");
    g.status = buf;
    TouchPlaylist();
}

static void DoOpenFiles(bool replace) {
    std::vector<wchar_t> buf(64 * 1024, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter =
        L"Audio files\0*.mp3;*.flac;*.wav;*.m4a;*.aac;*.wma;*.aif;*.aiff;*.ogg;*.opus;*.m3u;*.m3u8\0"
        L"Lossless\0*.flac;*.wav;*.aif;*.aiff;*.m4a\0"
        L"All files\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<wstr> paths;
    const wchar_t* p = buf.data();
    wstr dir = p;
    p += dir.size() + 1;
    if (*p == 0) paths.push_back(dir);                       // single selection
    else {
        if (dir.back() != L'\\') dir += L'\\';
        while (*p) { paths.push_back(dir + p); p += wcslen(p) + 1; }
    }
    AddPaths(paths, replace);
}

static void DoOpenFolder(bool replace) {
    BROWSEINFOW bi{};
    wchar_t disp[MAX_PATH] = { 0 };
    bi.hwndOwner = g.hwnd;
    bi.pszDisplayName = disp;
    bi.lpszTitle = L"Add a music folder (searched recursively)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return;
    wchar_t path[MAX_PATH] = { 0 };
    if (SHGetPathFromIDListW(idl, path)) AddPaths({ path }, replace);
    CoTaskMemFree(idl);
}

static void DoSavePlaylist() {
    wchar_t buf[MAX_PATH] = L"playlist.m3u8";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Playlist\0*.m3u8\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"m3u8";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn))
        g.status = g.pl.saveM3U(buf) ? L"Playlist saved" : L"Could not save playlist";
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
static void Layout(HDC dc) {
    g.btns.clear();
    g.chips.clear();

    const int PAD = S(12);
    int x = PAD, w = g.cw - PAD * 2;
    int y = PAD;
    if (w < S(200)) w = S(200);

    // --- header -----------------------------------------------------------
    int headH = S(66);
    if (g.cfg.showVis) {
        g.rcVis = MakeR(x, y, x + S(148), y + headH);
        g.rcInfo = MakeR(g.rcVis.right + S(14), y, x + w, y + headH);
    } else {
        g.rcVis = MakeR(0, 0, 0, 0);
        g.rcInfo = MakeR(x, y, x + w, y + headH);
    }
    y += headH + S(10);

    // --- seek bar ---------------------------------------------------------
    int tw = S(56);
    g.rcTimeL = MakeR(x, y, x + tw, y + S(20));
    g.rcTimeR = MakeR(x + w - tw, y, x + w, y + S(20));
    g.rcSeek = MakeR(g.rcTimeL.right + S(8), y + S(6), g.rcTimeR.left - S(8), y + S(14));
    y += S(20) + S(12);

    // --- transport --------------------------------------------------------
    int bw = S(38), bh = S(30), gap = S(5);
    int bx = x;
    const int order[6] = { H_PREV, H_RW, H_PLAY, H_STOP, H_FF, H_NEXT };
    const int glyphs[6] = { 0, 1, 2, 4, 5, 6 };
    for (int i = 0; i < 6; ++i) {
        Btn b;
        b.r = MakeR(bx, y, bx + bw, y + bh);
        b.id = order[i];
        b.glyph = (order[i] == H_PLAY && g.player.state() == PlayState::Playing) ? 3 : glyphs[i];
        g.btns.push_back(b);
        bx += bw + gap;
    }

    int slW = S(112), slBal = S(78);
    int rightEnd = x + w;
    g.rcBal = MakeR(rightEnd - slBal, y + bh / 2 - S(4), rightEnd, y + bh / 2 + S(4));
    g.rcVol = MakeR(g.rcBal.left - S(14) - slW, y + bh / 2 - S(4), g.rcBal.left - S(14), y + bh / 2 + S(4));
    if (g.rcVol.left < bx + S(10)) {                 // narrow window: shrink
        g.rcVol.left = bx + S(10);
        if (g.rcVol.left > g.rcVol.right) g.rcVol.left = g.rcVol.right;
    }
    y += bh + S(12);

    // --- chips ------------------------------------------------------------
    struct CDef { int id; const wchar_t* label; bool active; };
    wchar_t repLabel[24];
    swprintf(repLabel, 24, L"Repeat%s", g.pl.repeat == 2 ? L" 1" : (g.pl.repeat == 1 ? L" All" : L""));

    CDef defs[] = {
        { H_OPEN,    L"Open",      false },
        { H_FOLDER,  L"Folder",    false },
        { H_SAVE,    L"Save",      false },
        { H_CLEAR,   L"Clear",     false },
        { H_SHUFFLE, L"Shuffle",   g.pl.shuffle },
        { H_REPEAT,  repLabel,     g.pl.repeat != 0 },
        { H_EQ,      L"EQ",        g.cfg.showEq },
        { H_VIS,     L"Vis",       g.cfg.showVis },
        { H_EXCL,    L"Exclusive", g.cfg.exclusive },
        { H_DEVICE,  L"Device",    !g.cfg.deviceId.empty() },
        { H_THEME,   g.cfg.dark ? L"Light" : L"Dark", false },
    };

    int chipH = S(24);
    int cx = x, cy = y;
    for (const CDef& d : defs) {
        int tww = TextW(dc, d.label, g.fSmall) + S(18);
        if (cx + tww > x + w && cx > x) { cx = x; cy += chipH + S(5); }
        Chip c;
        c.r = MakeR(cx, cy, cx + tww, cy + chipH);
        c.id = d.id;
        c.label = d.label;
        c.active = d.active;
        c.wide = false;
        g.chips.push_back(c);
        cx += tww + S(5);
    }
    y = cy + chipH + S(12);

    // --- equaliser --------------------------------------------------------
    if (g.cfg.showEq) {
        int eqH = S(112);
        g.rcEqPanel = MakeR(x, y, x + w, y + eqH);
        int inner = S(10);
        int slotW = (w - inner * 2) / (EQ_BANDS + 1);
        int sx = x + inner;
        int top = g.rcEqPanel.top + S(18), bot = g.rcEqPanel.bottom - S(20);
        g.rcEqPre = MakeR(sx + slotW / 2 - S(4), top, sx + slotW / 2 + S(4), bot);
        for (int i = 0; i < EQ_BANDS; ++i) {
            int c0 = sx + slotW * (i + 1);
            g.rcEqBand[i] = MakeR(c0 + slotW / 2 - S(4), top, c0 + slotW / 2 + S(4), bot);
        }
        y += eqH + S(10);
    } else {
        g.rcEqPanel = MakeR(0, 0, 0, 0);
    }

    // --- status + playlist -------------------------------------------------
    int statusH = S(22);
    g.rcStatus = MakeR(x, g.chh - PAD - statusH, x + w, g.chh - PAD);
    int plTop = y;
    int plBot = g.rcStatus.top - S(8);
    if (plBot < plTop + S(40)) plBot = plTop + S(40);
    g.rcPlist = MakeR(x, plTop, x + w, plBot);

    int rowH = S(21);
    int visRows = (g.rcPlist.bottom - g.rcPlist.top) / (rowH > 0 ? rowH : 1);
    bool needScroll = g.pl.size() > visRows;
    g.rcScroll = needScroll
        ? MakeR(g.rcPlist.right - S(9), g.rcPlist.top, g.rcPlist.right, g.rcPlist.bottom)
        : MakeR(0, 0, 0, 0);

    int maxTop = std::max(0, g.pl.size() - visRows);
    if (g.scrollTop > maxTop) g.scrollTop = maxTop;
    if (g.scrollTop < 0) g.scrollTop = 0;
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------
static void DrawSlider(HDC dc, const RECT& r, float t, const Theme& th, bool centered) {
    RECT track = r;
    Fill(dc, track, th.track);
    int w = r.right - r.left;
    int kx = r.left + (int)(t * w);
    if (centered) {
        int mid = (r.left + r.right) / 2;
        RECT f = MakeR(std::min(mid, kx), r.top, std::max(mid, kx), r.bottom);
        Fill(dc, f, th.fill);
    } else {
        RECT f = MakeR(r.left, r.top, kx, r.bottom);
        Fill(dc, f, th.fill);
    }
    RECT knob = MakeR(kx - S(4), r.top - S(4), kx + S(4), r.bottom + S(4));
    RoundBox(dc, knob, th.knob, th.fill, S(6));
}

static void PaintAll(HDC dc) {
    const Theme& th = ThemeFor(g.cfg.dark);
    RECT full = MakeR(0, 0, g.cw, g.chh);
    Fill(dc, full, th.bg);

    Layout(dc);

    // Every section below is skipped when it lies outside the invalid region,
    // so a 30 fps analyser tick costs one small box instead of a whole window.
    // ---- visualiser -------------------------------------------------------
    if (g.cfg.showVis && g.rcVis.right > g.rcVis.left && RectVisible(dc, &g.rcVis)) {
        RoundBox(dc, g.rcVis, th.panel, th.border, S(6));
        int n = VIS_BARS;
        int innerL = g.rcVis.left + S(6), innerR = g.rcVis.right - S(6);
        int innerT = g.rcVis.top + S(6), innerB = g.rcVis.bottom - S(6);
        int avail = innerR - innerL;
        int bwd = std::max(1, avail / n - 1);
        for (int i = 0; i < n; ++i) {
            int bx0 = innerL + avail * i / n;
            float v = g.bars[i];
            int h = (int)(v * (innerB - innerT));
            if (h < 1 && v > 0.01f) h = 1;
            RECT b = MakeR(bx0, innerB - h, bx0 + bwd, innerB);
            COLORREF c = v > 0.72f ? th.visHi : th.visLo;
            Fill(dc, b, c);
        }
    }

    // ---- track info -------------------------------------------------------
    if (RectVisible(dc, &g.rcInfo)) {
        wstr title = L"No track loaded", sub, tech;
        if (g.pl.current >= 0 && g.pl.current < g.pl.size()) {
            const wstr& p = g.pl.items[g.pl.current].path;
            Meta m = g.meta.get(p);
            title = m.resolved && !m.title.empty() ? m.title : FileNameOf(p);
            if (m.resolved) {
                sub = m.artist;
                if (!m.album.empty()) sub += (sub.empty() ? L"" : L"  \x2014  ") + m.album;
            }
        }
        if (g.player.hasTrack()) {
            const TrackFormat& f = g.player.format();
            wchar_t buf[200];
            if (f.lossless && f.srcBits > 0)
                swprintf(buf, 200, L"%s  \x00b7  %d Hz  \x00b7  %d-bit  \x00b7  %d ch",
                    f.codec.c_str(), f.sampleRate, f.srcBits, f.channels);
            else
                swprintf(buf, 200, L"%s  \x00b7  %d Hz  \x00b7  %d kbps  \x00b7  %d ch",
                    f.codec.c_str(), f.sampleRate, f.bitrateKbps, f.channels);
            tech = buf;
        }

        RECT r = g.rcInfo;
        RECT r1 = MakeR(r.left, r.top + S(2), r.right, r.top + S(26));
        RECT r2 = MakeR(r.left, r.top + S(26), r.right, r.top + S(45));
        RECT r3 = MakeR(r.left, r.top + S(45), r.right, r.top + S(64));
        Text(dc, r1, title, th.text, g.fBold, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        Text(dc, r2, sub, th.textDim, g.fSmall, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        Text(dc, r3, tech, g.player.format().lossless ? th.ok : th.textFaint,
            g.fTiny, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // ---- seek bar ---------------------------------------------------------
    RECT seekBand = { g.rcTimeL.left, g.rcTimeL.top - S(2), g.rcTimeR.right, g.rcTimeL.bottom + S(2) };
    if (RectVisible(dc, &seekBand)) {
        double dur = g.player.duration();
        double pos = g.seekPreview >= 0 ? g.seekPreview : g.player.position();
        float t = (dur > 0.01) ? (float)Clampd(pos / dur, 0.0, 1.0) : 0.0f;

        RECT tr = g.rcSeek;
        Fill(dc, tr, th.track);
        RECT f = MakeR(tr.left, tr.top, tr.left + (int)(t * (tr.right - tr.left)), tr.bottom);
        Fill(dc, f, th.fill);
        int kx = f.right;
        RECT knob = MakeR(kx - S(5), tr.top - S(5), kx + S(5), tr.bottom + S(5));
        RoundBox(dc, knob, th.knob, th.fill, S(8));

        Text(dc, g.rcTimeL, FormatTime(pos), th.text, g.fSmall, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        wstr right = g.cfg.elapsed ? FormatTime(dur)
            : FormatTime(dur > pos ? dur - pos : 0, true);
        Text(dc, g.rcTimeR, right, th.textDim, g.fSmall, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }

    // ---- transport --------------------------------------------------------
    for (const Btn& b : g.btns) {
        if (!RectVisible(dc, &b.r)) continue;
        bool hot = (g.hot == b.id);
        RoundBox(dc, b.r, hot ? th.hover : th.panelAlt, th.border, S(6));
        COLORREF c = (b.id == H_PLAY) ? th.accent : th.text;
        Glyph(dc, b.r, b.glyph, c);
    }

    // ---- volume / balance -------------------------------------------------
    RECT slBand = { g.rcVol.left, g.rcVol.top - S(6), g.rcBal.right, g.rcVol.bottom + S(20) };
    if (RectVisible(dc, &slBand)) {
        DrawSlider(dc, g.rcVol, g.cfg.volume, th, false);
        DrawSlider(dc, g.rcBal, (g.cfg.balance + 1.0f) * 0.5f, th, true);
        RECT l = MakeR(g.rcVol.left, g.rcVol.bottom + S(3), g.rcVol.right, g.rcVol.bottom + S(17));
        wchar_t buf[24]; swprintf(buf, 24, L"VOL %d%%", (int)(g.cfg.volume * 100 + 0.5f));
        Text(dc, l, buf, th.textFaint, g.fTiny, DT_SINGLELINE | DT_LEFT);
        RECT b = MakeR(g.rcBal.left, g.rcBal.bottom + S(3), g.rcBal.right, g.rcBal.bottom + S(17));
        Text(dc, b, L"BALANCE", th.textFaint, g.fTiny, DT_SINGLELINE | DT_RIGHT);
    }

    // ---- chips ------------------------------------------------------------
    for (const Chip& c : g.chips) {
        if (!RectVisible(dc, &c.r)) continue;
        bool hot = (g.hot == c.id);
        COLORREF fillc = c.active ? th.accentDim : (hot ? th.hover : th.panelAlt);
        COLORREF bord = c.active ? th.accent : th.border;
        COLORREF txt = c.active ? (g.cfg.dark ? th.knob : th.panel) : th.text;
        RoundBox(dc, c.r, fillc, bord, S(6));
        Text(dc, c.r, c.label, txt, g.fSmall, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    // ---- equaliser --------------------------------------------------------
    if (g.cfg.showEq && RectVisible(dc, &g.rcEqPanel)) {
        RoundBox(dc, g.rcEqPanel, th.panel, th.border, S(6));
        auto drawBand = [&](const RECT& r, float db, const wchar_t* label) {
            Fill(dc, r, th.track);
            int mid = (r.top + r.bottom) / 2;
            RECT z = MakeR(r.left - S(3), mid, r.right + S(3), mid + 1);
            Fill(dc, z, th.border);
            float t = (db + 12.0f) / 24.0f;
            int ky = r.bottom - (int)(t * (r.bottom - r.top));
            RECT f = MakeR(r.left, std::min(mid, ky), r.right, std::max(mid, ky));
            Fill(dc, f, th.fill);
            RECT knob = MakeR(r.left - S(4), ky - S(3), r.right + S(4), ky + S(3));
            RoundBox(dc, knob, th.knob, th.fill, S(4));
            RECT lr = MakeR(r.left - S(20), r.bottom + S(2), r.right + S(20), r.bottom + S(18));
            Text(dc, lr, label, th.textFaint, g.fTiny, DT_SINGLELINE | DT_CENTER);
            };
        drawBand(g.rcEqPre, g.cfg.preamp, L"PRE");
        for (int i = 0; i < EQ_BANDS; ++i) {
            wchar_t lab[16];
            if (EQ_FREQ[i] >= 1000) swprintf(lab, 16, L"%gk", EQ_FREQ[i] / 1000.0);
            else                    swprintf(lab, 16, L"%g", EQ_FREQ[i]);
            drawBand(g.rcEqBand[i], g.cfg.eqGain[i], lab);
        }
        RECT hdr = MakeR(g.rcEqPanel.left + S(8), g.rcEqPanel.top + S(2),
            g.rcEqPanel.right - S(8), g.rcEqPanel.top + S(16));
        Text(dc, hdr, g.cfg.eqOn ? L"EQUALIZER \x2014 on (click a band, right-click resets)"
            : L"EQUALIZER \x2014 off (drag a band to enable)",
            g.cfg.eqOn ? th.accent : th.textFaint, g.fTiny, DT_SINGLELINE | DT_LEFT);
    }

    // ---- playlist ---------------------------------------------------------
    if (RectVisible(dc, &g.rcPlist)) {
        RoundBox(dc, g.rcPlist, th.panel, th.border, S(6));
        int rowH = S(21);
        int visRows = (g.rcPlist.bottom - g.rcPlist.top) / rowH;
        int numW = S(38), durW = S(52);

        int savedDC = SaveDC(dc);          // compose with the caller's clip, don't replace it
        IntersectClipRect(dc, g.rcPlist.left + 1, g.rcPlist.top + 1,
            g.rcPlist.right - 1, g.rcPlist.bottom - 1);

        for (int r = 0; r < visRows; ++r) {
            int i = g.scrollTop + r;
            if (i < 0 || i >= g.pl.size()) break;
            int top = g.rcPlist.top + r * rowH;
            RECT row = MakeR(g.rcPlist.left + 1, top, g.rcPlist.right - 1, top + rowH);

            bool isSel = i < (int)g.selected.size() && g.selected[i];
            bool isCur = (i == g.pl.current);
            if (isSel) Fill(dc, row, th.sel);
            if (isCur) {
                RECT bar = MakeR(row.left, row.top, row.left + S(3), row.bottom);
                Fill(dc, bar, th.selBar);
            }

            wchar_t num[16]; swprintf(num, 16, L"%d.", i + 1);
            RECT rn = MakeR(row.left + S(8), row.top, row.left + numW, row.bottom);
            Text(dc, rn, num, th.textFaint, g.fTiny, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

            RECT rt = MakeR(row.left + numW + S(8), row.top,
                row.right - durW - S(12), row.bottom);
            Text(dc, rt, RowLabel(i), isCur ? th.accent : th.text, g.fSmall,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            double d = RowDur(i);
            if (d > 0) {
                RECT rd = MakeR(row.right - durW - S(10), row.top, row.right - S(10), row.bottom);
                Text(dc, rd, FormatTime(d), th.textFaint, g.fTiny,
                    DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            }
        }

        if (g.pl.size() == 0) {
            RECT c = g.rcPlist;
            Text(dc, c, L"Drop files or folders here  \x00b7  or press Ctrl+O",
                th.textFaint, g.fSmall, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }

        RestoreDC(dc, savedDC);

        if (g.rcScroll.right > g.rcScroll.left) {
            Fill(dc, g.rcScroll, th.panelAlt);
            int total = g.pl.size();
            int h = g.rcScroll.bottom - g.rcScroll.top;
            int thumbH = std::max(S(24), h * visRows / std::max(1, total));
            int maxTop = std::max(1, total - visRows);
            int ty = g.rcScroll.top + (h - thumbH) * g.scrollTop / maxTop;
            RECT thumb = MakeR(g.rcScroll.left + S(2), ty, g.rcScroll.right - S(2), ty + thumbH);
            RoundBox(dc, thumb, th.textFaint, th.textFaint, S(4));
        }
    }

    // ---- status -----------------------------------------------------------
    if (RectVisible(dc, &g.rcStatus)) {
        wstr left = g.status;
        if (left.empty()) {
            left = g.player.hasTrack()
                ? (g.player.outputDesc() + L"  \x00b7  " + g.player.deviceName())
                : wstr(L"Ready  \x00b7  fully offline, no network access");
        }

        wchar_t rbuf[200];
        int u = g.player.underruns();
        if (g.cfg.showStats)
            swprintf(rbuf, 200, L"dec %.2f  rs %.2f  mix %.2f  rnd %.2f  \x00b7  %d tracks%s",
                g.st.usDecode / 10000.0, g.st.usResample / 10000.0,
                g.st.usMixEq / 10000.0, g.st.usRender / 10000.0,
                g.pl.size(), u > 0 ? L"  \x00b7  glitches!" : L"");
        else
            swprintf(rbuf, 200, L"%d tracks  \x00b7  %.0f%%%s",
                g.pl.size(), g.cfg.scale * 100.0f, u > 0 ? L"  \x00b7  glitches!" : L"");

        // Reserve the right-hand text's width so the two never overlap.
        int rw = TextW(dc, rbuf, g.fTiny);
        RECT lr = g.rcStatus;
        lr.right = g.rcStatus.right - rw - S(14);
        if (lr.right > lr.left)
            Text(dc, lr, left, th.textFaint, g.fTiny,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        Text(dc, g.rcStatus, rbuf, u > 0 ? th.accent : th.textFaint, g.fTiny,
            DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }
}

// ---------------------------------------------------------------------------
// hit testing
// ---------------------------------------------------------------------------
static int HitTest(POINT p, int* bandOut) {
    *bandOut = -1;
    for (const Btn& b : g.btns) if (In(b.r, p)) return b.id;
    for (const Chip& c : g.chips) if (In(c.r, p)) return c.id;

    RECT seekHot = g.rcSeek; InflateRect(&seekHot, S(4), S(8));
    if (In(seekHot, p)) return H_SEEK;
    RECT volHot = g.rcVol; InflateRect(&volHot, S(4), S(8));
    if (In(volHot, p)) return H_VOL;
    RECT balHot = g.rcBal; InflateRect(&balHot, S(4), S(8));
    if (In(balHot, p)) return H_BAL;
    if (In(g.rcTimeR, p)) return H_TIME;

    if (g.cfg.showEq) {
        RECT r = g.rcEqPre; InflateRect(&r, S(8), S(2));
        if (In(r, p)) { *bandOut = -2; return H_EQPRE; }
        for (int i = 0; i < EQ_BANDS; ++i) {
            RECT b = g.rcEqBand[i]; InflateRect(&b, S(8), S(2));
            if (In(b, p)) { *bandOut = i; return H_EQBAND + i; }
        }
    }
    if (g.rcScroll.right > g.rcScroll.left && In(g.rcScroll, p)) return H_SCROLL;
    if (In(g.rcPlist, p)) return H_PLIST;
    return H_NONE;
}

static float SliderValue(const RECT& r, int mx) {
    int w = r.right - r.left;
    if (w <= 0) return 0.0f;
    return Clampf((float)(mx - r.left) / (float)w, 0.0f, 1.0f);
}

static float BandValue(const RECT& r, int my) {
    int h = r.bottom - r.top;
    if (h <= 0) return 0.0f;
    float t = Clampf((float)(r.bottom - my) / (float)h, 0.0f, 1.0f);
    return t * 24.0f - 12.0f;
}

static int RowAt(POINT p) {
    int rowH = S(21);
    if (!In(g.rcPlist, p) || rowH <= 0) return -1;
    int r = (p.y - g.rcPlist.top) / rowH;
    int i = g.scrollTop + r;
    return (i >= 0 && i < g.pl.size()) ? i : -1;
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------
static void SetScale(float s) {
    g.cfg.scale = Clampf(s, 0.75f, 3.0f);
    g.scaleF = g.cfg.scale * (float)g.dpi / 96.0f;
    MakeFonts();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void ToggleTheme() {
    g.cfg.dark = !g.cfg.dark;
    SetDarkTitleBar(g.hwnd, g.cfg.dark);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void HandleClick(int id, POINT p, bool dbl, bool ctrl, bool shift) {
    switch (id) {
    case H_PREV: PlayPrev(); break;
    case H_NEXT: PlayNext(true); break;
    case H_RW:   g.player.nudge(-5.0); break;
    case H_FF:   g.player.nudge(5.0); break;
    case H_STOP: g.player.stop(); break;
    case H_PLAY:
        if (!g.player.hasTrack()) {
            int start = g.pl.current >= 0 ? g.pl.current : 0;
            if (g.pl.size() > 0) PlayIndex(start);
        } else g.player.togglePause();
        break;
    case H_TIME: g.cfg.elapsed = !g.cfg.elapsed; break;
    case H_OPEN:   DoOpenFiles(ctrl); break;
    case H_FOLDER: DoOpenFolder(ctrl); break;
    case H_SAVE:   DoSavePlaylist(); break;
    case H_CLEAR:  g.player.stop(); g.pl.clear(); g.selected.clear(); g.scrollTop = 0;
        UpdateTitle(); TouchPlaylist(); break;
    case H_SHUFFLE: g.pl.shuffle = !g.pl.shuffle; g.cfg.shuffle = g.pl.shuffle; if (g.pl.shuffle) g.pl.reshuffle(); break;
    case H_REPEAT:  g.pl.repeat = (g.pl.repeat + 1) % 3; g.cfg.repeat = g.pl.repeat; break;
    case H_EQ:      g.cfg.showEq = !g.cfg.showEq; break;
    case H_VIS:     g.cfg.showVis = !g.cfg.showVis; break;
    case H_THEME:   ToggleTheme(); break;
    case H_DEVICE: {
        POINT sp = p;                                  // client coords
        for (const Chip& c : g.chips)
            if (c.id == H_DEVICE) { sp.x = c.r.left; sp.y = c.r.bottom + 2; break; }
        ClientToScreen(g.hwnd, &sp);                   // convert exactly once
        ShowDeviceMenu(sp);
        break;
    }
    case H_EXCL:
        g.cfg.exclusive = !g.cfg.exclusive;
        g.player.setExclusive(g.cfg.exclusive);
        if (g.cfg.exclusive && g.player.hasTrack() && !g.player.exclusiveActive())
            g.status = L"Device refused exclusive mode - staying on shared";
        else g.status.clear();
        break;
    case H_PLIST: {
        int i = RowAt(p);
        if (i < 0) break;
        if (dbl) { PlayIndex(i); break; }
        if (g.selected.size() != g.pl.items.size()) g.selected.assign(g.pl.items.size(), 0);
        if (shift && g.anchor >= 0) {
            int a = std::min(g.anchor, i), b = std::max(g.anchor, i);
            std::fill(g.selected.begin(), g.selected.end(), 0);
            for (int k = a; k <= b; ++k) g.selected[k] = 1;
        } else if (ctrl) {
            g.selected[i] = !g.selected[i];
            g.anchor = i;
        } else {
            std::fill(g.selected.begin(), g.selected.end(), 0);
            g.selected[i] = 1;
            g.anchor = i;
        }
        break;
    }
    default: break;
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// window proc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        g.hwnd = hwnd;
        DragAcceptFiles(hwnd, TRUE);
        SetDarkTitleBar(hwnd, g.cfg.dark);
        SetTimer(hwnd, 1, 33, nullptr);
        return 0;

    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        SetScale(g.cfg.scale);
        RECT* pr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, pr->left, pr->top, pr->right - pr->left, pr->bottom - pr->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SIZE: {
        g.cw = LOWORD(lp);
        g.chh = HIWORD(lp);
        if (g.memDC) {
            DeleteObject(g.memBmp); DeleteDC(g.memDC);
            g.memDC = nullptr; g.memBmp = nullptr;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        RECT rc{ 0, 0, (int)(400 * g.scaleF), (int)(430 * g.scaleF) };
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)(g.dpi > 0 ? g.dpi : 96));
        mm->ptMinTrackSize.x = rc.right - rc.left;
        mm->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }

    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (!g.memDC || g.memW != g.cw || g.memH != g.chh) {
            if (g.memDC) { DeleteObject(g.memBmp); DeleteDC(g.memDC); }
            g.memDC = CreateCompatibleDC(dc);
            g.memBmp = CreateCompatibleBitmap(dc, std::max(1, g.cw), std::max(1, g.chh));
            SelectObject(g.memDC, g.memBmp);
            g.memW = g.cw; g.memH = g.chh;
        }
        // Draw only the invalid region: a 30 fps tick repaints the analyser and
        // the clock, never the playlist.
        int sv = SaveDC(g.memDC);
        IntersectClipRect(g.memDC, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
        PaintAll(g.memDC);
        RestoreDC(g.memDC, sv);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
            ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
            g.memDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER: {
        ULONGLONG nowTick = GetTickCount64();
        if (nowTick - g.lastStatTick >= 1000) {
            g.lastStatTick = nowTick;
            g.st = g.player.takeStats();
        }

        // Autosave: shortly after the playlist changes, and every 15 s while
        // playing so the resume point survives a kill as well as a clean exit.
        if (g.plDirty && nowTick - g.plDirtyAt >= 1500) {
            SaveSession();
            g.cfg.save();
            g.lastSessionSave = nowTick;
        } else if (g.player.state() == PlayState::Playing && nowTick - g.lastSessionSave >= 15000) {
            g.lastSessionSave = nowTick;
            g.cfg.resumeIndex = g.pl.current;
            g.cfg.resumePos = g.player.position();
            g.cfg.save();
        }
        bool animating = false;
        if (g.cfg.showVis && g.player.state() == PlayState::Playing) {
            g.player.getVis(g.visSamples, VIS_FFT);
            g.spec.compute(g.visSamples, g.bars);
            animating = true;
        } else {
            float peak = 0.0f;
            for (int i = 0; i < VIS_BARS; ++i) {
                g.bars[i] *= 0.85f;
                if (g.bars[i] > peak) peak = g.bars[i];
            }
            animating = peak > 0.004f;
        }

        // 30 fps only while something moves; 4 fps when idle.
        int want = animating ? 33 : 250;
        if (want != g.timerMs) { g.timerMs = want; SetTimer(hwnd, 1, want, nullptr); }

        if (g.cfg.showVis && g.rcVis.right > g.rcVis.left)
            InvalidateRect(hwnd, &g.rcVis, FALSE);
        if (g.rcTimeR.right > g.rcTimeL.left) {
            RECT band = { g.rcTimeL.left, g.rcTimeL.top - S(2),
                          g.rcTimeR.right, g.rcTimeL.bottom + S(2) };
            InvalidateRect(hwnd, &band, FALSE);
        }
        if (g.rcStatus.right > g.rcStatus.left)
            InvalidateRect(hwnd, &g.rcStatus, FALSE);
        return 0;
    }

    case WM_APP_TRACK_END:
        PlayNext(false);
        return 0;

    case WM_APP_DEV_LOST: {
        // Debounce: one rebuild per burst of notifications.
        ULONGLONG now = GetTickCount64();
        if (now - g.lastDevReset < 700) return 0;
        g.lastDevReset = now;
        g.status = L"Output device was reconfigured \x2014 reconnecting";
        ReopenAudio();
        return 0;
    }

    case WM_APP_DEV_CHANGED: {
        // Only react when we follow the default and it actually moved, or when
        // the endpoint we are pinned to came back.
        ULONGLONG now = GetTickCount64();
        if (now - g.lastDevReset < 700) return 0;
        bool needed = false;
        if (g.player.followsDefault()) {
            std::vector<AudioDevice> devs = g.player.listDevices();
            for (const auto& d : devs)
                if (d.isDefault && d.id != g.player.activeDeviceId()) { needed = true; break; }
        } else if (g.player.activeDeviceId() != g.cfg.deviceId) {
            needed = true;
        }
        if (needed) {
            g.lastDevReset = now;
            g.status = L"Default output changed \x2014 switching";
            ReopenAudio();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP_META_READY:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM_(lp), GET_Y_LPARAM_(lp) };
        if (g.drag == D_SEEK) {
            double dur = g.player.duration();
            g.seekPreview = dur * SliderValue(g.rcSeek, p.x);
        } else if (g.drag == D_VOL) {
            g.cfg.volume = SliderValue(g.rcVol, p.x);
            g.player.setVolume(g.cfg.volume);
        } else if (g.drag == D_BAL) {
            g.cfg.balance = SliderValue(g.rcBal, p.x) * 2.0f - 1.0f;
            if (fabsf(g.cfg.balance) < 0.06f) g.cfg.balance = 0.0f;
            g.player.setBalance(g.cfg.balance);
        } else if (g.drag == D_EQ) {
            float db = BandValue(g.dragBand < 0 ? g.rcEqPre : g.rcEqBand[g.dragBand], p.y);
            if (fabsf(db) < 0.7f) db = 0.0f;
            if (g.dragBand < 0) { g.cfg.preamp = db; g.player.eq().setPreamp(db); }
            else { g.cfg.eqGain[g.dragBand] = db; g.player.eq().setGain(g.dragBand, db); }
            if (!g.cfg.eqOn) { g.cfg.eqOn = true; g.player.eq().setEnabled(true); }
        } else if (g.drag == D_SCROLL) {
            int rowH = S(21);
            int visRows = (g.rcPlist.bottom - g.rcPlist.top) / std::max(1, rowH);
            int h = g.rcScroll.bottom - g.rcScroll.top;
            int maxTop = std::max(0, g.pl.size() - visRows);
            int thumbH = std::max(S(24), h * visRows / std::max(1, g.pl.size()));
            int travel = std::max(1, h - thumbH);
            g.scrollTop = Clampi((p.y - g.rcScroll.top - g.scrollGrabDy) * maxTop / travel, 0, maxTop);
        } else {
            int band;
            int id = HitTest(p, &band);
            if (id != g.hot) { g.hot = id; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT p{ GET_X_LPARAM_(lp), GET_Y_LPARAM_(lp) };
        SetFocus(hwnd);
        int band;
        int id = HitTest(p, &band);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (id == H_SEEK) {
            g.drag = D_SEEK;
            g.seekPreview = g.player.duration() * SliderValue(g.rcSeek, p.x);
            SetCapture(hwnd);
        } else if (id == H_VOL) {
            g.drag = D_VOL; SetCapture(hwnd);
            g.cfg.volume = SliderValue(g.rcVol, p.x);
            g.player.setVolume(g.cfg.volume);
        } else if (id == H_BAL) {
            g.drag = D_BAL; SetCapture(hwnd);
            g.cfg.balance = SliderValue(g.rcBal, p.x) * 2.0f - 1.0f;
            g.player.setBalance(g.cfg.balance);
        } else if (id == H_EQPRE || (id >= H_EQBAND && id < H_EQBAND + EQ_BANDS)) {
            g.drag = D_EQ;
            g.dragBand = (id == H_EQPRE) ? -1 : (id - H_EQBAND);
            SetCapture(hwnd);
            SendMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
        } else if (id == H_SCROLL) {
            int rowH = S(21);
            int visRows = (g.rcPlist.bottom - g.rcPlist.top) / std::max(1, rowH);
            int h = g.rcScroll.bottom - g.rcScroll.top;
            int thumbH = std::max(S(24), h * visRows / std::max(1, g.pl.size()));
            int maxTop = std::max(1, g.pl.size() - visRows);
            int ty = g.rcScroll.top + (h - thumbH) * g.scrollTop / maxTop;
            g.scrollGrabDy = (p.y >= ty && p.y < ty + thumbH) ? (p.y - ty) : thumbH / 2;
            g.drag = D_SCROLL;
            SetCapture(hwnd);
            SendMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
        } else {
            HandleClick(id, p, msg == WM_LBUTTONDBLCLK, ctrl, shift);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g.drag == D_SEEK && g.seekPreview >= 0) {
            g.player.seek(g.seekPreview);
            g.seekPreview = -1.0;
        }
        if (g.drag != D_NONE) { ReleaseCapture(); g.drag = D_NONE; g.dragBand = -1; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_RBUTTONDOWN: {
        POINT p{ GET_X_LPARAM_(lp), GET_Y_LPARAM_(lp) };
        int band;
        int id = HitTest(p, &band);
        if (id == H_EQPRE) { g.cfg.preamp = 0; g.player.eq().setPreamp(0); }
        else if (id >= H_EQBAND && id < H_EQBAND + EQ_BANDS) {
            int b = id - H_EQBAND;
            g.cfg.eqGain[b] = 0; g.player.eq().setGain(b, 0);
        } else if (id == H_EQ) {
            for (int i = 0; i < EQ_BANDS; ++i) { g.cfg.eqGain[i] = 0; g.player.eq().setGain(i, 0); }
            g.cfg.preamp = 0; g.player.eq().setPreamp(0);
        } else if (id == H_VOL) { g.cfg.volume = 0.7f; g.player.setVolume(0.7f); }
        else if (id == H_BAL) { g.cfg.balance = 0; g.player.setBalance(0); }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            SetScale(g.cfg.scale + (delta > 0 ? 0.1f : -0.1f));
        } else {
            POINT p{ GET_X_LPARAM_(lp), GET_Y_LPARAM_(lp) };
            ScreenToClient(hwnd, &p);
            if (In(g.rcPlist, p)) {
                g.scrollTop -= (delta / WHEEL_DELTA) * 3;
                int rowH = S(21);
                int visRows = (g.rcPlist.bottom - g.rcPlist.top) / std::max(1, rowH);
                g.scrollTop = Clampi(g.scrollTop, 0, std::max(0, g.pl.size() - visRows));
            } else {
                g.cfg.volume = Clampf(g.cfg.volume + (delta > 0 ? 0.03f : -0.03f), 0.0f, 1.0f);
                g.player.setVolume(g.cfg.volume);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<wstr> paths;
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH * 2];
            if (DragQueryFileW(hd, i, buf, MAX_PATH * 2)) paths.push_back(buf);
        }
        DragFinish(hd);
        bool wasEmpty = g.pl.size() == 0;
        AddPaths(paths, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        if (wasEmpty && g.pl.size() > 0) PlayIndex(0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (wp) {
        case VK_SPACE:
            if (!g.player.hasTrack() && g.pl.size() > 0) PlayIndex(std::max(0, g.pl.current));
            else g.player.togglePause();
            break;
        case VK_LEFT:  g.player.nudge(ctrl ? -30.0 : -5.0); break;
        case VK_RIGHT: g.player.nudge(ctrl ? 30.0 : 5.0); break;
        case VK_UP:
            g.cfg.volume = Clampf(g.cfg.volume + 0.05f, 0, 1); g.player.setVolume(g.cfg.volume); break;
        case VK_DOWN:
            g.cfg.volume = Clampf(g.cfg.volume - 0.05f, 0, 1); g.player.setVolume(g.cfg.volume); break;
        case VK_RETURN: {
            for (int i = 0; i < (int)g.selected.size(); ++i)
                if (g.selected[i]) { PlayIndex(i); break; }
            break;
        }
        case VK_DELETE: {
            std::vector<int> del;
            for (int i = 0; i < (int)g.selected.size(); ++i) if (g.selected[i]) del.push_back(i);
            g.pl.removeSelection(del);
            g.selected.assign(g.pl.items.size(), 0);
            TouchPlaylist();
            break;
        }
        case 'B': PlayNext(true); break;
        case 'Z': PlayPrev(); break;
        case 'V': g.player.stop(); break;
        case 'X': if (g.pl.size() > 0) PlayIndex(std::max(0, g.pl.current)); break;
        case 'C': g.player.togglePause(); break;
        case 'S': if (!ctrl) { g.pl.shuffle = !g.pl.shuffle; g.cfg.shuffle = g.pl.shuffle; } break;
        case 'R': if (!ctrl) { g.pl.repeat = (g.pl.repeat + 1) % 3; g.cfg.repeat = g.pl.repeat; } break;
        case 'T': if (!ctrl) ToggleTheme(); break;
        case 'O': if (ctrl) DoOpenFiles(false); break;
        case 'L': if (ctrl) DoOpenFolder(false); break;
        case 'A': if (ctrl) { g.selected.assign(g.pl.items.size(), 1); } break;
        case VK_OEM_PLUS: case VK_ADD:      if (ctrl) SetScale(g.cfg.scale + 0.1f); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: if (ctrl) SetScale(g.cfg.scale - 0.1f); break;
        case '0': if (ctrl) SetScale(1.0f); break;
        case VK_ESCAPE: break;
        default: break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_HOTKEY:
        switch (wp) {
        case 101: g.player.togglePause(); break;
        case 102: g.player.stop(); break;
        case 103: PlayNext(true); break;
        case 104: PlayPrev(); break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CLOSE: {
        WINDOWPLACEMENT wpl{ sizeof(wpl) };
        GetWindowPlacement(hwnd, &wpl);
        g.cfg.maximized = (wpl.showCmd == SW_SHOWMAXIMIZED);
        g.cfg.winX = wpl.rcNormalPosition.left;
        g.cfg.winY = wpl.rcNormalPosition.top;
        g.cfg.winW = wpl.rcNormalPosition.right - wpl.rcNormalPosition.left;
        g.cfg.winH = wpl.rcNormalPosition.bottom - wpl.rcNormalPosition.top;
        SaveSession();
        g.cfg.save();
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    // MTA: Media Foundation, WASAPI and the property system all live in one
    // apartment so no interface has to be marshalled between threads.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    g.inst = inst;
    g.cfg.load();
    g.scaleF = g.cfg.scale;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PlainAmpWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);

    // Create at an arbitrary size first: passing CW_USEDEFAULT for x makes
    // Windows ignore nWidth/nHeight, so the real size is applied below once
    // the window's monitor DPI is known.
    HWND hwnd = CreateWindowExW(0, L"PlainAmpWnd", L"PlainAmp",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    g.hwnd = hwnd;

    g.dpi = (int)GetDpiForWindow(hwnd);
    if (g.dpi <= 0) g.dpi = 96;
    SetScale(g.cfg.scale);

    {
        int wpx, hpx, xp, yp;
        bool restored = (g.cfg.winW > 200 && g.cfg.winH > 200);
        if (restored) {
            wpx = g.cfg.winW; hpx = g.cfg.winH; xp = g.cfg.winX; yp = g.cfg.winY;
            RECT saved{ xp, yp, xp + wpx, yp + hpx };
            if (!MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) restored = false;  // monitor gone
        }
        if (!restored) {
            RECT rc{ 0, 0, (int)(540 * g.scaleF), (int)(680 * g.scaleF) };
            AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)g.dpi);
            wpx = rc.right - rc.left; hpx = rc.bottom - rc.top;
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
            if (hpx > ah) hpx = ah;
            xp = mi.rcWork.left + (aw - wpx) / 2;
            yp = mi.rcWork.top + (ah - hpx) / 2;
        }
        SetWindowPos(hwnd, nullptr, xp, yp, wpx, hpx, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!g.player.init()) {
        MessageBoxW(hwnd, L"No audio output device could be opened.", L"PlainAmp", MB_ICONERROR);
    }
    g.player.notifyHwnd = hwnd;
    g.player.setDeviceId(g.cfg.deviceId);
    g.player.watchDevices(hwnd);
    g.player.setExclusive(g.cfg.exclusive);
    g.player.setVolume(g.cfg.volume);
    g.player.setBalance(g.cfg.balance);
    ApplyEqFromCfg();

    g.meta.start(hwnd);
    g.pl.shuffle = g.cfg.shuffle;
    g.pl.repeat = g.cfg.repeat;

    if (g.cfg.mediaKeys) {
        RegisterHotKey(hwnd, 101, 0, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(hwnd, 102, 0, VK_MEDIA_STOP);
        RegisterHotKey(hwnd, 103, 0, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(hwnd, 104, 0, VK_MEDIA_PREV_TRACK);
    }

    // Always restore last session's playlist first; files named on the command
    // line are appended to it rather than replacing it.
    g.pl.loadM3U(Config::playlistPath());
    int restored = g.pl.size();

    std::vector<wstr> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (argv) {
        for (int i = 0; i < argc; ++i) if (argv[i][0]) args.push_back(argv[i]);
        LocalFree(argv);
    }
    g.plDirty = false;                     // restoring is not a change ...
    if (!args.empty()) AddPaths(args, false);   // ... but appending is, so autosave picks it up
    g.selected.assign(g.pl.items.size(), 0);

    ShowWindow(hwnd, g.cfg.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd);

    if (!args.empty() && g.pl.size() > restored) {
        PlayIndex(restored);               // play the first file that was passed in
    } else if (g.cfg.resumeIndex >= 0 && g.cfg.resumeIndex < g.pl.size()) {
        // Cue up where we left off, but don't start playing on its own.
        CueIndex(g.cfg.resumeIndex, g.cfg.resumePos);
        g.status = L"Resumed playlist \x2014 press Play";
    }
    g.lastSessionSave = GetTickCount64();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.meta.stop();
    g.player.shutdown();
    if (g.memDC) { DeleteObject(g.memBmp); DeleteDC(g.memDC); }
    CoUninitialize();
    return 0;
}
