"""Rewrite a lifted function's memory accesses into explicit MMIO calls.

Hardware registers are normally reached by trapping the page they live on:
the access faults, the fault handler decodes the host instruction and hands
it to the device model. A host with no page faults (WebAssembly) cannot do
that, so for the functions that touch registers -- in practice the XDK's
D3D, DirectSound and USB code -- every access is made a call instead:

    MEM32(a) = v;      ->  MMIO_W32(a, v);
    x = MEM16(a);      ->  x = MMIO_R16(a);
    SMEM8(a)           ->  ((int8_t)MMIO_R8(a))
    MEMF(a) = f;       ->  MMIO_WF(a, f);

MMIO_R*/MMIO_W* (recomp_types.h) take the plain-memory path inline and call
the device router only for addresses in the register apertures, so the cost
outside a register access is one compare.

The lifter emits stores only as `MEMn(addr) = expr` statements -- no
compound assignment, no increment, no address-of -- which is what makes a
textual rewrite sound. Anything else raises rather than slipping through
unrewritten: a store left as MEMn would bypass the device model silently.
"""

import re

_ACCESS = re.compile(r'(?<![A-Za-z0-9_])(S?)MEM(8|16|32|64|F|D)\(')
_SIGNED = {"8": "int8_t", "16": "int16_t", "32": "int32_t", "64": "int64_t"}


class MmioRewriteError(ValueError):
    pass


def _close_paren(s, i):
    """Index of the ')' matching the '(' just before s[i]."""
    depth = 1
    while i < len(s):
        c = s[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        elif c in '"\'':
            i = s.index(c, i + 1)
        i += 1
    raise MmioRewriteError("unbalanced parentheses: %r" % s)


def _expr_end(s, i):
    """End of an expression starting at s[i]: the first ';' or ',' at depth 0,
    or an unmatched ')'."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in '([':
            depth += 1
        elif c in ')]':
            if depth == 0:
                return i
            depth -= 1
        elif c in ';,' and depth == 0:
            return i
        elif c in '"\'':
            i = s.index(c, i + 1)
        i += 1
    return i


def _is_store(s, j):
    """s[j:] follows a MEM access: True for a plain '=' assignment."""
    rest = s[j:].lstrip()
    if rest.startswith('=') and not rest.startswith('=='):
        return True
    for op in ('+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=',
               '++', '--'):
        if rest.startswith(op):
            raise MmioRewriteError("compound store not supported: %r" % s)
    return False


def rewrite(s):
    """Rewrite every MEM access in one line (or statement) of lifted C."""
    out = []
    pos = 0
    while True:
        m = _ACCESS.search(s, pos)
        if not m:
            out.append(s[pos:])
            return ''.join(out)
        signed, width = m.group(1), m.group(2)
        if signed and width in ('F', 'D'):
            raise MmioRewriteError("signed float access: %r" % s)
        prev = s[:m.start()].rstrip()
        if prev.endswith('&') and not prev.endswith('&&'):
            # Unary (address-of) unless it follows an operand: `x & MEM8(a)`.
            before = prev[:-1].rstrip()
            if not before or not (before[-1].isalnum() or before[-1] in '_)]'):
                raise MmioRewriteError("address of a MEM access: %r" % s)
        close = _close_paren(s, m.end())
        addr = rewrite(s[m.end():close])
        out.append(s[pos:m.start()])
        if _is_store(s, close + 1):
            # A signed store (fistp) is the same bits stored unsigned.
            eq = s.index('=', close + 1)
            end = _expr_end(s, eq + 1)
            value = rewrite(s[eq + 1:end]).strip()
            out.append("MMIO_W%s(%s, %s)" % (width, addr, value))
            pos = end
        else:
            call = "MMIO_R%s(%s)" % (width, addr)
            if signed:
                call = "((%s)%s)" % (_SIGNED[width], call)
            out.append(call)
            pos = close + 1


def rewrite_lines(lines):
    return [rewrite(line) for line in lines]
