// Tests for PingPeek: log parsing, input validation, the UDP ping and the overlay drawing, against
// synthetic logs, a local echo server and (when present) this PC's real Fortnite logs. Run test.cmd.
// PINGPEEK_LIVE=1 also pings the server of the match you are in. Example addresses are from the
// documentation ranges (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24).
#include <stdio.h>
#define WinMain pingpeek_winmain
#include "../pingpeek.c"

static int fails, passes;
#define CHECK(cond, name)                                       \
    do {                                                        \
        if (cond) passes++;                                     \
        else {                                                  \
            printf("FAIL  %s  (line %d)\n", name, __LINE__);    \
            fails++;                                            \
        }                                                       \
    } while (0)

#define PFX "[2026.10.05-14.18.46:902][240]"

// ---- pure functions -----------------------------------------------------------------------------

static void test_helpers(void) {
    CHECK(!strcmp(body(PFX "LogNet: Browse: x"), "LogNet: Browse: x"), "body strips [time][frame]");
    CHECK(!strcmp(body("[2026.10.04-14.33.40:754][ 50]LogNet: x"), "LogNet: x"), "body handles padded frame");
    CHECK(!strcmp(body("LogNet: no prefix"), "LogNet: no prefix"), "body keeps prefix-less startup lines");
    CHECK(!strcmp(body("[broken"), "[broken"), "body leaves malformed prefix alone");

    CHECK(!strcmp(place_name("DE"), "Frankfurt"), "DE shows as Frankfurt");
    CHECK(!strcmp(place_name("ncal"), "California"), "data center codes match case-insensitively");
    CHECK(!strcmp(place_name("XYZ"), "XYZ"), "unknown data center code shown as is");

    int v;
    CHECK(parse_coord(L"0", &v) && v == 0, "coord 0");
    CHECK(parse_coord(L"1700", &v) && v == 1700, "coord 1700");
    CHECK(!parse_coord(L"abc", &v), "coord rejects text");
    CHECK(!parse_coord(L"12px", &v), "coord rejects trailing junk");
    CHECK(!parse_coord(L"-5", &v), "coord rejects negative");
    CHECK(!parse_coord(L"99999", &v), "coord rejects huge");
    CHECK(!parse_coord(L"", &v), "coord rejects empty");

    ULONG a = 0;
    USHORT p = 0;
    CHECK(parse_ping_address("203.0.113.10, PingPort: 22222", &a, &p) && p == 22222 && a == inet_addr("203.0.113.10"),
          "ping address: real line");
    CHECK(parse_ping_address("198.51.100.7, PingPort: 9", &a, &p) && p == 9, "ping address: other port");
    const char *bad[] = {"127.0.0.1, PingPort: 22222", "0.1.2.3, PingPort: 22222", "224.0.0.1, PingPort: 22222",
                         "255.255.255.255, PingPort: 22222", "1.2.3, PingPort: 22222", "1.2.3.4.5, PingPort: 22222",
                         "1.2.3.400, PingPort: 22222", "example.com, PingPort: 22222", "1.2.3.4, PingPort: 0",
                         "1.2.3.4, PingPort: 70000", "1.2.3.4, PingPort: ", "1.2.3.4 PingPort: 22222",
                         "1.2.3.4", "123456789012345678, PingPort: 1", ""};
    for (int i = 0; i < (int)(sizeof bad / sizeof *bad); i++) {
        a = 7;
        p = 7;
        CHECK(!parse_ping_address(bad[i], &a, &p) && a == 7 && p == 7, bad[i]);
    }
}

// ---- log parsing through tail() -----------------------------------------------------------------

static wchar_t path[MAX_PATH];
static void put(const wchar_t *mode, const char *s) {
    FILE *f = _wfopen(path, mode);
    fputs(s, f);
    fclose(f);
}

static void test_log(void) {
    static Log L;
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"pingpeek_test.log");
    DeleteFileW(path);
    lstrcpyW(L.path, path);

    tail(&L);
    CHECK(!L.found && !L.in_match, "missing log: reported as not found");

    put(L"wb", "\xEF\xBB\xBF" "Log file open\r\nLogNet: Browse: /Game/Maps/Frontend?Name=Player\r\n"
               PFX "LogMatchmaking: [MatchmakingClient:x][client_match_assigned] AccountId=[a], MatchId=[b], "
                   "ServerAttributes=[{/Fortnite.com/Matchmaking:SubRegion:DE, sessionId:c}]\r\n");
    tail(&L);
    CHECK(L.found && !L.in_match && !strcmp(L.next_dc, "DE") && !L.dc[0], "assignment alone: pending DE, no match");

    put(L"ab", PFX "LogNet: Browse: 203.0.113.10:9108/Game/Maps/Frontend?EncryptionToken=x\r\n");
    tail(&L);
    CHECK(L.in_match && !strcmp(L.dc, "DE") && !L.next_dc[0] && !L.port, "join: in match, DE, waiting for address");

    put(L"ab", PFX "LogNet: ServerSetPingAddress: EPingType::UDPQoS PingAddress: 203.0.113.10, PingPort: 22222\r\n");
    tail(&L);
    CHECK(L.port == 22222 && L.addr == inet_addr("203.0.113.10"), "ping address picked up");

    put(L"ab", PFX "LogNet: ServerSetPingAddress: EPingType::UDPQoS PingAddress: 127.0.0.1, PingPort: 22222\r\n");
    tail(&L);
    CHECK(L.addr == inet_addr("203.0.113.10"), "loopback ping address ignored, previous kept");

    put(L"ab", PFX "LogNet: Browse: /Game/Maps/Frontend?closed\r\n");
    tail(&L);
    CHECK(!L.in_match && !L.port && !L.dc[0], "back to lobby clears match, address and label");

    put(L"ab", PFX "LogNet: ServerSetPingAddress: EPingType::UDPQoS PingAddress: 1.2.3.4, PingPort: 22222\r\n");
    tail(&L);
    CHECK(!L.port, "ping address outside a match ignored");

    put(L"ab", PFX "LogSomething: player said LogNet: Browse: 192.0.2.9:1/x\r\n");
    tail(&L);
    CHECK(!L.in_match, "marker in the middle of another line ignored");

    put(L"ab", PFX "LogNet: Browse: 198.51");
    tail(&L);
    CHECK(!L.in_match, "half-written line not acted on yet");
    put(L"ab", ".100.7:9010/Game/Maps/Frontend\r\n");
    tail(&L);
    CHECK(L.in_match && !L.dc[0], "half-written line completed; join without assignment has no label");

    put(L"ab", PFX "LogExit: Exiting.\r\n");
    tail(&L);
    CHECK(!L.in_match && !L.port, "LogExit ends the match");

    {   // a marker beyond the 4096-byte cap is ignored; the next line still parses
        static char big[6000];
        memset(big, 'x', 5000);
        strcpy(big + 5000, "LogNet: Browse: 1.2.3.4:1/x\r\n");
        put(L"ab", big);
        tail(&L);
        CHECK(!L.in_match, "over-long line ignored");
        put(L"ab", PFX "LogNet: Browse: 1.2.3.4:1/x\r\n");
        tail(&L);
        CHECK(L.in_match, "line after an over-long line parses");
    }

    put(L"ab", PFX "LogMatchmaking: [client_match_assigned] ServerAttributes=[{/Fortnite.com/Matchmaking:SubRegion:NCAL}]\r\n"
               PFX "LogNet: Browse: 1.2.3.4:1/x\r\n");
    tail(&L);
    CHECK(!strcmp(L.dc, "NCAL"), "join while already in a match takes the new assignment");
    put(L"ab", PFX "LogMatchmaking: [client_match_assigned] ServerAttributes=[{/Fortnite.com/Matchmaking:SubRegion:ABCDEFGHIJ}]\r\n"
               PFX "LogNet: Browse: 1.2.3.4:1/x\r\n");
    tail(&L);
    CHECK(!strcmp(L.dc, "ABCDEFG"), "over-long data center capped at 7 letters");
    // Seen in real logs: some assignments carry no SubRegion. Must not reuse the last match's label.
    put(L"ab", PFX "LogMatchmaking: [client_match_assigned] ServerAttributes=[{sessionId:x, assignmentToken:y}]\r\n"
               PFX "LogNet: Browse: 1.2.3.4:1/x\r\n");
    tail(&L);
    CHECK(L.in_match && !L.dc[0], "assignment without a data center: no stale label");

    // Fortnite's real rotation: rename the old log away, create a new one with the same name.
    wchar_t backup[MAX_PATH];
    lstrcpyW(backup, path);
    lstrcatW(backup, L".bak");
    DeleteFileW(backup);
    MoveFileW(path, backup);
    tail(&L);
    CHECK(!L.found, "between rename and recreate: not found");
    put(L"wb", "\xEF\xBB\xBF" PFX "LogNet: Browse: /Game/Maps/Frontend?Name=Player\r\n");
    tail(&L);
    CHECK(L.found && !L.in_match && !L.dc[0] && L.off < 200, "new log: state reset");

    put(L"ab", PFX "LogNet: Browse: 1.2.3.4:1/x\r\n");
    tail(&L);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    SetFilePointer(f, 10, NULL, FILE_BEGIN);
    SetEndOfFile(f);
    CloseHandle(f);
    put(L"ab", "\r\n");
    tail(&L);
    CHECK(!L.in_match && L.off == 12, "log truncated in place: state reset");

    DeleteFileW(path);
    DeleteFileW(backup);
}

// ---- UDP ping against a local echo server ----------------------------------------------------------

enum { ECHO, SILENT, MUTATE, SHORT_REPLY, LATE_FIRST, DELAY_50 };
static volatile LONG mode;
static volatile double server_held_ms;   // how long DELAY_50 really held the packet (Sleep rounds up)
static SOCKET srv;

static DWORD WINAPI echo_server(LPVOID arg) {
    (void)arg;
    char buf[64];
    int late_done = 0;
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from, n = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) continue;
            return 0;
        }
        if (n < PKT_LEN) continue;   // like the real responder
        switch (mode) {
        case SILENT: break;
        case MUTATE: buf[3] ^= 1; sendto(srv, buf, PKT_LEN, 0, (struct sockaddr *)&from, fl); break;
        case SHORT_REPLY: sendto(srv, buf, 10, 0, (struct sockaddr *)&from, fl); break;
        case LATE_FIRST:
            if (!late_done) {
                late_done = 1;
                Sleep(1300);
            }
            sendto(srv, buf, PKT_LEN, 0, (struct sockaddr *)&from, fl);
            break;
        case DELAY_50: {
            LARGE_INTEGER fq, a, b;
            QueryPerformanceFrequency(&fq);
            QueryPerformanceCounter(&a);
            Sleep(50);
            QueryPerformanceCounter(&b);
            server_held_ms = (b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart;
            sendto(srv, buf, PKT_LEN, 0, (struct sockaddr *)&from, fl);
            break;
        }
        default: sendto(srv, buf, PKT_LEN, 0, (struct sockaddr *)&from, fl); break;
        }
    }
}

static int timed_ping(SOCKET s, ULONG addr, USHORT port, USHORT seq, double *took_ms) {
    LARGE_INTEGER fq, a, b;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&a);
    int r = udp_ping(s, addr, port, seq);
    QueryPerformanceCounter(&b);
    *took_ms = (b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart;
    return r;
}

static void test_ping(void) {
    SOCKET s = open_ping_socket();
    CHECK(s != INVALID_SOCKET, "ping socket opens");
    srv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(srv, (struct sockaddr *)&sa, sizeof sa);
    int sl = sizeof sa;
    getsockname(srv, (struct sockaddr *)&sa, &sl);
    USHORT port = ntohs(sa.sin_port);
    ULONG lo = inet_addr("127.0.0.1");
    CloseHandle(CreateThread(NULL, 0, echo_server, NULL, 0, NULL));
    double took;
    int r;

    mode = ECHO;
    r = timed_ping(s, lo, port, 1, &took);
    CHECK(r >= 0 && r <= 5, "echo: answered quickly");

    // Shared CI machines schedule threads late now and then, so allow more slack there.
    wchar_t ci[8];
    double slack = GetEnvironmentVariableW(L"CI", ci, 8) ? 15 : 3;
    mode = DELAY_50;
    r = timed_ping(s, lo, port, 2, &took);
    printf("      server held the echo %.1f ms, ping measured %d ms\n", server_held_ms, r);
    CHECK(r >= 0 && r - server_held_ms > -1.5 && r - server_held_ms < slack, "round trip matches the real delay");

    mode = SILENT;
    r = timed_ping(s, lo, port, 3, &took);
    CHECK(r == -1 && took >= 950 && took <= 1300, "no echo: lost after ~1 s");

    mode = MUTATE;
    r = timed_ping(s, lo, port, 4, &took);
    CHECK(r == -1, "altered echo rejected");

    mode = SHORT_REPLY;
    r = timed_ping(s, lo, port, 5, &took);
    CHECK(r == -1, "short reply rejected");

    mode = LATE_FIRST;
    r = timed_ping(s, lo, port, 6, &took);
    CHECK(r == -1, "late echo: first ping times out");
    r = timed_ping(s, lo, port, 7, &took);
    printf("      ping after a late echo measured %d ms\n", r);
    CHECK(r >= 150 && r <= 600, "late echo of ping 6 not counted as ping 7");

    // Nothing listening: Windows answers with ICMP port-unreachable. Must time out cleanly, and the
    // socket must keep working afterwards.
    mode = ECHO;
    r = timed_ping(s, lo, 1, 8, &took);
    CHECK(r == -1, "closed port: lost, no error");
    r = timed_ping(s, lo, port, 9, &took);
    CHECK(r >= 0, "socket still works after port-unreachable");

    closesocket(srv);
    closesocket(s);
}

// ---- overlay drawing ------------------------------------------------------------------------------

static SIZE box_size(int state, int ms, const char *dc) {
    RECT r;
    g_state = state;
    g_ms = ms;
    strcpy(g_dc, dc);
    refresh();
    GetWindowRect(g_wnd, &r);
    SIZE s = {r.right - r.left, r.bottom - r.top};
    return s;
}

static void test_render(void) {
    if (!GetSystemMetrics(SM_CMONITORS)) {
        printf("  (no display - skipped)\n");
        return;
    }
    g_font = make_font(FONT_PX);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"pingpeek_test";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"pingpeek_test", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL,
                            NULL, NULL);   // never shown

    SIZE a = box_size(ST_PING, 5, "DE"), b = box_size(ST_PING, 55, "DE"), c = box_size(ST_PING, 555, "DE");
    CHECK(a.cx == b.cx && b.cx == c.cx, "box width doesn't change with the number of digits");
    SIZE lobby = box_size(ST_IDLE, 0, ""), lost = box_size(ST_LOST, 0, "DE");
    CHECK(a.cy == lobby.cy && a.cy == lost.cy, "box height is the same in every state");
    HANDLE me = GetCurrentProcess();
    refresh();
    DWORD gdi0 = GetGuiResources(me, GR_GDIOBJECTS), user0 = GetGuiResources(me, GR_USEROBJECTS);
    for (int i = 0; i < 5000; i++) {
        g_state = i % 7;
        g_ms = i % 1000;
        strcpy(g_dc, i % 3 ? "DE" : "");
        refresh();
    }
    DWORD gdi1 = GetGuiResources(me, GR_GDIOBJECTS), user1 = GetGuiResources(me, GR_USEROBJECTS);
    printf("      5000 redraws: GDI objects %lu -> %lu, USER objects %lu -> %lu\n", gdi0, gdi1, user0, user1);
    CHECK(gdi1 == gdi0 && user1 == user0, "redrawing leaks no GDI/USER objects");

    // The box stays fully on the monitor whatever position was asked for.
    RECT r, m = game_monitor();
    g_state = ST_PING;
    g_x = 20000;
    g_y = 20000;
    refresh();
    GetWindowRect(g_wnd, &r);
    CHECK(r.right <= m.right && r.bottom <= m.bottom && r.left >= m.left && r.top >= m.top, "far-off position clamped on screen");
    g_x = 12;
    g_y = 12;
    refresh();
    GetWindowRect(g_wnd, &r);
    CHECK(r.left == m.left + 12 && r.top == m.top + 12, "position is relative to the game's monitor");
    DestroyWindow(g_wnd);
}

// ---- this PC's real Fortnite logs ------------------------------------------------------------------

static void replay(const wchar_t *file, int *joins_total, int *no_addr_total) {
    static Log L;
    static char raw[1 << 20];
    memset(&L, 0, sizeof L);
    FILE *f = _wfopen(file, L"rb");
    if (!f) return;
    int joins = 0, with_addr = 0, labelled = 0, pending = 0, lines_left = 0;
    long line_no = 0, join_line = 0;
    while (fgets(raw, sizeof raw, f)) {
        line_no++;
        size_t n = strlen(raw);
        if (n > sizeof L.line - 1) n = sizeof L.line - 1;
        memcpy(L.line, raw, n);
        L.line[n] = 0;
        BOOL was_in = L.in_match;
        USHORT had_port = L.port;
        const char *b = body(L.line);
        BOOL join = !strncmp(b, "LogNet: Browse: ", 16) && b[16] != '/';
        on_line(&L);
        if (join) {
            joins++;
            pending = 1;
            join_line = line_no;
            if (L.dc[0]) labelled++;
        }
        if (pending && !had_port && L.port) {
            with_addr++;
            pending = 0;
        }
        if (pending && was_in && !L.in_match) pending = 0;   // left before an address came
    }
    lines_left = (int)(line_no - join_line);
    fclose(f);
    int missing = joins - with_addr;
    // A join in the last few lines of a log that ended abruptly cannot have an address yet.
    if (missing == 1 && pending && lines_left < 2000) missing = 0;
    printf("  %-46ls joins=%-3d with ping address=%-3d with data center=%-3d\n", wcsrchr(file, L'\\') + 1, joins,
           with_addr, labelled);
    *joins_total += joins;
    *no_addr_total += missing;
}

static void test_real_logs(void) {
    wchar_t dir[MAX_PATH], pat[MAX_PATH], file[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    lstrcatW(dir, L"\\FortniteGame\\Saved\\Logs\\");
    lstrcpyW(pat, dir);
    lstrcatW(pat, L"FortniteGame*.log");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  (no Fortnite logs on this PC - skipped)\n");
        return;
    }
    int joins = 0, missing = 0;
    do {
        lstrcpyW(file, dir);
        lstrcatW(file, fd.cFileName);
        replay(file, &joins, &missing);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    CHECK(joins > 0 && missing == 0, "every real match join got a ping address");
}

static void live(void) {
    static Log L;
    wchar_t v[4];
    if (!GetEnvironmentVariableW(L"PINGPEEK_LIVE", v, 4)) return;
    printf("\n== live: the match you are in ==\n");
    GetEnvironmentVariableW(L"LOCALAPPDATA", L.path, MAX_PATH);
    lstrcatW(L.path, L"\\FortniteGame\\Saved\\Logs\\FortniteGame.log");
    tail(&L);
    if (!L.in_match || !L.port) {
        printf("  not in a match with a ping address - skipped\n");
        return;
    }
    SOCKET s = open_ping_socket();
    unsigned char *b = (unsigned char *)&L.addr;
    for (int i = 1; i <= 5; i++) {
        int r = udp_ping(s, L.addr, L.port, (USHORT)i);
        printf("  %s %u.%u.%u.%u:%u  %d ms\n", L.dc, b[0], b[1], b[2], b[3], L.port, r);
        Sleep(1000);
    }
    closesocket(s);
}

int main(void) {
    printf("== helpers ==\n");
    test_helpers();
    printf("== log parsing ==\n");
    test_log();
    printf("== UDP ping (local echo server) ==\n");
    test_ping();
    printf("== overlay drawing ==\n");
    test_render();
    printf("== real Fortnite logs on this PC ==\n");
    test_real_logs();
    live();
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails != 0;
}
