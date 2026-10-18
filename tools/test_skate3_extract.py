import struct

import numpy as np

from skate3_extract import RATE, loopable, splc_streams


def test_splc_streams_finds_each_snr():
    snr = lambda n: b"\x03\x00\xbb\x80" + struct.pack(">III", 1000, 8 + n, 1000) + bytes(n)  # noqa: E731
    bank = b"SPLC" + bytes(60) + snr(16) + snr(40)
    got = splc_streams(bank)
    assert [len(s) for s in got] == [8 + 8 + 16, 8 + 8 + 40]


def test_loopable_joins_tail_to_head():
    x = np.arange(RATE, dtype=np.float32)
    y = loopable(x, 0.1)
    n = int(0.1 * RATE)
    assert len(y) == len(x) - n
    assert y[0] == x[-n]           # starts where the tail left off
