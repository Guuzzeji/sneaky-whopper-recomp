import sys, struct, zlib
def conv(src, dst):
    b = open(src, 'rb').read()
    off, = struct.unpack_from('<I', b, 10); w, h = struct.unpack_from('<ii', b, 18); bpp, = struct.unpack_from('<H', b, 28)
    bp = bpp // 8; stride = (w * bp + 3) & ~3; raw = bytearray()
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        row = b[off + sy * stride: off + sy * stride + w * bp]
        raw.append(0)
        for x in range(w):
            raw += bytes((row[x*bp+2], row[x*bp+1], row[x*bp]))
    def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, abs(h), 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(bytes(raw), 6)) + ch(b'IEND', b''))
for p in sys.argv[1:]: conv(p, p[:-4] + '.png')
