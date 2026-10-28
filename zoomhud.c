// ZoomHUD — HUD с зумом камеры для Warcraft III 1.27b + управление зумом.
// Сборка: build3.bat. Устанавливается в папку игры как PPP.mix (грузит wl27.dll от w3l.exe).
// Настройки: ZoomHUD.ini рядом с DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <windns.h>
#include <intrin.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "winmm.lib")
#include <mmsystem.h>
#pragma comment(lib, "ws2_32.lib")

#define GAME_DLL        "Game.dll"
#define OFF_SETCAMERA   0x211E00     // void f(int, float* distance, float* time)
#define OFF_INGAME_ROOT 0xD46118     // цепочка «в игре»

// ------------------------------------------------------------------ состояние
static BYTE  *g_gameBase = NULL;
static volatile float g_zoom = 2350.0f;   // наша цель
static volatile float g_real = -1.0f;     // что сообщила игра (<0 = неизвестно)
static volatile int   g_hud  = 1;
static volatile int   g_hold = 0;
static volatile int   g_appliedVal = -1;
static volatile int   g_wasInGame  = 0;
static volatile int   g_debugAct = 0;
static volatile int   g_apm = 0;          // действий за последние 60 с
static int            g_apmCounts[60] = {0};
static DWORD          g_apmSec = 0;
static volatile int   g_ping = -1;        // мс, -1 = неизвестно
static volatile LONG g_exit = 0;          // процесс завершается — потокам пора выходить
static volatile LONG  g_frames = 0;       // кадры (EndScene)
static volatile DWORD g_timerStart = 0;   // старт игрового таймера
static volatile int   g_timerOn = 0;
static volatile LONG  g_framesLast = 0;
static DWORD g_frameHist[64];             // гистограмма дельт (мс)
static volatile int   g_fps10 = 0;        // FPS * 10
static volatile int   g_dbgSleep = 0;     // писать в лог Sleep из Game.dll
static int C_HiRes = 1;                   // поднять разрешение таймера (снимает кап 64 fps)
static DWORD          g_sleepReal = 0;    // адрес настоящего Sleep
static volatile DWORD g_peerIp = 0;

// ------------------------------------------------------------------ конфиг
static int C_Step=100, C_Min=150, C_Max=4050, C_Start=2350;
static int C_KeyUp=VK_PRIOR, C_KeyDown=VK_NEXT, C_KeyHold=VK_END, C_KeyHud=VK_INSERT;
static int C_Font=12, C_Hold=0, C_AutoHold=0, C_ShowReal=1;
static char C_FontFace[64] = "Georgia";
static int C_ColR=255, C_ColG=225, C_ColB=120;   // единый цвет всего HUD
static int C_Swallow = 1;
static int C_ShowTimer = 1;
static int C_HookES = 1, C_HookCam = 1;   // отключаемые перехваты (для диагностики)                        // не отдавать наши клавиши игре (иначе она крутит камеру)
static HHOOK g_kbHook = NULL;
static volatile LONG g_pu=0, g_pd=0, g_ph=0, g_pi=0;   // счётчики нажатий из хука
static int C_X=8, C_Y=8;   // позиция HUD
static int C_ShowApm=1, C_ShowPing=1, C_Gap=340, C_PingEvery=2000, C_Y2=-1;
static char C_PingHost[128] = {0};   // резервный адрес для пинга (если пира не нашли)

// ------------------------------------------------------------------ лог
static CRITICAL_SECTION g_logCs;
static int  g_logCsInit = 0;
static char g_logPath[MAX_PATH] = {0};

static void logf_(const char *fmt, ...)
{
    char buf[512]; char line[600];
    int n, m; va_list ap; HANDLE h; DWORD w; SYSTEMTIME st;

    if (!g_logPath[0]) return;
    va_start(ap, fmt); n = wvsprintfA(buf, fmt, ap); va_end(ap);
    if (g_logCsInit) EnterCriticalSection(&g_logCs);
    h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        GetLocalTime(&st);
        m = wsprintfA(line, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        WriteFile(h, line, m, &w, NULL);
        WriteFile(h, buf, n, &w, NULL);
        WriteFile(h, "\r\n", 2, &w, NULL);
        CloseHandle(h);
    }
    if (g_logCsInit) LeaveCriticalSection(&g_logCs);
}

static void module_dir(char *out)
{
    HMODULE self = NULL;
    char *s, *last = NULL;
    out[0] = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&logf_, &self);
    if (!GetModuleFileNameA(self, out, MAX_PATH)) { out[0] = 0; return; }
    for (s = out; *s; s++) if (*s == '\\') last = s;
    if (!last) { out[0] = 0; return; }
    last[1] = 0;
}

static void load_config(void)
{
    char path[MAX_PATH];
    module_dir(path);
    if (!path[0]) return;
    lstrcatA(path, "ZoomHUD.ini");

    C_Step     = GetPrivateProfileIntA("ZoomHUD", "Step",     100, path);
    C_Min      = GetPrivateProfileIntA("ZoomHUD", "Min",      150, path);
    C_Max      = GetPrivateProfileIntA("ZoomHUD", "Max",      4050, path);
    C_Start    = GetPrivateProfileIntA("ZoomHUD", "Start",    2350, path);
    C_KeyUp    = GetPrivateProfileIntA("ZoomHUD", "KeyUp",    VK_PRIOR,  path);
    C_KeyDown  = GetPrivateProfileIntA("ZoomHUD", "KeyDown",  VK_NEXT,   path);
    C_KeyHold  = GetPrivateProfileIntA("ZoomHUD", "KeyHold",  VK_END,    path);
    C_KeyHud   = GetPrivateProfileIntA("ZoomHUD", "KeyHud",   VK_INSERT, path);
    C_Font     = GetPrivateProfileIntA("ZoomHUD", "Font",     12, path);
    GetPrivateProfileStringA("ZoomHUD", "FontFace", "Georgia", C_FontFace, sizeof(C_FontFace), path);
    C_ColR     = GetPrivateProfileIntA("ZoomHUD", "ColorR",   255, path);
    C_ColG     = GetPrivateProfileIntA("ZoomHUD", "ColorG",   225, path);
    C_ColB     = GetPrivateProfileIntA("ZoomHUD", "ColorB",   120, path);
    C_Hold     = GetPrivateProfileIntA("ZoomHUD", "Hold",     0,  path);
    C_AutoHold = GetPrivateProfileIntA("ZoomHUD", "AutoHold", 0,  path);
    C_ShowReal = GetPrivateProfileIntA("ZoomHUD", "ShowReal", 1,  path);
    C_ShowApm    = GetPrivateProfileIntA("ZoomHUD", "ShowApm",    1, path);
    C_ShowPing   = GetPrivateProfileIntA("ZoomHUD", "ShowPing",   1, path);
    C_Gap        = GetPrivateProfileIntA("ZoomHUD", "Gap",        340, path);
    C_PingEvery  = GetPrivateProfileIntA("ZoomHUD", "PingEvery",  2000, path);
    if (C_PingEvery < 500) C_PingEvery = 500;
    g_debugAct = GetPrivateProfileIntA("ZoomHUD", "DebugAct", 0, path);
    g_dbgSleep = GetPrivateProfileIntA("ZoomHUD", "DebugSleep", 0, path);
    C_HiRes    = GetPrivateProfileIntA("ZoomHUD", "HighResTimer", 1, path);
    C_Swallow  = GetPrivateProfileIntA("ZoomHUD", "SwallowKeys", 1, path);
    C_ShowTimer = GetPrivateProfileIntA("ZoomHUD", "ShowTimer", 1, path);
    C_HookES   = GetPrivateProfileIntA("ZoomHUD", "HookEndScene", 1, path);
    C_HookCam  = GetPrivateProfileIntA("ZoomHUD", "HookCamera", 1, path);
    GetPrivateProfileStringA("ZoomHUD", "PingHost", "", C_PingHost, sizeof(C_PingHost), path);
    C_Y2       = GetPrivateProfileIntA("ZoomHUD", "Y2",      -1,  path);
    C_X        = GetPrivateProfileIntA("ZoomHUD", "X",        8,  path);
    C_Y        = GetPrivateProfileIntA("ZoomHUD", "Y",        8,  path);
    if (C_X < 0) C_X = 0;
    if (C_Y < 0) C_Y = 0;
    if (C_Y2 < 0) C_Y2 = C_Y;
    if (C_Step < 1) C_Step = 1;
    if (C_Min  < 0) C_Min  = 0;
    if (C_Max <= C_Min) C_Max = C_Min + 100;
    if (C_Font < 7)  C_Font = 7;
    if (C_Font > 40) C_Font = 40;
    if (C_KeyUp   <= 0) C_KeyUp   = VK_PRIOR;
    if (C_KeyDown <= 0) C_KeyDown = VK_NEXT;
    g_zoom = (float)C_Start;
    g_hold = C_Hold ? 1 : 0;
}

// ------------------------------------------------------------------ D3D9 / HUD
static void fmt_time(char *out, DWORD secs)
{
    if (secs >= 3600) wsprintfA(out, "%d:%02d:%02d", (int)(secs / 3600), (int)((secs / 60) % 60), (int)(secs % 60));
    else              wsprintfA(out, "%02d:%02d", (int)(secs / 60), (int)(secs % 60));
}
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9 *);
static EndScene_t g_origEndScene = NULL;
static HFONT      g_font = NULL;
static int        g_fontSize = 0;
static char       g_text[160];

static void draw_hud(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL; HDC dc = NULL; int n;

    if (!dev) return;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (SUCCEEDED(bb->GetDC(&dc)) && dc) {
        if (!g_font || g_fontSize != C_Font) {
            if (g_font) DeleteObject(g_font);
            g_font = CreateFontA(-C_Font, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 FF_DONTCARE, C_FontFace);
            g_fontSize = C_Font;
        }
        if (C_ShowReal && g_real > 0.0f && (int)g_real != (int)g_zoom)
            n = wsprintfA(g_text, "%d (now %d)%s", (int)g_zoom, (int)g_real, g_hold ? " *" : "");
        else
            n = wsprintfA(g_text, "%d%s", (int)g_zoom, g_hold ? " *" : "");
        if (n > 0) {
            HGDIOBJ old = SelectObject(dc, g_font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0, 0, 0));
            TextOutA(dc, C_X + 1, C_Y + 1, g_text, n);
            SetTextColor(dc, RGB(C_ColR, C_ColG, C_ColB));
            TextOutA(dc, C_X, C_Y, g_text, n);
            if (C_ShowApm || C_ShowPing) {
                char s2[128], pt[32]; int m = 0, fps = g_fps10;
                if (C_ShowPing) {
                    if (g_ping >= 0) wsprintfA(pt, "%d.%02dms", g_ping, 0);
                    else             lstrcpyA(pt, "0.00ms");
                } else pt[0] = 0;
                if (C_ShowPing) { wsprintfA(s2, "Ping: %s", pt); m = lstrlenA(s2); }
                if (C_ShowApm)  { wsprintfA(s2 + m, "%sAPM: %d", m ? "   " : "", g_apm); m = lstrlenA(s2); }
                { wsprintfA(s2 + m, "%sFPS: %d.%d", m ? "   " : "", fps / 10, fps % 10); m = lstrlenA(s2); }
                if (C_ShowTimer) {
                    char tb[24];
                    if (g_timerOn) fmt_time(tb, (GetTickCount() - g_timerStart) / 1000);
                    else           lstrcpyA(tb, "00:00");
                    wsprintfA(s2 + m, "%sTime: %s", m ? "   " : "", tb);
                    m = lstrlenA(s2);
                }
                if (m > 0) {
                    int x2 = C_X + C_Gap, y2 = C_Y2;
                    SetTextColor(dc, RGB(0, 0, 0));
                    TextOutA(dc, x2 + 1, y2 + 1, s2, m);
                    SetTextColor(dc, RGB(C_ColR, C_ColG, C_ColB));
                    TextOutA(dc, x2, y2, s2, m);
                }
            }
            SelectObject(dc, old);
        }
        bb->ReleaseDC(dc);
    }
    bb->Release();
}

static HRESULT WINAPI hkEndScene(IDirect3DDevice9 *dev)
{
    static LONGLONG prevQ = 0, freq = 0;
    static DWORD prev = 0;
    static int once = 0;
    if (!once) {                       // один раз: параметры презентации (vsync?)
        IDirect3DSwapChain9 *sc = NULL;
        once = 1;
        if (dev && SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
            D3DPRESENT_PARAMETERS pp;
            ZeroMemory(&pp, sizeof(pp));
            if (SUCCEEDED(sc->GetPresentParameters(&pp)))
                logf_("present: interval=%u windowed=%d %ux%u Hz=%u", pp.PresentationInterval,
                      pp.Windowed, pp.BackBufferWidth, pp.BackBufferHeight, pp.FullScreen_RefreshRateInHz);
            sc->Release();
        } else logf_("GetSwapChain failed");
    }
    DWORD now = GetTickCount();
    if (!freq) QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
    if (prevQ && g_dbgSleep && freq) {
        LONGLONG q; int b;
        QueryPerformanceCounter((LARGE_INTEGER *)&q);
        b = (int)((q - prevQ) * 1000 * 2 / freq);   // в полумиллисекундах
        if (b >= 0 && b < 64) g_frameHist[b]++;
    }
    { LONGLONG q0; QueryPerformanceCounter((LARGE_INTEGER *)&q0); prevQ = q0; }
    if (prev && g_dbgSleep) { DWORD d = now - prev; (void)d; }
    prev = now;
    InterlockedIncrement(&g_frames);
    __try { if (g_hud && !g_exit) draw_hud(dev); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    return g_origEndScene(dev);
}

static void *g_hookFn[4]; static BYTE g_hookOrig[4][8]; static int g_hookLen[4]; static int g_hookN = 0;

static void save_orig(void *fn, const BYTE *bytes, int len)
{
    if (g_hookN >= 4) return;
    memcpy(g_hookOrig[g_hookN], bytes, len);
    g_hookFn[g_hookN] = fn;
    g_hookLen[g_hookN] = len;
    g_hookN++;
}

// вернуть исходные байты — вызывается при выходе процесса
static void remove_hooks(void)
{
    int i;
    for (i = 0; i < g_hookN; i++) {
        DWORD old;
        BYTE *t = (BYTE *)g_hookFn[i];
        if (!t) continue;
        if (VirtualProtect(t, 16, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(t, g_hookOrig[i], g_hookLen[i]);
            VirtualProtect(t, 16, old, &old);
            FlushInstructionCache(GetCurrentProcess(), t, 16);
        }
    }
    logf_("hooks removed: %d", g_hookN);
}

static int install_inline5(void *fn, void *hook, BYTE *tramp)
{
    BYTE *t = (BYTE *)fn; DWORD old;
    if (!fn) return 0;
    memcpy(tramp, t, 5);
    save_orig(fn, t, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((t + 5) - (tramp + 10));
    if (!VirtualProtect(tramp, 32, PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (!VirtualProtect(t, 16, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9;
    *(DWORD *)(t + 1) = (DWORD)((BYTE *)hook - (t + 5));
    VirtualProtect(t, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    return 1;
}

static int install_inline7(void *fn, void *hook, BYTE *tramp)
{
    BYTE *t = (BYTE *)fn; DWORD old;
    if (!fn) return 0;
    memcpy(tramp, t, 7);
    save_orig(fn, t, 7);
    tramp[7] = 0xE9;
    *(DWORD *)(tramp + 8) = (DWORD)((t + 7) - (tramp + 12));
    if (!VirtualProtect(tramp, 32, PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (!VirtualProtect(t, 16, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9;
    *(DWORD *)(t + 1) = (DWORD)((BYTE *)hook - (t + 5));
    t[5] = 0x90; t[6] = 0x90;
    VirtualProtect(t, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    return 1;
}

// ------------------------------------------------------------------ перехват клавиш (чтобы игра их не видела)
static LRESULT CALLBACK kb_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        int vk = (int)k->vkCode;
        {   // работаем только когда активно наше окно — иначе не мешаем другим программам
            DWORD fpid = 0;
            HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &fpid);
            if (fpid != GetCurrentProcessId()) return CallNextHookEx(g_kbHook, code, wp, lp);
        }
        int mine = (vk == C_KeyUp || vk == C_KeyDown || vk == C_KeyHold || vk == C_KeyHud);
        if (mine && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
            if (vk == C_KeyUp)        InterlockedIncrement(&g_pu);
            else if (vk == C_KeyDown) InterlockedIncrement(&g_pd);
            else if (vk == C_KeyHold) InterlockedIncrement(&g_ph);
            else                      InterlockedIncrement(&g_pi);
            if (C_Swallow) return 1;      // игра нажатие не увидит
        }
        if (mine && (wp == WM_KEYUP || wp == WM_SYSKEYUP) && C_Swallow) return 1;
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

static DWORD WINAPI kb_thread(LPVOID param)
{
    MSG msg;
    (void)param;
    g_kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, kb_proc, GetModuleHandleA(NULL), 0);
    logf_("keyboard hook: %p", (void *)g_kbHook);
    while (!g_exit) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(50);
    }
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = NULL; }
    return 0;
}

// ------------------------------------------------------------------ камера
static BYTE g_trampCam[32];

static void _cdecl call_set_camera(int a1, float *dist, float *time)
{
    ((void (__cdecl *)(int, float *, float *))(void *)g_trampCam)(a1, dist, time);
}

// перехват: узнаём, что игра делает с камерой сама (колесо, карта)
static void _cdecl hkSetCamera(int a1, float *dist, float *time)
{
    static DWORD lastLog = 0; static int lastVal = -100000;
    int v = dist ? (int)*dist : -1;
    if (dist) g_real = *dist;
    if (v != lastVal || GetTickCount() - lastLog > 2000) {
        lastLog = GetTickCount(); lastVal = v;
        logf_("game> camera field=%d dist=%d t=%d", a1, v, time ? (int)*time : -1);
    }
    call_set_camera(a1, dist, time);
}

static int in_game(void);

// ---------------------------------------------------------------- APM / пинг
static void apm_poll(void)
{
    static BYTE prev[256];
    static DWORD lastPoll = 0;
    DWORD now = GetTickCount(), sec;
    int vk, ig;

    if (now - lastPoll < 30) return;
    lastPoll = now;
    sec = now / 1000;
    if (sec != g_apmSec) { g_apmSec = sec; g_apmCounts[sec % 60] = 0; }
    ig = in_game();
    for (vk = 1; vk < 256; vk++) {
        BYTE st;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) { prev[vk] = 0; continue; }  // дубли VK_L*/VK_R*
        st = (BYTE)((GetAsyncKeyState(vk) & 0x8000) ? 1 : 0);
        if (st && !prev[vk] && ig && C_ShowApm) { g_apmCounts[sec % 60]++; if (g_debugAct) logf_("act vk=%d", vk); }
        prev[vk] = st;
    }
    {
        int sum = 0;
        for (vk = 0; vk < 60; vk++) sum += g_apmCounts[vk];
        g_apm = sum;
    }
}

static void apm_reset(void)
{
    int i;
    for (i = 0; i < 60; i++) g_apmCounts[i] = 0;
    g_apm = 0;
}

// адрес пира игры: установленные TCP-соединения нашего процесса
static DWORD find_peer_ip(void)
{
    DWORD size = 64 * 1024, pid = GetCurrentProcessId(), found = 0;
    PMIB_TCPTABLE_OWNER_PID t = (PMIB_TCPTABLE_OWNER_PID)HeapAlloc(GetProcessHeap(), 0, size);
    if (!t) return 0;
    if (GetExtendedTcpTable(t, &size, FALSE, 2 /*AF_INET*/, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        DWORD i;
        for (i = 0; i < t->dwNumEntries; i++) {
            DWORD a = t->table[i].dwRemoteAddr;
            if (t->table[i].dwOwningPid == pid && t->table[i].dwState == MIB_TCP_STATE_ESTAB &&
                a != 0 && a != 0x0100007F) { found = a; break; }
        }
    }
    HeapFree(GetProcessHeap(), 0, t);
    return found;
}

// "1.2.3.4" -> DWORD в сетевом порядке (как inet_addr)
static DWORD parse_ip4(const char *s)
{
    unsigned a, b, c, d, n = 0, v[4];
    const char *p = s;
    while (*p && n < 4) {
        unsigned x = 0; int digits = 0;
        while (*p >= '0' && *p <= '9') { x = x * 10 + (*p - '0'); p++; digits++; }
        if (!digits || x > 255) return 0;
        v[n++] = x;
        if (*p == '.') p++; else break;
    }
    if (n != 4 || *p) return 0;
    a = v[0]; b = v[1]; c = v[2]; d = v[3];
    return (DWORD)(a | (b << 8) | (c << 16) | (d << 24));
}

// резервный адрес из ini (числовой IP или имя хоста)
static DWORD resolve_host(const char *host)
{
    DWORD ip;
    PDNS_RECORDA rec = NULL;
    if (!host || !host[0]) return 0;
    ip = parse_ip4(host);
    if (ip) return ip;
    if (DnsQuery_A(host, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &rec, NULL) == 0 && rec) {
        ip = rec->Data.A.IpAddress;
        DnsRecordListFree(rec, DnsFreeRecordListDeep);
        return ip;
    }
    return 0;
}

static int ping_once(DWORD ip)
{
    HANDLE h = IcmpCreateFile();
    BYTE send[32] = {0};
    BYTE reply[sizeof(ICMP_ECHO_REPLY) + 32 + 8];
    int rtt = -1;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (IcmpSendEcho(h, (IPAddr)ip, send, sizeof(send), NULL, reply, sizeof(reply), 1000) > 0)
        rtt = (int)((PICMP_ECHO_REPLY)reply)->RoundTripTime;
    IcmpCloseHandle(h);
    return rtt;
}

static DWORD WINAPI ping_thread(LPVOID param)
{
    (void)param;
    for (;;) {
        if (C_ShowPing) {
            DWORD ip = find_peer_ip();
            int fromPeer = ip ? 1 : 0;
            if (!ip) ip = resolve_host(C_PingHost);
            g_peerIp = ip;
            g_ping = ip ? ping_once(ip) : -1;
            {
                static DWORD lastIp = 0; static int lastRtt = -1000;
                if (ip != lastIp || g_ping < 0 || lastRtt < 0 || g_ping - lastRtt > 9 || lastRtt - g_ping > 9) {
                    lastIp = ip; lastRtt = g_ping;
                    logf_("ping %s=%d.%d.%d.%d rtt=%d", fromPeer ? "peer" : "host",
                          ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, g_ping);
                }
            }
        }
        {
            DWORD left = (DWORD)C_PingEvery;
            while (left && !g_exit) { DWORD d = left > 100 ? 100 : left; Sleep(d); left -= d; }
        }
    }
}

static void apply_zoom(void)
{
    float v, t = 0.5f;
    if (g_exit || !g_gameBase || !in_game()) return;   // вне игры вызов камеры даёт исключение
    v = g_zoom;
    __try {
        call_set_camera(0, &v, &t);
        g_appliedVal = (int)g_zoom;
        g_real = v;
        logf_("apply %d ok", (int)v);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("apply EXCEPTION"); }
}

static int game_state(void)
{
    DWORD v;
    if (!g_gameBase) return -1;
    __try {
        v = *(DWORD *)(g_gameBase + OFF_INGAME_ROOT); if (!v) return -1;
        v = *(DWORD *)(v + 0x34);  if (!v) return -1;
        v = *(DWORD *)(v + 0x10);  if (!v) return -1;
        v = *(DWORD *)(v + 0x8);   if (!v) return -1;
        return (int)*(DWORD *)(v + 0x278);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int in_game(void)
{
    int s = game_state();
    return (s == 4 || s == 6) ? 1 : 0;   // 4 — игра, 6 — пауза (меню F10 не должна сбрасывать таймер)
}

// ------------------------------------------------------------------ перехват исключений (диагностика вылетов)
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_gameBase) {
        BYTE *addr = (BYTE *)ep->ExceptionRecord->ExceptionAddress;
        DWORD rva = (DWORD)(addr - g_gameBase);
        if (rva < 0xE6E000) {          // только код Game.dll
            static volatile LONG logged = 0;
            if (InterlockedExchange(&logged, 1) == 0) {
                void *stack[24]; USHORT n = CaptureStackBackTrace(0, 24, stack, NULL);
                char buf[512]; int i, p2 = 0;
                p2 += wsprintfA(buf + p2, "VEH AV tid=%lu at Game.dll+0x%X (read=%lu) stack:",
                                GetCurrentThreadId(), rva,
                                ep->ExceptionRecord->ExceptionInformation[0]);
                for (i = 0; i < n && p2 < 400; i++) {
                    DWORD off = (DWORD)((BYTE *)stack[i] - g_gameBase);
                    p2 += wsprintfA(buf + p2, " %X", off < 0xE6E000 ? off : 0xFFFFFFFF);
                }
                logf_("%s", buf);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ перехват Sleep (диагностика FPS-лимитера)
static void __stdcall hkSleep(DWORD ms)
{
    void *ret = _ReturnAddress();
    logf_("Sleep(%lu) <- Game.dll+0x%X", ms, g_gameBase ? (DWORD)((BYTE *)ret - g_gameBase) : 0);
    ((void (WINAPI *)(DWORD))g_sleepReal)(ms);
}

static DWORD (WINAPI *g_realWFSO)(HANDLE, DWORD) = NULL;
static DWORD __stdcall hkWFSO(HANDLE h, DWORD ms)
{
    static int seen[64]; static int nseen = 0; static DWORD lastLog = 0;
    void *ret = _ReturnAddress();
    DWORD rva = g_gameBase ? (DWORD)((BYTE *)ret - g_gameBase) : 0;
    if (ms && nseen < 64) {
        int k, dup = 0;
        for (k = 0; k < nseen; k++) if (seen[k] == (int)ms) { dup = 1; break; }
        if (!dup) { seen[nseen++] = (int)ms; logf_("WFSO timeout=%lu <- 0x%X", ms, rva); }
        if (GetTickCount() - lastLog > 3000) { lastLog = GetTickCount(); logf_("WFSO timeout=%lu <- 0x%X (повтор)", ms, rva); }
    }
    return g_realWFSO(h, ms);
}

static int patch_wait_iat(void);

static int patch_sleep_iat_mod(HMODULE g);
static int patch_sleep_iat(void)
{
    int n = 0;
    n += patch_sleep_iat_mod(GetModuleHandleA(NULL));      // war3.exe
    n += patch_sleep_iat_mod(GetModuleHandleA(GAME_DLL));  // Game.dll
    return n;
}

static int patch_iat_fn(HMODULE g, void *realFn, void *newFn)
{
    BYTE *base = (BYTE *)g;
    IMAGE_DOS_HEADER *dos; IMAGE_NT_HEADERS *nt; IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0; DWORD old;
    if (!g) return 0;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress == 0) return 0;
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; iat->u1.Function; iat++) {
            if ((void *)iat->u1.Function == realFn) {
                if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    iat->u1.Function = (ULONG_PTR)newFn;
                    VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                    patched++;
                }
            }
        }
    }
    return patched;
}

static int patch_wait_iat(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void *real = (void *)GetProcAddress(k32, "WaitForSingleObject");
    int n = 0;
    if (!real) return 0;
    if (!g_realWFSO) g_realWFSO = (DWORD (WINAPI *)(HANDLE, DWORD))real;
    n += patch_iat_fn(GetModuleHandleA(NULL),     real, (void *)hkWFSO);
    n += patch_iat_fn(GetModuleHandleA(GAME_DLL), real, (void *)hkWFSO);
    return n;
}

static int patch_sleep_iat_mod(HMODULE g)
{
    int patched = 0;
    DWORD old;
    BYTE *base = (BYTE *)g;
    IMAGE_DOS_HEADER *dos; IMAGE_NT_HEADERS *nt; IMAGE_IMPORT_DESCRIPTOR *imp;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void *realSleep = (void *)GetProcAddress(k32, "Sleep");

    if (!g || !realSleep) return 0;
    if (!g_sleepReal) g_sleepReal = (DWORD)realSleep;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; iat->u1.Function; iat++) {
            if ((void *)iat->u1.Function == realSleep) {
                if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    iat->u1.Function = (ULONG_PTR)hkSleep;
                    VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                    patched++;
                }
            }
        }
    }
    return patched;
}

// ------------------------------------------------------------------ хуки
static HWND make_dummy_window(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ZoomHudWnd";
    RegisterClassA(&wc);
    return CreateWindowExA(0, "ZoomHudWnd", "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                           NULL, NULL, wc.hInstance, NULL);
}

static void hook_endscene(void)
{
    typedef HRESULT (WINAPI *C9Ex_t)(UINT, IDirect3D9Ex **);
    HMODULE d3d = LoadLibraryA("d3d9.dll");
    C9Ex_t c9ex; HWND wnd; IDirect3D9Ex *ex = NULL; IDirect3DDevice9Ex *dev = NULL;
    D3DPRESENT_PARAMETERS pp;

    if (!d3d) { logf_("d3d9 load failed"); return; }
    c9ex = (C9Ex_t)GetProcAddress(d3d, "Direct3DCreate9Ex");
    wnd  = make_dummy_window();
    if (!c9ex || !wnd) { logf_("EndScene: нет c9ex/окна"); return; }
    if (FAILED(c9ex(D3D_SDK_VERSION, &ex)) || !ex) { logf_("Create9Ex failed"); return; }

    ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
    pp.BackBufferWidth = 8; pp.BackBufferHeight = 8;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (SUCCEEDED(ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &dev)) && dev) {
        void **vt = *(void ***)dev;
        BYTE *tr = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        void *fnEnd = vt[42];
        if (tr && fnEnd && ((BYTE *)fnEnd)[0] == 0x6A && install_inline7(fnEnd, (void *)hkEndScene, tr)) {
            g_origEndScene = (EndScene_t)(void *)tr;
            logf_("EndScene hooked (7b) %p", fnEnd);
        } else if (tr && fnEnd && install_inline5(fnEnd, (void *)hkEndScene, tr)) {
            g_origEndScene = (EndScene_t)(void *)tr;
            logf_("EndScene hooked (5b) %p", fnEnd);
        } else {
            g_origEndScene = (EndScene_t)fnEnd;
            logf_("EndScene: inline не встал (%p)", fnEnd);
        }
        dev->Release();
    } else logf_("CreateDeviceEx failed");
    ex->Release();
    if (wnd) DestroyWindow(wnd);
}

static void hook_camera(void)
{
    BYTE *fn = g_gameBase + OFF_SETCAMERA;
    if (install_inline5(fn, (void *)hkSetCamera, g_trampCam))
        logf_("SetCamera hooked %p", (void *)fn);
    else
        logf_("SetCamera hook failed (%02X %02X %02X %02X %02X)", fn[0], fn[1], fn[2], fn[3], fn[4]);
}

static DWORD WINAPI hook_thread(LPVOID param)
{
    (void)param;
    while (!g_gameBase && !g_exit) { HMODULE g = GetModuleHandleA(GAME_DLL); if (g) g_gameBase = (BYTE *)g; Sleep(20); }
    if (g_exit) return 0;
    logf_("Game.dll=%p", (void *)g_gameBase);
    if (C_HookCam) hook_camera();
    if (g_dbgSleep) logf_("Sleep IAT patched: %d | WaitForSingleObject: %d", patch_sleep_iat(), patch_wait_iat());
    if (C_HookES) hook_endscene();
    return 0;
}

// ------------------------------------------------------------------ главный цикл
static DWORD WINAPI main_thread(LPVOID param)
{
    int pUp = 0, pDown = 0, pHold = 0, pHud = 0;
    DWORD lastTick = 0, lastHold = 0, lastGameSet = 0;
    (void)param;

    {
        HANDLE h = CreateThread(NULL, 0, hook_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        h = CreateThread(NULL, 0, ping_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        if (C_Swallow) { h = CreateThread(NULL, 0, kb_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    }
    for (;;) {
        int up = 0, down = 0, hold = 0, hud = 0;
        if (g_exit) break;
        if (g_kbHook) {
            LONG nUp = InterlockedExchange(&g_pu, 0), nDn = InterlockedExchange(&g_pd, 0);
            LONG nHd = InterlockedExchange(&g_ph, 0), nHu = InterlockedExchange(&g_pi, 0);
            up = nUp > 0; down = nDn > 0; hold = nHd > 0; hud = nHu > 0;
            if (nUp > 1) g_zoom += (float)C_Step * (nUp - 1);
            if (nDn > 1) g_zoom -= (float)C_Step * (nDn - 1);
        } else {
            up   = (GetAsyncKeyState(C_KeyUp)   & 0x8000) != 0;
            down = (GetAsyncKeyState(C_KeyDown) & 0x8000) != 0;
            hold = (GetAsyncKeyState(C_KeyHold) & 0x8000) != 0;
            hud  = (GetAsyncKeyState(C_KeyHud)  & 0x8000) != 0;
        }

        if (up && !pUp) {
            g_zoom += (float)C_Step;
            if (g_zoom > (float)C_Max) g_zoom = (float)C_Max;
            apply_zoom(); lastGameSet = GetTickCount();
        }
        if (down && !pDown) {
            g_zoom -= (float)C_Step;
            if (g_zoom < (float)C_Min) g_zoom = (float)C_Min;
            apply_zoom(); lastGameSet = GetTickCount();
        }
        if (hold && !pHold) { g_hold = !g_hold; logf_("hold=%d", g_hold); }
        if (hud && !pHud)   { g_hud = !g_hud;   logf_("hud=%d", g_hud); }
        pUp = up; pDown = down; pHold = hold; pHud = hud;

        apm_poll();
        {
            static DWORD lastFps = 0; static LONG lastFrames = 0;
            DWORD nowt = GetTickCount();
            if (nowt - lastFps >= 1000) {
                LONG f = InterlockedCompareExchange(&g_frames, 0, 0);
                DWORD dt = nowt - lastFps;
                if (lastFps && dt) g_fps10 = (int)((f - lastFrames) * 10000 / (LONG)dt);
                lastFps = nowt; lastFrames = f;
            }
        }
        {
            static DWORD lastApmLog = 0;
            if (GetTickCount() - lastApmLog > 5000) {
                lastApmLog = GetTickCount();
                {
                    static ULONGLONG prevK = 0, prevU = 0; static DWORD prevT = 0;
                    FILETIME c, e, k, u; ULONGLONG kk = 0, uu = 0; DWORD t = GetTickCount();
                    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
                        kk = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
                        uu = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
                    }
                    if (prevT && t > prevT) {
                        double dt = (double)(t - prevT) * 10000.0;   // в 100-нс тиках
                        int cpu = (int)(((double)((kk - prevK) + (uu - prevU)) * 100.0) / dt);
                        logf_("apm=%d fps=%d.%d cpu=%d%%", g_apm, g_fps10/10, g_fps10%10, cpu);
                    } else logf_("apm=%d fps=%d.%d", g_apm, g_fps10/10, g_fps10%10);
                    prevK = kk; prevU = uu; prevT = t;
                }
                if (g_dbgSleep) {
                    char hb[256]; int k, p2 = 0;
                    for (k = 1; k < 64; k++) if (g_frameHist[k]) { p2 += wsprintfA(hb + p2, "%d.%dms:%lu ", k/2, (k%2)*5, g_frameHist[k]); g_frameHist[k] = 0; }
                    if (p2) logf_("frame deltas: %s", hb);
                }
            }
        }

        if (g_gameBase && GetTickCount() - lastTick > 200) {
            int ig = in_game();
            {
                static int lastState = -999;
                int st = game_state();
                if (st != lastState) { lastState = st; logf_("game state = %d (in_game=%d)", st, ig); }
            }
            lastTick = GetTickCount();
            if (ig && !g_wasInGame) {
                g_appliedVal = -1; apm_reset();
                g_timerStart = GetTickCount(); g_timerOn = 1;
                logf_("timer start");
            }
            if (!ig && g_wasInGame) {          // игра закончилась — таймер сбрасываем
                g_timerOn = 0; g_timerStart = 0;
                logf_("timer stop");
            }
            g_wasInGame = ig;
            if (ig && C_AutoHold && g_real > 0.0f && (int)g_real != (int)g_zoom &&
                GetTickCount() - lastGameSet > 2000) {
                if (!g_hold) { g_hold = 1; logf_("auto-hold ON"); }
            }
            if (ig && (g_appliedVal != (int)g_zoom || (g_hold && (GetTickCount() - lastHold > 500)))) {
                if (g_hold) lastHold = GetTickCount();
                apply_zoom();
            }
        }
        Sleep(15);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    HANDLE h;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_exit, 1);
        remove_hooks();                 // чтобы ничего не указывало в нашу память после выгрузки
        if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = NULL; }
        Sleep(120);                     // дать потокам выйти, пока грузчик не выгрузил нас
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = 1;
        module_dir(g_logPath);
        if (g_logPath[0]) lstrcatA(g_logPath, "zoom.log");
        load_config();
        AddVectoredExceptionHandler(1, veh_handler);
        if (C_HiRes) logf_("timeBeginPeriod(1) -> %u", (unsigned)timeBeginPeriod(1));
        logf_("=== ZoomHUD pid=%lu step=%d range=%d..%d start=%d font=%d real=%d",
              GetCurrentProcessId(), C_Step, C_Min, C_Max, C_Start, C_Font, C_ShowReal);
        h = CreateThread(NULL, 0, main_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
