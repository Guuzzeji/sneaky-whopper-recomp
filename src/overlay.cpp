/* Debug overlay: Dear ImGui drawn over the presented frame.
 *
 *   F1  the panels (D3D9, health, cheats); the mouse and keys go to them
 *   F2  a frame-rate line in the corner
 *
 * The framebuffer window belongs to the toolkit's window thread, and ImGui
 * is not thread-safe, so the window hook only queues messages; present()
 * (the game thread) feeds them to ImGui, draws and renders. Everything here
 * runs under d3d9_hle.c's device lock. */
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "overlay.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

extern "C" {
uint32_t xbox_ContiguousAllocatedBytes(void);
uint32_t xbox_ContiguousInUseBytes(void);
int xbox_HeapReclaimEnabled(void);
unsigned xa2_underruns(void);
void mkb_set_suspended(int on);
int mkb_cursor_hidden(void);
typedef int (*xbox_fb_msg_hook_t)(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
void xbox_FramebufferWindowSetMsgHook(xbox_fb_msg_hook_t fn);
}

static volatile LONG s_open, s_hud;

/* -- Window messages: queued on the window thread ---------------------------- */

struct Msg { HWND h; UINT m; WPARAM w; LPARAM l; };
static CRITICAL_SECTION s_qlock;
static Msg s_q[256];
static unsigned s_qn;

static int msg_hook(void *h, unsigned m, uintptr_t w, intptr_t l)
{
    /* No pointer over the picture while the mouse turns the camera. This is
     * the window's thread, the only one whose SetCursor shows. */
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && mkb_cursor_hidden()) {
        SetCursor(NULL);
        return 1;
    }
    if (m == WM_KEYDOWN && !(l & (1 << 30))) {          /* not a repeat */
        if (w == VK_F1) {
            LONG open = !s_open;
            InterlockedExchange(&s_open, open);
            mkb_set_suspended(open);
            return 1;
        }
        if (w == VK_F2) {
            InterlockedExchange(&s_hud, !s_hud);
            return 1;
        }
    }
    if (!s_open)
        return 0;
    bool key = m >= WM_KEYFIRST && m <= WM_KEYLAST;
    bool mouse = m >= WM_MOUSEFIRST && m <= WM_MOUSELAST;
    if (!key && !mouse && m != WM_SETFOCUS && m != WM_KILLFOCUS)
        return 0;
    EnterCriticalSection(&s_qlock);
    if (s_qn < sizeof s_q / sizeof s_q[0])
        s_q[s_qn++] = Msg{ (HWND)h, m, (WPARAM)w, (LPARAM)l };
    LeaveCriticalSection(&s_qlock);
    return key;                       /* keys are the overlay's while it is open */
}

static void drain_messages(void)
{
    Msg q[256];
    unsigned n;
    EnterCriticalSection(&s_qlock);
    n = s_qn;
    memcpy(q, s_q, n * sizeof q[0]);
    s_qn = 0;
    LeaveCriticalSection(&s_qlock);
    for (unsigned i = 0; i < n; i++)
        ImGui_ImplWin32_WndProcHandler(q[i].h, q[i].m, q[i].w, q[i].l);
}

/* -- Samples ------------------------------------------------------------------ */

enum { HIST = 240 };
static float s_ms[HIST];              /* host time between presents */
static int s_hist_pos;
static double s_fps;

struct ThreadCpu { DWORD id; double pct; ULONGLONG last; };
static ThreadCpu s_thr[64];
static int s_nthr;

/* Each thread's share of one core over the last second. */
static void sample_threads(double wall_100ns)
{
    ThreadCpu now[64];
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    if (snap == INVALID_HANDLE_VALUE)
        return;
    for (BOOL ok = Thread32First(snap, &te); ok && n < 64; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        FILETIME c, e, k, u;
        if (!t)
            continue;
        if (GetThreadTimes(t, &c, &e, &k, &u)) {
            ULONGLONG used = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime)
                           + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
            double pct = 0.0;
            for (int i = 0; i < s_nthr; i++)
                if (s_thr[i].id == te.th32ThreadID && wall_100ns > 0)
                    pct = (double)(used - s_thr[i].last) * 100.0 / wall_100ns;
            now[n++] = ThreadCpu{ te.th32ThreadID, pct, used };
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    std::sort(now, now + n, [](const ThreadCpu &a, const ThreadCpu &b) { return a.pct > b.pct; });
    memcpy(s_thr, now, n * sizeof now[0]);
    s_nthr = n;
}

static unsigned s_underruns_per_s;

static void sample(void)
{
    static LARGE_INTEGER freq, last, sec0;
    static unsigned frames, und0;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        s_ms[s_hist_pos] = (float)((now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart);
        s_hist_pos = (s_hist_pos + 1) % HIST;
    }
    last = now;
    frames++;
    if (!sec0.QuadPart) {
        sec0 = now;
        und0 = xa2_underruns();
        sample_threads(0);
    } else if (now.QuadPart - sec0.QuadPart >= freq.QuadPart) {
        double secs = (double)(now.QuadPart - sec0.QuadPart) / freq.QuadPart;
        unsigned und = xa2_underruns();
        s_fps = frames / secs;
        s_underruns_per_s = und - und0;
        und0 = und;
        frames = 0;
        sec0 = now;
        if (s_open)
            sample_threads(secs * 1e7);
    }
}

/* -- Panels ------------------------------------------------------------------- */

static const char *level_name(uint32_t type)
{
    static const char *const names[] = { "Logging", "Suburbia", "Construction", "Urban" };
    return type < 4 ? names[type] : "?";
}

static void panel_d3d9(const OverlayD3dStats *st)
{
    float mx = 0.0f, sum = 0.0f;
    for (float v : s_ms) { mx = std::max(mx, v); sum += v; }
    ImGui::Text("%.1f fps   %.2f ms avg   %.2f ms worst (last %d frames)",
                s_fps, sum / HIST, mx, (int)HIST);
    ImGui::PlotLines("##ms", s_ms, HIST, s_hist_pos, "frame time (ms)", 0.0f, 50.0f,
                     ImVec2(-1, 80));
    if (ImGui::BeginTable("d3d", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char *k, const char *fmt, unsigned a, unsigned b = 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
            ImGui::TableNextColumn(); ImGui::Text(fmt, a, b);
        };
        row("Draws / frame", "%u", st->draws);
        row("Primitives / frame", "%u", st->prims);
        row("Render state sets / frame", "%u", st->states);
        row("Textures created / frame", "%u", st->tex_creates);
        row("Vertex programs compiled / frame", "%u", st->vs_compiles);
        row("Render target uploads / frame", "%u", st->rt_uploads);
        row("Render targets", "%u / %u", st->rts, st->rts_max);
        row("Textures cached", "%u / %u", st->texs, st->texs_max);
        row("Vertex programs", "%u / %u", st->vss, st->vss_max);
        row("Vertex declarations", "%u / %u", st->vdecls, st->vdecls_max);
        row("Window", "%u x %u", st->sc_w, st->sc_h);
        row("Render scale", "%ux", st->scale);
        row("Presents", "%u", st->frame);
        row("HD textures (RECOMP_HD_TEX)", "%u", st->hd_textures);
        ImGui::EndTable();
    }
}

static void panel_health(const OverlayGameInfo &gi)
{
    PROCESS_MEMORY_COUNTERS pmc;
    double mb = 1024.0 * 1024.0;
    ImGui::Text("Contiguous memory: %.1f MB in use, %.1f MB high-water%s",
                xbox_ContiguousInUseBytes() / mb, xbox_ContiguousAllocatedBytes() / mb,
                xbox_HeapReclaimEnabled() ? "" : " (reclaim off)");
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        ImGui::Text("Host working set: %.0f MB", pmc.WorkingSetSize / mb);
    ImGui::Text("Audio underruns: %u total, %u in the last second",
                xa2_underruns(), s_underruns_per_s);
    ImGui::Separator();
    ImGui::Text("World mode %08X, state %u, level %08X%s", gi.world, gi.world_state, gi.level,
                gi.paused ? ", paused" : "");
    for (int i = 0; i < OVERLAY_LEVELS; i++)
        if (gi.levels[i] == gi.level && gi.level)
            ImGui::Text("Playing: %s", level_name(gi.level_type[i]));
    ImGui::Separator();
    ImGui::TextUnformatted("Threads (CPU % of one core, last second)");
    if (ImGui::BeginTable("thr", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < s_nthr && i < 12; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%5lu", s_thr[i].id);
            ImGui::TableNextColumn(); ImGui::Text("%5.1f%%", s_thr[i].pct);
        }
        ImGui::EndTable();
    }
}

/* Shooter mode: the guns are the folders tools/cod4_extract.py made; the
 * loadout is a primary and a sidearm (1 / 2 switch in game). */
static void panel_shooter(void)
{
    static char guns[64][64];
    static int ngun = -1, sel[2] = { 0, -1 };
    static bool on;
    ShooterHud h;

    if (ngun < 0) {
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(SHOOTER_GUN_DIR "/*", &fd);
        ngun = 0;
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.'
                    && ngun < 64)
                    snprintf(guns[ngun++], sizeof guns[0], "%s", fd.cFileName);
            } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
        /* a sensible start: an AK and a pistol if they were extracted */
        for (int i = 0; i < ngun; i++) {
            if (!strcmp(guns[i], "ak47_mp")) sel[0] = i;
            if (sel[1] < 0 && (!strcmp(guns[i], "deserteagle_mp") || !strcmp(guns[i], "beretta_mp")))
                sel[1] = i;
        }
        if (sel[1] < 0) sel[1] = 0;
    }
    if (!shooter_available()) {
        ImGui::TextDisabled("Shooter mode (CoD4): not available");
        ImGui::TextWrapped("It needs files from your own CoD4 install: run tools/cod4_extract.py "
                           "and tools/cod4_anim.py (docs/MODS.md).");
        return;
    }
    /* follow the game: the loadout may come from shooter.ini or RECOMP_SHOOTER */
    shooter_hud(&h);
    on = h.on != 0;
    for (int i = 0; i < ngun; i++) {
        if (!strcmp(guns[i], h.primary)) sel[0] = i;
        if (!strcmp(guns[i], h.sidearm)) sel[1] = i;
    }
    auto names = [](void *, int i, const char **out) { *out = guns[i]; return true; };
    bool changed = ImGui::Checkbox("Shooter mode (CoD4, third person)", &on);
    changed |= ImGui::Combo("Primary", &sel[0], names, NULL, ngun);
    changed |= ImGui::Combo("Sidearm", &sel[1], names, NULL, ngun);
    if (changed)
        game_set_shooter(on, guns[sel[0]], guns[sel[1]]);
    shooter_hud(&h);
    if (h.on)
        ImGui::TextWrapped("WASD moves (he keeps facing the aim), Shift sprints, C crouches (Ctrl holds it). "
                           "Left click fires, right click aims down the sights, R reloads, 1 / 2 switch guns. "
                           "Citizens knocked out: %d", h.knockouts);
}

/* CoD4's HUD: a crosshair whose gap is the spread cone, a hit marker, and
 * the gun's name over its ammo in the corner. */
static void shooter_overlay(void)
{
    ShooterHud h;
    shooter_hud(&h);
    if (!h.live)
        return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 sz = ImGui::GetIO().DisplaySize, c(sz.x * 0.5f, sz.y * 0.5f);
    float fov = h.fov > 1.0f ? h.fov : 65.0f;
    /* pixels per unit of tan(angle): the picture's height covers the 4:3 FOV */
    float k = sz.y * 0.5f * (4.0f / 3.0f) / tanf(fov * 0.5f * 0.0174533f);
    float gap = std::max(5.0f, k * tanf(h.spread * 0.5f * 0.0174533f)), len = 10.0f;
    float vis = (1.0f - h.ads) * (1.0f - h.sprint);
    ImU32 white = IM_COL32(255, 255, 255, (int)(230 * vis)), shade = IM_COL32(0, 0, 0, (int)(120 * vis));
    if (vis > 0.05f) {
        for (int pass = 0; pass < 2; pass++) {
            ImU32 col = pass ? white : shade;
            float o = pass ? 0.0f : 1.0f, t = pass ? 2.0f : 4.0f;
            dl->AddLine(ImVec2(c.x - gap - len + o, c.y + o), ImVec2(c.x - gap + o, c.y + o), col, t);
            dl->AddLine(ImVec2(c.x + gap + o, c.y + o), ImVec2(c.x + gap + len + o, c.y + o), col, t);
            dl->AddLine(ImVec2(c.x + o, c.y - gap - len + o), ImVec2(c.x + o, c.y - gap + o), col, t);
            dl->AddLine(ImVec2(c.x + o, c.y + gap + o), ImVec2(c.x + o, c.y + gap + len + o), col, t);
        }
    }
    if (h.ads > 0.5f)
        dl->AddCircleFilled(c, 2.5f, IM_COL32(255, 60, 40, (int)(255 * h.ads)));
    if (h.hit > 0.0f) {
        ImU32 hc = IM_COL32(255, 255, 255, (int)(255 * std::min(1.0f, h.hit * 5.0f)));
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                dl->AddLine(ImVec2(c.x + sx * 7.0f, c.y + sy * 7.0f),
                            ImVec2(c.x + sx * 15.0f, c.y + sy * 15.0f), hc, 2.5f);
    }
    if (h.reload > 0.0f) {            /* under the crosshair: what the reload has left to go */
        float w = 90.0f, y = c.y + 38.0f, fsr = ImGui::GetFontSize();
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fsr, FLT_MAX, 0.0f, "RELOADING");
        dl->AddText(ImGui::GetFont(), fsr, ImVec2(c.x - ts.x * 0.5f + 1, y + 1), IM_COL32(0, 0, 0, 170), "RELOADING");
        dl->AddText(ImGui::GetFont(), fsr, ImVec2(c.x - ts.x * 0.5f, y), IM_COL32(255, 210, 90, 235), "RELOADING");
        y += ts.y + 3.0f;
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, y), ImVec2(c.x + w * 0.5f, y + 4.0f), IM_COL32(0, 0, 0, 140), 2.0f);
        dl->AddRectFilled(ImVec2(c.x - w * 0.5f, y), ImVec2(c.x - w * 0.5f + w * std::min(1.0f, h.reload), y + 4.0f),
                          IM_COL32(255, 210, 90, 220), 2.0f);
    }
    char ammo[32];
    snprintf(ammo, sizeof ammo, "%d", h.ammo);
    float fs = ImGui::GetFontSize() * 2.2f, fsn = ImGui::GetFontSize() * 1.3f;
    ImVec2 as = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, ammo);
    ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(fsn, FLT_MAX, 0.0f, h.name);
    ImVec2 at(sz.x - as.x - 48.0f, sz.y - as.y - 48.0f), nt(sz.x - ns.x - 48.0f, at.y - ns.y - 4.0f);
    ImU32 acol = h.reload > 0.0f ? IM_COL32(255, 210, 90, 255)
               : h.ammo * 4 <= h.clip ? IM_COL32(255, 80, 60, 255) : IM_COL32(255, 255, 255, 235);
    dl->AddText(ImGui::GetFont(), fsn, ImVec2(nt.x + 2, nt.y + 2), IM_COL32(0, 0, 0, 170), h.name);
    dl->AddText(ImGui::GetFont(), fsn, nt, IM_COL32(220, 220, 220, 235), h.name);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 170), ammo);
    dl->AddText(ImGui::GetFont(), fs, at, acol, ammo);
    /* the clip as a row of rounds, CoD4 style, under the count */
    int n = std::min(h.clip, 60);
    float bw = 3.0f, gapx = 2.0f, x0 = sz.x - 48.0f - n * (bw + gapx), y0 = sz.y - 40.0f;
    for (int i = 0; i < n; i++)
        dl->AddRectFilled(ImVec2(x0 + i * (bw + gapx), y0), ImVec2(x0 + i * (bw + gapx) + bw, y0 + 9.0f),
                          i < n - h.ammo * n / std::max(1, h.clip) ? IM_COL32(90, 90, 90, 160)
                                                                   : IM_COL32(235, 235, 235, 220));
}

/* Skate mode (skate.c): Skate 3's flick-it on the mouse. */
static void panel_skate(void)
{
    SkateHud h;
    if (!skate_available()) {
        ImGui::TextDisabled("Skate mode (Skate 3): not available");
        ImGui::TextWrapped("It needs files from your own Skate 3 disc: run tools/skate3_anim.py "
                           "and tools/skate3_extract.py (docs/MODS.md).");
        return;
    }
    skate_hud(&h);
    bool on = h.on != 0;
    if (ImGui::Checkbox("Skate mode (Skate 3 style)", &on)) {
        if (on)
            game_set_shooter(0, NULL, NULL);
        skate_set(on);
    }
    if (!h.on)
        return;
    ImGui::Text("%s. Score %d, tricks %d.", h.walk ? "On foot, board in hand (G to ride)" : "Riding (G to walk)",
                h.score, h.tricks);
    ImGui::TextWrapped("W pushes, S brakes, A / D steer, the mouse looks around, Space ollies, G steps off or back on.");
    ImGui::TextWrapped("Tricks: jump (Space, or off a ledge), then flick the mouse quickly in one direction, early "
                       "enough for the board to come round.\n"
                       "  up: Ollie          up-left: Kickflip        up-right: Heelflip\n"
                       "  left: BS Shove-it  right: FS Shove-it\n"
                       "  down-left: Varial Kickflip  down-right: Varial Heelflip  down: 360 Flip\n"
                       "Hold Shift for the nollie versions of up, up-left and up-right.");
    ImGui::TextWrapped("Hold the left button for a manual (right: nose manual) and balance it with W / S. In "
                       "the air the buttons grab (left Melon, right Method, both Mute). Tricks in a row make a "
                       "line, banked after a second on plain wheels. Landing before a flip has come round is a "
                       "bail.");
}

/* Skate's HUD: the trick and its points as it lands, the score, the speed,
 * and where the flick-it stick is. */
static void skate_overlay(void)
{
    SkateHud h;
    skate_hud(&h);
    if (!h.live)
        return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 sz = ImGui::GetIO().DisplaySize;
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize();
    if (h.show_t > 0.0f && h.shown[0]) {
        float big = fs * 2.6f, a = std::min(1.0f, h.show_t * 2.0f);
        ImVec2 ts = font->CalcTextSizeA(big, FLT_MAX, 0.0f, h.shown);
        ImVec2 at(sz.x * 0.5f - ts.x * 0.5f, sz.y * 0.16f - (2.0f - h.show_t) * 10.0f);
        ImU32 col = h.bail ? IM_COL32(255, 70, 50, (int)(255 * a)) : IM_COL32(255, 230, 80, (int)(255 * a));
        dl->AddText(font, big, ImVec2(at.x + 3, at.y + 3), IM_COL32(0, 0, 0, (int)(180 * a)), h.shown);
        dl->AddText(font, big, at, col, h.shown);
    }
    if (h.combo_n > 0) {              /* the line being built: its tricks and what it's worth */
        char cl[96];
        float mid = fs * 1.4f;
        snprintf(cl, sizeof cl, "%d x %d", h.combo_pts, h.combo_n);
        ImVec2 cs = font->CalcTextSizeA(mid * 1.3f, FLT_MAX, 0.0f, cl);
        ImVec2 cp(sz.x * 0.5f - cs.x * 0.5f, sz.y * 0.16f + fs * 3.2f);
        dl->AddText(font, mid * 1.3f, ImVec2(cp.x + 2, cp.y + 2), IM_COL32(0, 0, 0, 170), cl);
        dl->AddText(font, mid * 1.3f, cp, IM_COL32(120, 230, 255, 235), cl);
        ImVec2 ls = font->CalcTextSizeA(mid, FLT_MAX, 0.0f, h.combo_line);
        ImVec2 lp(sz.x * 0.5f - ls.x * 0.5f, cp.y + cs.y + 2.0f);
        dl->AddText(font, mid, ImVec2(lp.x + 2, lp.y + 2), IM_COL32(0, 0, 0, 170), h.combo_line);
        dl->AddText(font, mid, lp, IM_COL32(235, 235, 235, 220), h.combo_line);
    }
    char line[64];
    snprintf(line, sizeof line, "%d", h.score);
    float mid = fs * 2.0f;
    ImVec2 ss = font->CalcTextSizeA(mid, FLT_MAX, 0.0f, line);
    ImVec2 sp(sz.x - ss.x - 44.0f, 36.0f);
    dl->AddText(font, mid, ImVec2(sp.x + 2, sp.y + 2), IM_COL32(0, 0, 0, 170), line);
    dl->AddText(font, mid, sp, IM_COL32(255, 255, 255, 235), line);
    snprintf(line, sizeof line, "%.0f km/h", h.speed * 0.0152f);   /* a unit is ~4.2 mm */
    ImVec2 ks = font->CalcTextSizeA(fs * 1.3f, FLT_MAX, 0.0f, line);
    ImVec2 kp(sz.x - ks.x - 44.0f, sp.y + ss.y + 2.0f);
    dl->AddText(font, fs * 1.3f, ImVec2(kp.x + 2, kp.y + 2), IM_COL32(0, 0, 0, 170), line);
    dl->AddText(font, fs * 1.3f, kp, IM_COL32(220, 220, 220, 230), line);
    /* the manual's balance: a bar with the marker drifting off the middle */
    if (h.manual) {
        float w = 220.0f, x0 = sz.x * 0.5f - w * 0.5f, y0 = sz.y * 0.78f;
        float b = std::max(-1.0f, std::min(1.0f, h.balance));
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + 10.0f), IM_COL32(0, 0, 0, 120), 4.0f);
        dl->AddRectFilled(ImVec2(x0 + w * 0.4f, y0), ImVec2(x0 + w * 0.6f, y0 + 10.0f), IM_COL32(80, 200, 90, 140));
        dl->AddCircleFilled(ImVec2(x0 + w * 0.5f * (1.0f + b), y0 + 5.0f), 8.0f,
                            fabsf(b) > 0.6f ? IM_COL32(255, 80, 60, 230) : IM_COL32(255, 230, 80, 230));
    }
    /* the flick-it stick */
    ImVec2 c(sz.x - 90.0f, sz.y - 90.0f);
    dl->AddCircleFilled(c, 40.0f, IM_COL32(0, 0, 0, 90));
    dl->AddCircle(c, 40.0f, IM_COL32(255, 255, 255, 120), 32, 2.0f);
    dl->AddCircleFilled(ImVec2(c.x + h.stick[0] * 32.0f, c.y - h.stick[1] * 32.0f), 9.0f,
                        IM_COL32(255, 230, 80, 220));
}

static void panel_cheats(const OverlayGameInfo &gi)
{
    if (!gi.gamestate) {
        ImGui::TextUnformatted("The game state is not set up yet.");
        return;
    }
    ImGui::TextWrapped("Unlocks change the loaded profile; the game saves them with the "
                       "rest of its progress.");
    if (ImGui::Button("Unlock all levels"))
        game_cheat_unlock_levels();
    ImGui::SameLine();
    if (ImGui::Button("Unlock all levels and missions"))
        game_cheat_unlock_all();
    /* GameState indexes unlocks and missions by g_levels[] slot, the same
     * index the game's own teleport menu uses. */
    for (int i = 0; i < 4; i++)
        ImGui::Text("  %-12s %-8s missions %05X", level_name(gi.level_type[i]),
                    gi.flags & (1u << i) ? "unlocked" : "locked", gi.missions[i]);
    ImGui::Separator();
    bool can_warp = gi.in_play && !gi.paused;
    ImGui::TextUnformatted(can_warp ? "Warp to level:"
                                    : "Warp to level (only during play, not paused):");
    ImGui::BeginDisabled(!can_warp);
    for (int i = 0; i < OVERLAY_LEVELS; i++) {
        if (!gi.levels[i])
            continue;
        char label[48];
        snprintf(label, sizeof label, "%s##warp%d", level_name(gi.level_type[i]), i);
        if (ImGui::Button(label)) {
            game_cheat_warp(i);
            InterlockedExchange(&s_open, 0);
            mkb_set_suspended(0);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::EndDisabled();
    ImGui::Separator();
    panel_shooter();
    ImGui::Separator();
    panel_skate();
}

/* -- Frame -------------------------------------------------------------------- */

extern "C" void overlay_present(void *dev_, void *hwnd_, void *bb_, const OverlayD3dStats *st)
{
    static bool hooked, ready;
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)dev_;
    HWND hwnd = (HWND)hwnd_;

    if (!hooked) {
        InitializeCriticalSection(&s_qlock);
        xbox_FramebufferWindowSetMsgHook(msg_hook);
        hooked = true;
    }
    sample();
    ShooterHud sh;
    shooter_hud(&sh);
    SkateHud kh;
    skate_hud(&kh);
    if (!s_open && !s_hud && !sh.live && !kh.live)
        return;
    if (!ready) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;
        /* The cursor shape is per thread, and this is not the window's. */
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX9_Init(dev);
        ready = true;
    }
    drain_messages();

    OverlayGameInfo gi;
    game_info(&gi);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    shooter_overlay();
    skate_overlay();
    if (s_hud) {
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##hud", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                     | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("%.1f fps  %u draws", s_fps, st->draws);
        ImGui::End();
    }
    if (s_open) {
        ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
        ImGui::Begin("Sneak King debug (F1 closes)");
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("D3D9")) { panel_d3d9(st); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Health")) { panel_health(gi); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Cheats")) { panel_cheats(gi); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
    ImGui::EndFrame();

    /* Into the swap chain's back buffer, leaving the device as the guest
     * state mirror expects it: the backend restores its own state block, and
     * the targets and viewport are put back here. */
    IDirect3DSurface9 *rt = NULL, *ds = NULL;
    D3DVIEWPORT9 vp;
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    dev->GetViewport(&vp);
    dev->SetRenderTarget(0, (IDirect3DSurface9 *)bb_);
    dev->SetDepthStencilSurface(NULL);
    if (SUCCEEDED(dev->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        dev->EndScene();
    }
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(ds);
    dev->SetViewport(&vp);
    if (rt) rt->Release();
    if (ds) ds->Release();
}
