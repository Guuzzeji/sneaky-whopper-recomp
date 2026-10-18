/*
 * main_web.c: the WebAssembly entry point (Emscripten, -sPROXY_TO_PTHREAD).
 *
 * main.c's boot sequence without the Windows parts (VEH crash reports,
 * dbghelp, the sampler, WinMain). main() runs on a pthread, so the guest's
 * main thread may block like any other.
 *
 * Game files: the page is served from a folder holding game/ (tools/
 * web_port.py makes one), and game/manifest.txt lists every file under it.
 * Each becomes an entry in a WasmFS fetch directory at /game: read lazily,
 * a chunk at a time, with HTTP range requests -- nothing is downloaded up
 * front. Saves live in memory for now.
 *
 * Switches: the page passes its query string as the environment (index.html),
 * so ?RECOMP_SCALE=1&RECOMP_PAD_SCRIPT=... work as they do natively.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <emscripten.h>
#include <emscripten/wasmfs.h>

#include <xbox/xboxrecomp.h>

extern RECOMP_TLS uint32_t g_esp;
extern MCPXAPUState *g_apu_state;
extern int recomp_dispatch_init(void);
extern void xdk_mainCRTStartup(void);
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern void xbox_OhciInit(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern int xbox_Nv2aMirrorFence(uint32_t device_ptr_va, uint32_t put_off, uint32_t get_ptr_off);
extern void d3d9_hle_init(void);
extern void web_host_init(void);

#define GAME_URL  "/game"            /* where the page's server has game/ */
#define CHUNK     (1u << 20)         /* range request size */

static void env_default_on(const char *name)
{
    const char *v = getenv(name);
    if (!v)
        setenv(name, "1", 1);
    else if (!strcmp(v, "0"))
        unsetenv(name);
}

/* Every directory on the way to `path`. Plain mkdir and open(O_CREAT), not
 * wasmfs_create_*: only entries made inside the fetch directory learn their
 * path, and the path is the URL they are fetched from. */
static void make_dirs(char *path)
{
    char *s;
    for (s = path + 1; (s = strchr(s, '/')) != NULL; s++) {
        *s = 0;
        mkdir(path, 0777);
        *s = '/';
    }
}

static int mount_game(void)
{
    backend_t be = wasmfs_create_fetch_backend(GAME_URL, CHUNK);
    char line[512], path[600];
    int n = 0, fd;
    FILE *f;

    if (wasmfs_create_directory("/game", 0777, be) != 0)
        return 0;
    fd = open("/game/manifest.txt", O_CREAT | O_RDONLY, 0444);
    if (fd < 0)
        return 0;
    close(fd);
    f = fopen("/game/manifest.txt", "r");
    if (!f) {
        fprintf(stderr, "[WEB] no %s/manifest.txt: serve the folder tools/web_port.py made\n", GAME_URL);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || !strcmp(line, "manifest.txt"))
            continue;
        snprintf(path, sizeof path, "/game/%s", line);
        make_dirs(path);
        fd = open(path, O_CREAT | O_RDONLY, 0444);
        if (fd >= 0) {
            close(fd);
            n++;
        } else if (n == 0) {
            fprintf(stderr, "[WEB] cannot create %s (%d)\n", path, fd);
        }
    }
    fclose(f);
    fprintf(stderr, "[WEB] %d game files at /game (fetched from %s on demand)\n", n, GAME_URL);
    return n > 0;
}

static int load_file(const char *path, void **out, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = n > 0 ? malloc((size_t)n) : NULL;
    if (!*out || fread(*out, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(*out);
        return 0;
    }
    fclose(f);
    *size = (size_t)n;
    return 1;
}

int main(void)
{
    void *xbe = NULL;
    size_t xbe_size = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=== Sneak King - Static Recompilation (WebAssembly) ===\n");

    if (!mount_game() || !load_file("/game/default.xbe", &xbe, &xbe_size)) {
        fprintf(stderr, "[WEB] cannot read /game/default.xbe\n");
        return 1;
    }
    mkdir("/save", 0777);
    printf("XBE loaded: %zu bytes\n", xbe_size);

    env_default_on("RECOMP_AC97_READY");
    env_default_on("RECOMP_HEAP_RECLAIM");
    env_default_on("RECOMP_VBLANK");
    env_default_on("RECOMP_USB");
    env_default_on("RECOMP_KEYBOARD");
    /* The 360's HD textures, as natively (main.c); ?RECOMP_HD_TEX=0 keeps
     * the Xbox's. RECOMP_WIDESCREEN comes from the page, which sizes its
     * canvas to match (index.html). */
    if (!getenv("RECOMP_HD_TEX"))
        setenv("RECOMP_HD_TEX", "/game/hd", 1);
    else if (!strcmp(getenv("RECOMP_HD_TEX"), "0"))
        unsetenv("RECOMP_HD_TEX");

    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "[WEB] memory layout failed\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED to initialise");
    }
    xbox_OhciInit();
    xbox_kernel_init();
    xbox_path_init("/game", "/save");
    xbox_kernel_bridge_init();
    xbox_Nv2aMirrorFence(0x0020D328, 0x2C, 0x30);
    d3d9_hle_init();
    web_host_init();
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable\n");
    xbox_WatchdogStart();

    printf("\nStarting game...\n");
    xdk_mainCRTStartup();
    printf("\nGame returned.\n");
    return 0;
}
