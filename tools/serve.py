"""Serve the web build locally: python tools/serve.py [--port 8080]

The page needs cross-origin isolation (COOP/COEP) for its threads, and the
game files are read with HTTP range requests, neither of which the standard
library's http.server does. Serves:

    /            web/index.html and the build (build-web/sneakking_recomp.*)
    /game/...    the extracted disc in game/, read in place
    /game/manifest.txt   every file under game/, generated

Only for your own machine: it binds to localhost.
"""
import argparse
import http.server
import os
import posixpath
import re
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
BUILD = os.path.join(ROOT, "build-web")
GAME = os.path.join(ROOT, "game")


def manifest():
    out = []
    for d, _, files in os.walk(GAME):
        for f in files:
            rel = os.path.relpath(os.path.join(d, f), GAME).replace(os.sep, "/")
            out.append(rel)
    return ("\n".join(sorted(out)) + "\n").encode()


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def local_path(self):
        path = posixpath.normpath(urllib.parse.unquote(urllib.parse.urlsplit(self.path).path))
        if path in ("/", "/index.html"):
            return os.path.join(WEB, "index.html")
        if path.startswith("/game/"):
            full = os.path.normpath(os.path.join(GAME, path[len("/game/"):]))
            return full if full.startswith(GAME) else None
        name = path.lstrip("/")
        for base in (WEB, BUILD):
            full = os.path.normpath(os.path.join(base, name))
            if full.startswith(base) and os.path.isfile(full):
                return full
        return None

    def send_range(self, size, ctype):
        """Status and headers for the Range asked for; returns (start, end),
        or None after a 416."""
        start, end = 0, size - 1
        m = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range") or "")
        if m and (m.group(1) or m.group(2)):
            if m.group(1):
                start = int(m.group(1))
                end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
            else:
                start = max(0, size - int(m.group(2)))
            if start > end:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.end_headers()
                return None
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        else:
            self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        return start, end

    def serve(self, head):
        if posixpath.normpath(urllib.parse.urlsplit(self.path).path) == "/game/manifest.txt":
            data = manifest()
            r = self.send_range(len(data), "text/plain")
            if r and not head:
                self.wfile.write(data[r[0]:r[1] + 1])
            return
        path = self.local_path()
        if not path or not os.path.isfile(path):
            return self.send_error(404)
        ctype = "application/wasm" if path.endswith(".wasm") else self.guess_type(path)
        r = self.send_range(os.path.getsize(path), ctype)
        if not r or head:
            return
        with open(path, "rb") as f:
            f.seek(r[0])
            left = r[1] - r[0] + 1
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def do_GET(self):
        self.serve(False)

    def do_HEAD(self):
        self.serve(True)

    def log_message(self, fmt, *args):
        if os.environ.get("SERVE_LOG"):
            super().log_message(fmt, *args)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("Sneak King: http://localhost:%d/" % args.port)
    srv.serve_forever()


if __name__ == "__main__":
    main()
