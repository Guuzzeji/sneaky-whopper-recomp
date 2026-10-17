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
    if (!s_open && !s_hud)
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
