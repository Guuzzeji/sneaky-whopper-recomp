"""cpuid, pushfd/popfd, cli/sti and wbinvd lift to code, not RECOMP_UNIMPL.

The CRT, XGRAPHICS and WMADEC probe the CPU before choosing an SSE path. With
these unlifted, cpuid left stale registers and the ID-bit probe's pushfd/popfd
pair skipped its push and pop, so esp was 4 bytes off after each one.
"""
import unittest

from .disasm import BasicBlock, Instruction
from .lifter import Lifter, lift_basic_block


def _lift(*mnemonics):
    insns = []
    for off, m in enumerate(mnemonics):
        i = Instruction(off, 1, m, "", "90")
        i.operands = []
        insns.append(i)
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=insns))
    return "\n".join(lifted)


class CpuProbeTests(unittest.TestCase):
    def test_none_unimplemented(self):
        out = _lift("cpuid", "pushfd", "popfd", "cli", "sti", "wbinvd")
        self.assertNotIn("RECOMP_UNIMPL", out)

    def test_cpuid_reports_sse_not_sse2(self):
        out = _lift("cpuid")
        self.assertIn("edx = 0x0383F9FFu", out)  # bit 25 SSE, bit 26 clear
        self.assertEqual(0x0383F9FF >> 25 & 3, 1)

    def test_pushfd_popfd_balance_the_stack(self):
        out = _lift("pushfd", "popfd")
        self.assertIn("PUSH32(esp,", out)
        self.assertIn("POP32(esp,", out)


if __name__ == "__main__":
    unittest.main()
