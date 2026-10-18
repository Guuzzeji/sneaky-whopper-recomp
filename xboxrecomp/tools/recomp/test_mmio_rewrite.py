import pytest

from tools.recomp.mmio_rewrite import MmioRewriteError, rewrite


@pytest.mark.parametrize("src, want", [
    ("MEM32(ecx + 4) = eax;", "MMIO_W32(ecx + 4, eax);"),
    ("eax = MEM32(esi);", "eax = MMIO_R32(esi);"),
    ("MEM8(edi) = MEM8(esi); esi += 1;", "MMIO_W8(edi, MMIO_R8(esi)); esi += 1;"),
    ("eax = (uint32_t)SMEM16(ebx);", "eax = (uint32_t)((int16_t)MMIO_R16(ebx));"),
    ("MEMF(esp + 8) = (float)fp_top();", "MMIO_WF(esp + 8, (float)fp_top());"),
    ("if (MEM32(MEM32(eax) + 4) == 0) goto L;",
     "if (MMIO_R32(MMIO_R32(eax) + 4) == 0) goto L;"),
    ("MEM16(f(a, b)) = g(c, d);", "MMIO_W16(f(a, b), g(c, d));"),
    ("if (x) MEM32(eax) = 1; else y = 2;", "if (x) MMIO_W32(eax, 1); else y = 2;"),
    ("ok = a && MEM32(eax);", "ok = a && MMIO_R32(eax);"),
    ("XMEM32(eax) = 1;", "XMEM32(eax) = 1;"),
    ("SET_HI8(eax, HI8(eax) & MEM8(edx));", "SET_HI8(eax, HI8(eax) & MMIO_R8(edx));"),
    ("x = y & MEM32(eax);", "x = y & MMIO_R32(eax);"),
    ("SMEM32(ebp + -8) = (int32_t)v; fp_pop();", "MMIO_W32(ebp + -8, (int32_t)v); fp_pop();"),
])
def test_rewrite(src, want):
    assert rewrite(src) == want


@pytest.mark.parametrize("src", [
    "MEM32(eax) += 1;",
    "MEM32(eax)++;",
    "p = &MEM32(eax);",
])
def test_rejects_unsupported(src):
    with pytest.raises(MmioRewriteError):
        rewrite(src)
