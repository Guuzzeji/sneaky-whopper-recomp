"""Self-checks for sneakking.py: python tools/test_sneakking.py (or pytest)."""
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sneakking import ROOT, apply_settings, fresh_stamp, read_settings  # noqa: E402


def test_settings_roundtrip():
    template = (ROOT / "settings.ini").read_text(encoding="utf-8")
    base = read_settings(template)
    assert base["GFX"] == "d3d11" and base["WIDESCREEN"] == "1"
    text = apply_settings(template, {"GFX": "vulkan", "SCALE": "3", "NEW": "x"})
    got = read_settings(text)
    assert got["GFX"] == "vulkan" and got["SCALE"] == "3" and got["NEW"] == "x"
    assert got["MOUSE_DEG"] == base["MOUSE_DEG"]                 # untouched
    assert text.count("# Graphics back end") == 1                # comments kept
    assert read_settings("# A=1\n[x]\n B = 2 \nC=\n") == {"B": "2", "C": ""}


def test_fresh_stamp():
    with tempfile.TemporaryDirectory() as d:
        inp, stamp = Path(d, "in"), Path(d, "stamp")
        inp.touch()
        assert not fresh_stamp(stamp, [inp])                     # never ran
        stamp.touch()
        os.utime(inp, (1, 1))
        assert fresh_stamp(stamp, [inp])
        os.utime(inp, None)
        os.utime(stamp, (1, 1))
        assert not fresh_stamp(stamp, [inp])                     # input changed
        assert not fresh_stamp(Path(d, "in"), [Path(d, "gone")])  # input missing


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("ok")
