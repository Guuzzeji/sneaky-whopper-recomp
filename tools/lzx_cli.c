/* lzx_cli: decompress a raw Xbox 360 LZX stream (as found in a XEX's
 * de-chunked basefile) with libmspack's lzxd.
 *
 *   lzx_cli <in> <out> <out_size> <window_size>
 *
 * Build once (libmspack cloned to C:\repos\_research\libmspack):
 *   cl /O2 /nologo /I<mspack>\libmspack\mspack tools\lzx_cli.c
 *      <mspack>\libmspack\mspack\lzxd.c <mspack>\libmspack\mspack\system.c
 *      /Fe:build\lzx_cli.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <mspack.h>
#include <system.h>
#include <lzx.h>

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: lzx_cli <in> <out> <out_size> <window_size>\n");
        return 2;
    }
    off_t out_size = (off_t)strtoul(argv[3], NULL, 0);
    unsigned long window = strtoul(argv[4], NULL, 0);
    int bits = 0;
    while ((1ul << bits) < window) bits++;

    struct mspack_system *sys = mspack_default_system;
    struct mspack_file *in = sys->open(sys, argv[1], MSPACK_SYS_OPEN_READ);
    struct mspack_file *out = sys->open(sys, argv[2], MSPACK_SYS_OPEN_WRITE);
    if (!in || !out) { fprintf(stderr, "lzx_cli: cannot open files\n"); return 1; }

    struct lzxd_stream *lzx = lzxd_init(sys, in, out, bits, 0, 0x8000, out_size, 0);
    int err = lzx ? lzxd_decompress(lzx, out_size) : MSPACK_ERR_NOMEMORY;
    if (lzx) lzxd_free(lzx);
    sys->close(in);
    sys->close(out);
    if (err) fprintf(stderr, "lzx_cli: error %d\n", err);
    return err;
}
