// PingPeek: your real ping to the Fortnite server you are playing on, as a small click-through overlay.
//
// Every Fortnite match server answers UDP pings on a separate port, and Fortnite logs that address
// each time you join a match:
//   LogNet: ServerSetPingAddress: EPingType::UDPQoS PingAddress: 203.0.113.10, PingPort: 22222
// The responder echoes the first 22 bytes of any packet at least that long. Fortnite's own ping display
// measures the same thing (net.PlayerFacingPingType=8 is EPingType::UDPQoS) as a moving average sampled
// every 2 s; PingPeek shows every raw sample, once a second. Nothing here touches the game process: it
// reads the game's log file and, while you are in a match, sends one 22-byte UDP packet a second.
//
// Usage: PingPeek.exe        start it; close it from the tray icon or by running it again
//        PingPeek.exe X Y    start it X,Y pixels from the top-left of the game's screen,
//                            or move the copy that is already running there

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <shellapi.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define WM_STATUS (WM_APP + 1)   // worker -> window: wParam = state | ms << 8, lParam = data center label
#define WM_TOGGLE (WM_APP + 2)   // a second launch without a position: close
#define WM_MOVETO (WM_APP + 3)   // a second launch with a position: wParam = x, lParam = y
#define WM_TRAY   (WM_APP + 4)   // tray icon mouse events
#define ID_EXIT   1              // tray menu item

#define PING_EVERY_MS    1000
#define PING_TIMEOUT_MS  1000
#define NO_ADDR_AFTER_MS 15000   // this long in a match without a ping address: say so instead of waiting
#define IGNORE_CLOSE_MS  2000    // a close request this soon after starting is a double launch, not a close
#define PKT_LEN          22      // the responder echoes exactly this many bytes and ignores shorter packets
#define FONT_PX          18      // overlay text height at 100% display scaling

enum { ST_NO_LOG, ST_IDLE, ST_WAITING, ST_NO_ADDR, ST_PING, ST_LOST, ST_ERROR };

static const wchar_t APP_NAME[] = L"PingPeek";
static const wchar_t USAGE[] =
    L"PingPeek shows your ping to the Fortnite server you're playing on.\n\n"
    L"Start it:   PingPeek.exe\n"
    L"Close it:   tray icon > Exit, or run PingPeek.exe again\n"
    L"Move it:   PingPeek.exe X Y\n"
    L"(X and Y are pixels from the top-left corner of the game's screen)\n\n"
    L"Example: PingPeek.exe 12 12";
static const char ADDR_MARK[] = "LogNet: ServerSetPingAddress: EPingType::UDPQoS PingAddress: ";

static HWND           g_wnd;
static HFONT          g_font;
static float          g_scale = 1.0f;   // monitor DPI / 96
static int            g_x = 12, g_y = 12;
static int            g_state = ST_IDLE, g_ms;
static char           g_dc[9];
static const wchar_t *g_flash;          // short notice shown whether or not the game is in front
static BOOL           g_exiting;
static ULONGLONG      g_started;
static NOTIFYICONDATAW g_tray;
static HWND           g_tray_wnd;          // hidden window that owns the tray icon and its menu
static UINT           g_taskbar_created;   // broadcast when Explorer restarts and the tray is rebuilt

// ---- Fortnite log -------------------------------------------------------------------------------

typedef struct {
    wchar_t   path[MAX_PATH];
    BOOL      found;
    DWORD     vol, idhi, idlo;   // identity of the file being read; changes when Fortnite starts a new log
    ULONGLONG off;
    char      line[4096];        // longer lines are cut; every marker we need sits near the start
    size_t    len;
    char      next_dc[8];        // data center of the latest match assignment, used by the next join
    char      dc[8];             // data center of the current match, "" if its join had no assignment
    BOOL      in_match;
    ULONGLONG joined;            // GetTickCount64() when the current join was read
    ULONG     addr;              // ping address of the current match, network byte order; 0 = not known yet
    USHORT    port;
} Log;

// Unreal lines look like "[2026.10.05-14.18.46:902][240]LogNet: Browse: ...". Returns the text after the
// "[time][frame]" prefix, so markers only match where Unreal writes the log category, never mid-message.
static const char *body(const char *s) {
    const char *p;
    if (s[0] == '[' && (p = strstr(s, "][")) && (p = strchr(p + 2, ']'))) return p + 1;
    return s;
}

static void parse_dc(const char *p, char *out) {
    int n = 0;
    while (n < 7 && ((p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= 'a' && p[n] <= 'z'))) {
        out[n] = p[n];
        n++;
    }
    out[n] = 0;
}

// Parses "203.0.113.10, PingPort: 22222". Rejects anything that is not a plain public-looking IPv4
// address and a valid port, so a damaged or edited log can't aim the pings at this machine or a broadcast.
static BOOL parse_ping_address(const char *a, ULONG *addr, USHORT *port) {
    const char *comma = strchr(a, ','), *pp = strstr(a, "PingPort: ");
    char ip[16], *end;
    IN_ADDR in;
    if (!comma || comma - a >= (ptrdiff_t)sizeof ip || !pp) return FALSE;
    memcpy(ip, a, (size_t)(comma - a));
    ip[comma - a] = 0;
    long p = strtol(pp + 10, &end, 10);
    if (end == pp + 10 || p < 1 || p > 65535 || inet_pton(AF_INET, ip, &in) != 1) return FALSE;
    unsigned char first = (unsigned char)(in.s_addr & 0xFF);   // s_addr is in network byte order
    if (first == 0 || first == 127 || first >= 224) return FALSE;
    *addr = in.s_addr;
    *port = (USHORT)p;
    return TRUE;
}

static void on_line(Log *L) {
    const char *b = body(L->line), *p;
    if (!strncmp(b, "LogMatchmaking: ", 16)) {
        if (strstr(b, "[client_match_assigned]") && (p = strstr(b, "SubRegion:"))) parse_dc(p + 10, L->next_dc);
    } else if (!strncmp(b, "LogNet: Browse: ", 16)) {
        // "Browse: 1.2.3.4:9010/..." joins a match server; "Browse: /Game/Maps/Frontend" is the local lobby.
        L->in_match = b[16] != '/';
        L->addr = 0;
        L->port = 0;
        if (L->in_match) {
            memcpy(L->dc, L->next_dc, sizeof L->dc);
            L->next_dc[0] = 0;
            L->joined = GetTickCount64();
        } else {
            L->dc[0] = 0;
        }
    } else if (!strncmp(b, ADDR_MARK, sizeof ADDR_MARK - 1)) {
        if (L->in_match) parse_ping_address(b + sizeof ADDR_MARK - 1, &L->addr, &L->port);
    } else if (!strncmp(b, "LogExit: Exiting", 16)) {
        L->in_match = FALSE;
        L->addr = 0;
        L->port = 0;
        L->dc[0] = 0;
    }
}

// Reads whatever Fortnite appended since last time. Opens and closes the file on every call, with full
// sharing, so it never gets in the way of Fortnite writing or rotating its log.
static void tail(Log *L) {
    static char buf[1 << 16];
    HANDLE f = CreateFileW(L->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME) L->found = FALSE;
        return;   // anything else (a sharing hiccup) keeps the last known state
    }
    L->found = TRUE;
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(f, &fi)) {
        ULONGLONG size = (ULONGLONG)fi.nFileSizeHigh << 32 | fi.nFileSizeLow;
        if (fi.dwVolumeSerialNumber != L->vol || fi.nFileIndexHigh != L->idhi || fi.nFileIndexLow != L->idlo ||
            size < L->off) {
            // A new game session started a fresh log.
            L->vol = fi.dwVolumeSerialNumber;
            L->idhi = fi.nFileIndexHigh;
            L->idlo = fi.nFileIndexLow;
            L->off = 0;
            L->len = 0;
            L->next_dc[0] = 0;
            L->dc[0] = 0;
            L->in_match = FALSE;
            L->addr = 0;
            L->port = 0;
        }
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)L->off;
        DWORD got;
        if (size > L->off && SetFilePointerEx(f, pos, NULL, FILE_BEGIN)) {
            while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) {
                L->off += got;
                char *p = buf, *end = buf + got;
                while (p < end) {
                    char *nl = memchr(p, '\n', (size_t)(end - p));
                    size_t n = (size_t)((nl ? nl : end) - p), room = sizeof L->line - 1 - L->len;
                    if (n > room) n = room;
                    memcpy(L->line + L->len, p, n);
                    L->len += n;
                    if (!nl) break;   // partial line: finish it on the next call
                    L->line[L->len] = 0;
                    on_line(L);
                    L->len = 0;
                    p = nl + 1;
                }
            }
        }
    }
    CloseHandle(f);
}

// ---- Game window --------------------------------------------------------------------------------

static BOOL is_fortnite(HWND h) {
    wchar_t cls[16], title[16];
    return h && GetClassNameW(h, cls, 16) && !lstrcmpW(cls, L"UnrealWindow") && GetWindowTextW(h, title, 16) &&
           !wcsncmp(title, L"Fortnite", 8);
}

static HWND find_fortnite(void) {
    HWND h = NULL;
    while ((h = FindWindowExW(NULL, h, L"UnrealWindow", NULL)))
        if (is_fortnite(h)) return h;
    return NULL;
}

// ---- Ping ---------------------------------------------------------------------------------------

// One UDP ping. Returns the round trip in ms, or -1 if no matching echo arrived within the timeout.
static int udp_ping(SOCKET s, ULONG addr, USHORT port, USHORT seq) {
    unsigned char out[PKT_LEN] = {0}, in[64];
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    // Our id, sequence number and send time. The echo has to match all of it, so a late echo of an
    // earlier ping can never be counted as this one.
    USHORT id = (USHORT)GetCurrentProcessId();
    memcpy(out, &id, 2);
    memcpy(out + 2, &seq, 2);
    memcpy(out + 6, &t0.QuadPart, 8);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = addr;
    if (sendto(s, (const char *)out, PKT_LEN, 0, (struct sockaddr *)&to, sizeof to) != PKT_LEN) return -1;
    for (;;) {
        QueryPerformanceCounter(&t1);
        LONGLONG left_us = PING_TIMEOUT_MS * 1000LL - (t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart;
        if (left_us <= 0) return -1;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {(long)(left_us / 1000000), (long)(left_us % 1000000)};
        if (select(0, &rd, NULL, NULL, &tv) <= 0) return -1;
        struct sockaddr_in from;
        int fromlen = sizeof from;
        int n = recvfrom(s, (char *)in, sizeof in, 0, (struct sockaddr *)&from, &fromlen);
        QueryPerformanceCounter(&t1);
        if (n == PKT_LEN && from.sin_addr.s_addr == addr && from.sin_port == to.sin_port && !memcmp(in, out, PKT_LEN))
            return (int)(((t1.QuadPart - t0.QuadPart) * 1000 + freq.QuadPart / 2) / freq.QuadPart);
        // Anything else - a late echo of an earlier ping, a stray packet, a receive error - is skipped.
    }
}

static SOCKET open_ping_socket(void) {
    WSADATA wsa;
    SOCKET s = INVALID_SOCKET;
    if (!WSAStartup(MAKEWORD(2, 2), &wsa)) s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        // Without this, an ICMP "port unreachable" makes later receives fail instead of simply timing out.
        BOOL off = FALSE;
        DWORD ret;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    }
    return s;
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    static Log L;
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n && n < MAX_PATH - 50) {   // otherwise the path stays empty and the overlay says "no Fortnite log"
        lstrcpyW(L.path, base);
        lstrcatW(L.path, L"\\FortniteGame\\Saved\\Logs\\FortniteGame.log");
    }
    SOCKET s = open_ping_socket();
    USHORT seq = 0;
    for (;;) {
        ULONGLONG start = GetTickCount64();
        tail(&L);
        int state, ms = 0;
        if (!L.found) state = ST_NO_LOG;
        else if (!L.in_match || !find_fortnite()) state = ST_IDLE;   // also a game that closed mid-match
        else if (!L.port) state = GetTickCount64() - L.joined > NO_ADDR_AFTER_MS ? ST_NO_ADDR : ST_WAITING;
        else if (s == INVALID_SOCKET) state = ST_ERROR;
        else {
            // Raised only for the measurement, so the echo is timestamped the moment it lands even while
            // the game keeps every core busy. The thread sleeps the rest of the time.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            ms = udp_ping(s, L.addr, L.port, ++seq);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
            state = ms < 0 ? ST_LOST : ST_PING;
            if (ms < 0) ms = 0;
        }
        LPARAM dc = 0;
        memcpy(&dc, L.dc, sizeof dc < sizeof L.dc ? sizeof dc : sizeof L.dc);
        PostMessageW(g_wnd, WM_STATUS, (WPARAM)(state | ms << 8), dc);
        ULONGLONG spent = GetTickCount64() - start;
        if (spent < PING_EVERY_MS) Sleep((DWORD)(PING_EVERY_MS - spent));
    }
}

// ---- Overlay ------------------------------------------------------------------------------------
// One line of white Arial Bold, centered in a rounded, translucent dark box. The box is the same height
// in every state, and while it shows a ping it is sized for three digits, so it keeps one width as the
// number moves between 9, 99 and 199.

static const COLORREF C_BG = RGB(16, 18, 22), C_TEXT = RGB(255, 255, 255);
static const float BG_ALPHA = 0.80f;

static int scaled(int px) { return (int)(px * g_scale + 0.5f); }

// Where Fortnite's data center codes (the log's SubRegion) are. Unknown codes are shown as they are.
static const char *const PLACES[][2] = {
    {"VA", "Virginia"},    {"OH", "Ohio"},          {"NCAL", "California"}, {"OR", "Oregon"},
    {"TX", "Texas"},       {"IA", "Iowa"},          {"MX", "Mexico"},       {"DE", "Frankfurt"},
    {"GB", "London"},      {"FR", "Paris"},         {"SYD", "Sydney"},      {"SAO", "Sao Paulo"},
    {"TOK", "Tokyo"},      {"BAH", "Bahrain"},      {"MUM", "Mumbai"},      {"IL", "Israel"},
    {"ZA", "South Africa"}, {"SG", "Singapore"},    {"MY", "Malaysia"},     {"TW", "Taiwan"},
    {"SE", "Sweden"},      {"IT", "Italy"},         {"CL", "Chile"},        {"BE", "Belgium"},
};

static const char *place_name(const char *code) {
    for (size_t i = 0; i < sizeof PLACES / sizeof *PLACES; i++)
        if (!lstrcmpiA(PLACES[i][0], code)) return PLACES[i][1];
    return code;
}

// The text to show, and in `widest` the text the box is sized for. Reads "28 ms · Frankfurt".
static void status_text(wchar_t *text, wchar_t *widest) {
    wchar_t at[32] = L"";
    if (g_dc[0]) wsprintfW(at, L" \x00B7 %S", place_name(g_dc));
    if (g_flash) {
        lstrcpyW(text, g_flash);
    } else {
        switch (g_state) {
        case ST_PING:    wsprintfW(text, L"%d ms%s", g_ms, at); break;
        case ST_WAITING: wsprintfW(text, L"connecting%s", at); break;
        case ST_LOST:    wsprintfW(text, L"lost%s", at); break;
        case ST_NO_ADDR: lstrcpyW(text, L"no ping data"); break;
        case ST_NO_LOG:  lstrcpyW(text, L"no Fortnite log"); break;
        case ST_IDLE:    lstrcpyW(text, L"lobby"); break;
        default:         lstrcpyW(text, L"error"); break;
        }
    }
    if (!g_flash && g_state == ST_PING) wsprintfW(widest, L"000 ms%s", at);
    else lstrcpyW(widest, text);
}

// ---- Tray icon ----------------------------------------------------------------------------------
// Shows that PingPeek is running. Hovering shows the current status; clicking opens a menu with Exit.
// It belongs to its own hidden window: the overlay window can never take focus, and a menu whose owner
// can't take focus doesn't close when you click elsewhere.

static void tray_update(DWORD action) {
    g_tray.cbSize = sizeof g_tray;
    g_tray.hWnd = g_tray_wnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tray.uCallbackMessage = WM_TRAY;
    if (!g_tray.hIcon)
        g_tray.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_tray.szTip[0]) lstrcpyW(g_tray.szTip, APP_NAME);
    Shell_NotifyIconW(action, &g_tray);
}

static void tray_tip(const wchar_t *status) {
    wchar_t tip[128];
    wsprintfW(tip, L"%s: %s", APP_NAME, status);
    if (lstrcmpW(tip, g_tray.szTip)) {
        lstrcpynW(g_tray.szTip, tip, 128);
        if (g_tray.hWnd) tray_update(NIM_MODIFY);   // not before the icon exists
    }
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Anti-aliased coverage of a w x h rounded rectangle with corner radius rad, at pixel center (x, y).
static float rounded_rect_cover(float x, float y, float w, float h, float rad) {
    float qx = fabsf(x - w / 2) - (w / 2 - rad), qy = fabsf(y - h / 2) - (h / 2 - rad);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return clamp01(0.5f - (sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rad));
}

static int clampi(int v, int lo, int hi) { return v > hi ? (hi > lo ? hi : lo) : v < lo ? lo : v; }

// Positions are relative to the monitor the game is on (the primary one if it isn't running), and the
// box is kept fully on that monitor whatever X,Y says.
static RECT game_monitor(void) {
    HWND fn = find_fortnite();
    POINT origin = {0, 0};
    HMONITOR m = fn ? MonitorFromWindow(fn, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {0};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(m, &mi);
    return mi.rcMonitor;
}

// Draws the text white-on-black with GDI to get its antialiased coverage, then composites box and text
// into premultiplied pixels for a per-pixel-alpha layered window.
static void refresh(void) {
    wchar_t text[48], widest[48];
    status_text(text, widest);
    if (!g_flash) tray_tip(text);
    int len = lstrlenW(text);

    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HGDIOBJ old_font = SelectObject(dc, g_font);
    TEXTMETRICW tm;
    SIZE ts, ws;
    GetTextMetricsW(dc, &tm);
    GetTextExtentPoint32W(dc, text, len, &ts);
    GetTextExtentPoint32W(dc, widest, lstrlenW(widest), &ws);
    int pad = scaled(12), h = tm.tmHeight + scaled(14), w = (ts.cx > ws.cx ? ts.cx : ws.cx) + 2 * pad;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD *px;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (bmp) {
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT box = {0, 0, w, h};
        DrawTextW(dc, text, len, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        float rad = (float)scaled(6);
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                DWORD *p = &px[yy * w + xx];
                float a = rounded_rect_cover(xx + 0.5f, yy + 0.5f, (float)w, (float)h, rad) * BG_ALPHA;   // box
                float R = GetRValue(C_BG) * a, G = GetGValue(C_BG) * a, B = GetBValue(C_BG) * a;
                float t = (*p & 0xFF) / 255.0f;   // text coverage
                R = GetRValue(C_TEXT) * t + R * (1 - t);
                G = GetGValue(C_TEXT) * t + G * (1 - t);
                B = GetBValue(C_TEXT) * t + B * (1 - t);
                a = t + a * (1 - t);
                *p = (DWORD)(a * 255 + 0.5f) << 24 | (DWORD)(R + 0.5f) << 16 | (DWORD)(G + 0.5f) << 8 |
                     (DWORD)(B + 0.5f);
            }
        }
        RECT m = game_monitor();
        POINT src = {0, 0}, dst = {clampi(m.left + g_x, m.left, m.right - w), clampi(m.top + g_y, m.top, m.bottom - h)};
        SIZE sz = {w, h};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_wnd, screen, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA);
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, old_font);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
}

static void update_visibility(void) {
    BOOL show = g_flash || is_fortnite(GetForegroundWindow());
    if (show && !IsWindowVisible(g_wnd)) {
        refresh();   // the game may have moved to another monitor since the last draw
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    } else if (!show && IsWindowVisible(g_wnd)) {
        ShowWindow(g_wnd, SW_HIDE);
    }
    // Stay above the game, but only touch the z-order when something else got on top.
    if (show && GetTopWindow(NULL) != g_wnd)
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void flash(const wchar_t *msg, UINT ms) {
    g_flash = msg;
    refresh();
    update_visibility();
    SetTimer(g_wnd, 1, ms, NULL);
}

static void quit(void) {
    g_exiting = TRUE;
    flash(L"PingPeek off", 1000);
}

static void tray_menu(void) {
    HMENU menu = CreatePopupMenu();
    POINT pt;
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(g_tray_wnd);   // otherwise the menu doesn't close when you click elsewhere
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_tray_wnd, NULL);
    PostMessageW(g_tray_wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == ID_EXIT && !g_exiting) quit();
}

static LRESULT CALLBACK trayproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbar_created && msg) {   // Explorer restarted: put the icon back
        tray_update(NIM_ADD);
        return 0;
    }
    if (msg == WM_TRAY) {
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) tray_menu();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CALLBACK on_foreground(HWINEVENTHOOK hook, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD thread,
                                   DWORD time) {
    (void)hook; (void)ev; (void)hwnd; (void)obj; (void)child; (void)thread; (void)time;
    update_visibility();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_STATUS: {
        char dc[9] = {0};
        int state = (int)(wp & 0xFF), ms = (int)(wp >> 8);
        memcpy(dc, &lp, sizeof lp < 8 ? sizeof lp : 8);
        if (state != g_state || ms != g_ms || strcmp(dc, g_dc)) {   // redraw only when something changed
            g_state = state;
            g_ms = ms;
            memcpy(g_dc, dc, sizeof g_dc);
            if (!g_flash) refresh();
        }
        update_visibility();
        return 0;
    }
    case WM_TOGGLE:
        if (g_exiting) {   // run again while it was saying "off": keep it running after all
            g_exiting = FALSE;
            flash(L"PingPeek on", 2000);
        } else if (GetTickCount64() - g_started >= IGNORE_CLOSE_MS) {
            quit();
        }
        return 0;
    case WM_MOVETO:
        g_x = (int)wp;
        g_y = (int)lp;
        if (!g_exiting) flash(L"PingPeek moved here", 2000);
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, 1);
        g_flash = NULL;
        if (g_exiting) {
            DestroyWindow(hwnd);
        } else {
            refresh();
            update_visibility();
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL parse_coord(const wchar_t *s, int *out) {
    wchar_t *end;
    long v = wcstol(s, &end, 10);
    if (end == s || *end || v < 0 || v > 20000) return FALSE;
    *out = (int)v;
    return TRUE;
}

static HFONT make_font(int px) {
    return CreateFontW(-scaled(px), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    int argc, x = 0, y = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    BOOL has_pos = argc == 3, ok = argc == 1 || (has_pos && parse_coord(argv[1], &x) && parse_coord(argv[2], &y));
    LocalFree(argv);
    if (!ok) {
        MessageBoxW(NULL, USAGE, APP_NAME, MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    // One copy at a time. A second launch moves the running copy (with a position) or closes it (without).
    HANDLE single = CreateMutexW(NULL, FALSE, L"Local\\PingPeek");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND running = NULL;
        for (int i = 0; i < 30 && !(running = FindWindowW(APP_NAME, NULL)); i++) Sleep(100);   // may be starting
        if (running) PostMessageW(running, has_pos ? WM_MOVETO : WM_TOGGLE, (WPARAM)x, (LPARAM)y);
        return 0;
    }
    if (has_pos) {
        g_x = x;
        g_y = y;
    }

    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    g_scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0f;
    ReleaseDC(NULL, screen);
    g_font = make_font(FONT_PX);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);
    // Layered + transparent = click-through; noactivate + toolwindow = never steals focus, not in alt-tab.
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            APP_NAME, APP_NAME, WS_POPUP, 0, 0, 1, 1, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;

    WNDCLASSW tc = {0};
    tc.lpfnWndProc = trayproc;
    tc.hInstance = inst;
    tc.lpszClassName = L"PingPeekTray";
    RegisterClassW(&tc);
    g_tray_wnd = CreateWindowExW(0, tc.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    tray_update(NIM_ADD);

    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_foreground, 0, 0,
                    WINEVENT_OUTOFCONTEXT);
    g_started = GetTickCount64();
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else g_state = ST_ERROR;
    flash(L"PingPeek on", 2000);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}
