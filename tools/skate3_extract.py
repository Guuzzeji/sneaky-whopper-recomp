"""Pull skate mode's sounds out of the user's Skate 3 disc into game/mods/skate3/sounds.

    .venv\\Scripts\\python.exe tools\\skate3_extract.py [skate3] [-o game\\mods\\skate3\\sounds]

The board's sounds are EA-XMA streams in AEMS banks (`.abk`, "ABKC") and in
a splice bank (`.bnk`, "SPLC") of bare SNR+SNS streams. vgmstream-cli decodes
both: pass --vgmstream, or have it on PATH (vgmstream.org, Windows build).
Each pick is written as 16-bit mono WAV at 22050 Hz, the rate skate.c plays.

The banks carry no names, so the picks below were chosen by listening to
their shape (length, spectrum); docs/SKATE3_NOTES.md lists what each bank holds.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import skate3_big  # noqa: E402

RATE = 22050
AUDIO = "data/audio/audiofiles.big"

# out name -> (bank, stream index from 1, seconds to keep (0 = all), loop crossfade s)
PICKS = {
    "roll":    ("PatchBank_Rolling_Surfaces.abk", 3, 4.0, 0.15),
    "land1":   ("PatchBank_Rolling_Surfaces.abk", 8, 0, 0),
    "land2":   ("PatchBank_Rolling_Surfaces.abk", 9, 0, 0),
    "land3":   ("PatchBank_Rolling_Surfaces.abk", 11, 0, 0),
    "pop1":    ("Skate_Collisions.bnk", 1, 0, 0),
    "pop2":    ("Skate_Collisions.bnk", 2, 0, 0),
    "pop3":    ("Skate_Collisions.bnk", 3, 0, 0),
    "flip1":   ("Sk8_Air_Flip_Tricks.abk", 1, 0, 0),
    "flip2":   ("Sk8_Air_Flip_Tricks.abk", 2, 0, 0),
    "flip3":   ("Sk8_Air_Flip_Tricks.abk", 3, 0, 0),
    "flip4":   ("Sk8_Air_Flip_Tricks.abk", 4, 0, 0),
    "scrape1": ("board_scrapes.abk", 1, 0, 0),
    "scrape2": ("board_scrapes.abk", 5, 0, 0),
    "grind":   ("GRINDS.abk", 1, 2.0, 0.15),
}


def splc_streams(data):
    """The SNR+SNS streams in a SPLC bank, in file order: an SNR header
    (codec 3 EA-XMA, 48 kHz) followed by its one SNS block."""
    out = []
    for m in re.finditer(rb"\x03[\x00-\x0f]\xbb\x80", data):
        h = m.start()
        if h + 12 > len(data):
            continue
        samples = struct.unpack_from(">I", data, h + 4)[0] & 0x1FFFFFFF
        block = struct.unpack_from(">I", data, h + 8)[0]
        if block >> 24 in (0, 0x80) and (block & 0xFFFFFF) < 0x100000 and 0 < samples < 48000 * 30:
            out.append(data[h:h + 8 + (block & 0xFFFFFF)])
    return out


def decode(vgm, path, index, tmp):
    wav = os.path.join(tmp, "o.wav")
    subprocess.run([vgm, "-s", str(index), "-o", wav, path], check=True, capture_output=True)
    with wave.open(wav) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32)
        x = x.reshape(-1, w.getnchannels()).mean(1)
        return x, w.getframerate()


def resample(x, sr):
    if sr == RATE:
        return x
    k = max(1, int(round(sr / RATE)))                # box filter before decimating
    x = np.convolve(x, np.ones(k) / k, mode="same")
    t = np.arange(int(len(x) * RATE / sr)) * (sr / RATE)
    return np.interp(t, np.arange(len(x)), x)


def loopable(x, fade):
    """Cross-fade the tail into the head so the loop has no click."""
    n = int(fade * RATE)
    if n <= 0 or len(x) < 3 * n:
        return x
    w = np.linspace(0, 1, n)
    head = x[:n] * w + x[-n:] * (1 - w)
    return np.concatenate([head, x[n:-n]])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("skate3", nargs="?", default="skate3")
    ap.add_argument("-o", "--out", default=os.path.join("game", "mods", "skate3", "sounds"))
    ap.add_argument("--vgmstream", default=shutil.which("vgmstream-cli")
                    or r"C:\repos\_research\vgmstream\vgmstream-cli.exe")
    a = ap.parse_args()
    if not os.path.isfile(a.vgmstream):
        sys.exit("vgmstream-cli not found: pass --vgmstream")
    big = os.path.join(a.skate3, AUDIO)
    banks = {b for b, *_ in PICKS.values()}
    os.makedirs(a.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        files = {}
        for name, off, packed, size in skate3_big.entries(big):
            base = name.rsplit("/", 1)[-1]
            if base in banks:
                files[base] = os.path.join(tmp, base)
                open(files[base], "wb").write(skate3_big.read(big, off, packed, size))
        for out, (bank, idx, keep, fade) in PICKS.items():
            path, sub = files[bank], idx
            if bank.endswith(".bnk"):              # one stream per file for vgmstream
                path, sub = os.path.join(tmp, "s.snr"), 1
                open(path, "wb").write(splc_streams(open(files[bank], "rb").read())[idx - 1])
            x, sr = decode(a.vgmstream, path, sub, tmp)
            x = resample(x, sr)
            if keep:
                x = x[:int(keep * RATE)]
            x = loopable(x, fade)
            x = x / max(np.abs(x).max(), 1.0) * 32000.0 if np.abs(x).max() > 32000 else x
            with wave.open(os.path.join(a.out, out + ".wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(x.astype(np.int16).tobytes())
            print(f"{out:8} {len(x) / RATE:5.2f} s  <- {bank} #{idx}")


if __name__ == "__main__":
    main()
