#!/usr/bin/env python3
"""asm6502.py - tiny two-pass 6502 assembler (stdlib only).

Syntax
  label:            define label (may be followed by an instruction)
  name = expr       constant
  .org expr         set location counter
  .byte e, "str"    bytes (strings as ASCII)
  .word e, ...      little-endian words
  .scr "TEXT"       C64 screen codes (A-Z -> 1-26, @ -> 0, 32-63 unchanged)
  .fill n [, v]     n bytes of v (default 0)
  .include "file"   insert another source file (path relative to cwd)
  ; comment
Expressions: $hex %bin dec 'c', labels, * (location counter), unary < > -,
binary + - * / & | ^ << >>, parentheses.
Operands: #imm  zp/abs  zp,X  abs,X  abs,Y  (zp,X)  (zp),Y  (abs)  A

Modes of use
  rom:       asm6502.py rom SRC.s OUT.bin START END [--header H --name N]
  cputests:  asm6502.py cputests OUT.h SRC1.s SRC2.s ...
"""
import re
import sys

OPS = {
    # mnemonic: {mode: opcode}
    'ADC': dict(imm=0x69, zp=0x65, zpx=0x75, abs=0x6D, absx=0x7D, absy=0x79, indx=0x61, indy=0x71),
    'AND': dict(imm=0x29, zp=0x25, zpx=0x35, abs=0x2D, absx=0x3D, absy=0x39, indx=0x21, indy=0x31),
    'CMP': dict(imm=0xC9, zp=0xC5, zpx=0xD5, abs=0xCD, absx=0xDD, absy=0xD9, indx=0xC1, indy=0xD1),
    'EOR': dict(imm=0x49, zp=0x45, zpx=0x55, abs=0x4D, absx=0x5D, absy=0x59, indx=0x41, indy=0x51),
    'LDA': dict(imm=0xA9, zp=0xA5, zpx=0xB5, abs=0xAD, absx=0xBD, absy=0xB9, indx=0xA1, indy=0xB1),
    'ORA': dict(imm=0x09, zp=0x05, zpx=0x15, abs=0x0D, absx=0x1D, absy=0x19, indx=0x01, indy=0x11),
    'SBC': dict(imm=0xE9, zp=0xE5, zpx=0xF5, abs=0xED, absx=0xFD, absy=0xF9, indx=0xE1, indy=0xF1),
    'STA': dict(zp=0x85, zpx=0x95, abs=0x8D, absx=0x9D, absy=0x99, indx=0x81, indy=0x91),
    'ASL': dict(acc=0x0A, zp=0x06, zpx=0x16, abs=0x0E, absx=0x1E),
    'LSR': dict(acc=0x4A, zp=0x46, zpx=0x56, abs=0x4E, absx=0x5E),
    'ROL': dict(acc=0x2A, zp=0x26, zpx=0x36, abs=0x2E, absx=0x3E),
    'ROR': dict(acc=0x6A, zp=0x66, zpx=0x76, abs=0x6E, absx=0x7E),
    'INC': dict(zp=0xE6, zpx=0xF6, abs=0xEE, absx=0xFE),
    'DEC': dict(zp=0xC6, zpx=0xD6, abs=0xCE, absx=0xDE),
    'BIT': dict(zp=0x24, abs=0x2C),
    'CPX': dict(imm=0xE0, zp=0xE4, abs=0xEC),
    'CPY': dict(imm=0xC0, zp=0xC4, abs=0xCC),
    'LDX': dict(imm=0xA2, zp=0xA6, zpy=0xB6, abs=0xAE, absy=0xBE),
    'LDY': dict(imm=0xA0, zp=0xA4, zpx=0xB4, abs=0xAC, absx=0xBC),
    'STX': dict(zp=0x86, zpy=0x96, abs=0x8E),
    'STY': dict(zp=0x84, zpx=0x94, abs=0x8C),
    'JMP': dict(abs=0x4C, ind=0x6C),
    'JSR': dict(abs=0x20),
}
for m, o in dict(BRK=0x00, CLC=0x18, CLD=0xD8, CLI=0x58, CLV=0xB8, DEX=0xCA, DEY=0x88,
                 INX=0xE8, INY=0xC8, NOP=0xEA, PHA=0x48, PHP=0x08, PLA=0x68, PLP=0x28,
                 RTI=0x40, RTS=0x60, SEC=0x38, SED=0xF8, SEI=0x78, TAX=0xAA, TAY=0xA8,
                 TSX=0xBA, TXA=0x8A, TXS=0x9A, TYA=0x98).items():
    OPS[m] = dict(imp=o)
for m, o in dict(BCC=0x90, BCS=0xB0, BEQ=0xF0, BMI=0x30, BNE=0xD0, BPL=0x10,
                 BVC=0x50, BVS=0x70).items():
    OPS[m] = dict(rel=o)

SIZE = dict(imp=1, acc=1, imm=2, zp=2, zpx=2, zpy=2, rel=2, indx=2, indy=2,
            abs=3, absx=3, absy=3, ind=3)


class AsmError(Exception):
    pass


def screencode(ch):
    c = ord(ch)
    if c == ord('@'):
        return 0
    if ord('A') <= c <= ord('Z'):
        return c - ord('A') + 1
    if ord('a') <= c <= ord('z'):
        return c - ord('a') + 1
    if 32 <= c <= 63:
        return c
    if ch == '[':
        return 27
    if ch == ']':
        return 29
    raise AsmError("no screen code for %r" % ch)


class Expr:
    """recursive-descent expression evaluator; unknown labels -> None"""

    TOK = re.compile(r"\s*(\$[0-9A-Fa-f]+|%[01]+|\d+|'.'|[A-Za-z_][A-Za-z0-9_]*|<<|>>|[-+*/&|^()<>])")

    def __init__(self, text, symbols, pc):
        self.toks = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            m = self.TOK.match(text, pos)
            if not m:
                raise AsmError("bad expression: %r" % text)
            self.toks.append(m.group(1))
            pos = m.end()
        self.i = 0
        self.sym = symbols
        self.pc = pc

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def parse(self):
        v = self.binop(0)
        if self.peek() is not None:
            raise AsmError("trailing tokens in expression: %r" % self.toks)
        return v

    PREC = [('|',), ('^',), ('&',), ('<<', '>>'), ('+', '-'), ('*', '/')]

    def binop(self, level):
        if level == len(self.PREC):
            return self.unary()
        v = self.binop(level + 1)
        while self.peek() in self.PREC[level]:
            op = self.take()
            w = self.binop(level + 1)
            if v is None or w is None:
                v = None
                continue
            v = {'|': lambda a, b: a | b, '^': lambda a, b: a ^ b, '&': lambda a, b: a & b,
                 '<<': lambda a, b: a << b, '>>': lambda a, b: a >> b,
                 '+': lambda a, b: a + b, '-': lambda a, b: a - b,
                 '*': lambda a, b: a * b, '/': lambda a, b: a // b}[op](v, w)
        return v

    def unary(self):
        t = self.peek()
        if t == '<':
            self.take()
            v = self.unary()
            return None if v is None else v & 0xFF
        if t == '>':
            self.take()
            v = self.unary()
            return None if v is None else (v >> 8) & 0xFF
        if t == '-':
            self.take()
            v = self.unary()
            return None if v is None else -v
        return self.atom()

    def atom(self):
        t = self.take()
        if t is None:
            raise AsmError("unexpected end of expression")
        if t == '(':
            v = self.binop(0)
            if self.take() != ')':
                raise AsmError("missing )")
            return v
        if t == '*':
            return self.pc
        if t.startswith('$'):
            return int(t[1:], 16)
        if t.startswith('%'):
            return int(t[1:], 2)
        if t[0].isdigit():
            return int(t)
        if t.startswith("'"):
            return ord(t[1])
        return self.sym.get(t.upper())


def split_args(s):
    """split on commas outside quotes"""
    out, cur, q = [], '', False
    for ch in s:
        if ch == '"':
            q = not q
        if ch == ',' and not q:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [a.strip() for a in out]


def strip_comment(line):
    q = False
    for i, ch in enumerate(line):
        if ch == '"':
            q = not q
        if ch == ';' and not q:
            return line[:i]
    return line


class Assembler:
    def __init__(self):
        self.sym = {}
        self.mem = {}
        self.sizes = {}   # line index -> chosen mode (fixed after pass 1)

    def ev(self, text, pc, final):
        v = Expr(text, self.sym, pc).parse()
        if v is None and final:
            raise AsmError("undefined symbol in %r" % text)
        return v

    def operand_mode(self, mnem, opnd, pc, final, idx):
        ops = OPS[mnem]
        o = opnd.strip()
        if o == '' :
            if 'imp' in ops:
                return 'imp', None
            if 'acc' in ops:
                return 'acc', None
            raise AsmError("%s needs an operand" % mnem)
        if o.upper() == 'A' and 'acc' in ops:
            return 'acc', None
        if o.startswith('#'):
            return 'imm', o[1:]
        if 'rel' in ops:
            return 'rel', o
        m = re.match(r'^\((.*),\s*[xX]\s*\)$', o)
        if m:
            return 'indx', m.group(1)
        m = re.match(r'^\((.*)\)\s*,\s*[yY]$', o)
        if m:
            return 'indy', m.group(1)
        if mnem == 'JMP':
            m = re.match(r'^\((.*)\)$', o)
            if m:
                return 'ind', m.group(1)
        index = ''
        m = re.match(r'^(.*),\s*([xXyY])$', o)
        if m:
            o, index = m.group(1), m.group(2).lower()
        # zero page or absolute?  decided once in pass 1
        if idx in self.sizes:
            return self.sizes[idx], o
        v = self.ev(o, pc, False)
        zp_mode = 'zp' + index
        abs_mode = 'abs' + index
        if v is not None and 0 <= v < 0x100 and zp_mode in ops:
            mode = zp_mode
        elif abs_mode in ops:
            mode = abs_mode
        elif zp_mode in ops:
            mode = zp_mode
        else:
            raise AsmError("bad addressing mode for %s: %s" % (mnem, opnd))
        self.sizes[idx] = mode
        return mode, o

    def run_pass(self, lines, final):
        pc = 0
        for idx, (lineno, raw) in enumerate(lines):
            try:
                line = strip_comment(raw).rstrip()
                if not line.strip():
                    continue
                m = re.match(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$', line)
                if m:
                    v = self.ev(m.group(2), pc, final)
                    if v is not None:
                        self.sym[m.group(1).upper()] = v
                    continue
                m = re.match(r'^\s*([A-Za-z_][A-Za-z0-9_]*):(.*)$', line)
                if m:
                    name = m.group(1).upper()
                    if not final and name in self.sym and self.sym[name] != pc:
                        raise AsmError("duplicate label %s" % name)
                    self.sym[name] = pc
                    line = m.group(2)
                    if not line.strip():
                        continue
                parts = line.strip().split(None, 1)
                word = parts[0]
                rest = parts[1] if len(parts) > 1 else ''
                if word.startswith('.'):
                    d = word.lower()
                    if d == '.org':
                        pc = self.ev(rest, pc, True)
                    elif d == '.byte':
                        for a in split_args(rest):
                            if a.startswith('"'):
                                for ch in a[1:-1]:
                                    self.emit(pc, ord(ch), final)
                                    pc += 1
                            else:
                                v = self.ev(a, pc, final)
                                self.emit(pc, (v or 0) & 0xFF, final)
                                pc += 1
                    elif d == '.word':
                        for a in split_args(rest):
                            v = self.ev(a, pc, final) or 0
                            self.emit(pc, v & 0xFF, final)
                            self.emit(pc + 1, (v >> 8) & 0xFF, final)
                            pc += 2
                    elif d == '.scr':
                        s = rest.strip()
                        if not (s.startswith('"') and s.endswith('"')):
                            raise AsmError(".scr needs a string")
                        for ch in s[1:-1]:
                            self.emit(pc, screencode(ch), final)
                            pc += 1
                    elif d == '.fill':
                        a = split_args(rest)
                        n = self.ev(a[0], pc, True)
                        v = self.ev(a[1], pc, final) if len(a) > 1 else 0
                        for i in range(n):
                            self.emit(pc + i, (v or 0) & 0xFF, final)
                        pc += n
                    else:
                        raise AsmError("unknown directive %s" % word)
                    continue
                mnem = word.upper()
                if mnem not in OPS:
                    raise AsmError("unknown mnemonic %s" % word)
                mode, expr = self.operand_mode(mnem, rest, pc, final, idx)
                op = OPS[mnem][mode]
                size = SIZE[mode]
                self.emit(pc, op, final)
                if size > 1:
                    v = self.ev(expr, pc, final)
                    if final:
                        if mode == 'rel':
                            off = v - (pc + 2)
                            if not -128 <= off <= 127:
                                raise AsmError("branch out of range (%d)" % off)
                            v = off & 0xFF
                        elif size == 2 and not 0 <= v <= 0xFF and mode != 'imm':
                            raise AsmError("operand out of zero page range: %s" % expr)
                        elif mode == 'imm' and not -128 <= v <= 0xFF:
                            raise AsmError("immediate out of range: %s" % expr)
                        self.emit(pc + 1, v & 0xFF, final)
                        if size == 3:
                            self.emit(pc + 2, (v >> 8) & 0xFF, final)
                pc += size
            except AsmError as e:
                raise AsmError("line %d: %s\n    %s" % (lineno, e, raw.rstrip()))

    def emit(self, addr, val, final):
        if final:
            if addr in self.mem:
                raise AsmError("overlapping output at $%04X" % addr)
            self.mem[addr] = val

    def assemble(self, text):
        lines = list(enumerate(text.splitlines(), 1))
        self.run_pass(lines, False)
        self.run_pass(lines, True)
        return self.mem


def read_source(path, depth=0):
    """read a source file, expanding  .include "file"  (relative to cwd)"""
    if depth > 8:
        raise AsmError("include nesting too deep")
    out = []
    with open(path) as f:
        for line in f.read().splitlines():
            m = re.match(r'^\s*\.include\s+"([^"]+)"', line, re.I)
            if m:
                out.append(read_source(m.group(1), depth + 1))
            else:
                out.append(line)
    return "\n".join(out)


def assemble_file(path):
    text = read_source(path)
    a = Assembler()
    a.assemble(text)
    return a, text


def c_array(name, data):
    out = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 16):
        out.append("  " + ",".join("0x%02X" % b for b in data[i:i + 16]) + ",")
    out.append("};")
    return "\n".join(out)


def cmd_rom(argv):
    src, out, start, end = argv[0], argv[1], int(argv[2], 16), int(argv[3], 16)
    header = name = None
    if '--header' in argv:
        header = argv[argv.index('--header') + 1]
        name = argv[argv.index('--name') + 1]
    a, _ = assemble_file(src)
    data = bytearray([0xFF] * (end - start))
    for addr, v in a.mem.items():
        if not start <= addr < end:
            raise SystemExit("byte at $%04X outside ROM range" % addr)
        data[addr - start] = v
    with open(out, "wb") as f:
        f.write(data)
    if header:
        with open(header, "w") as f:
            f.write("/* GENERATED by tools/asm6502.py from %s - do not edit */\n#pragma once\n#include <stdint.h>\n" % src)
            f.write(c_array(name, data) + "\n")
    used = len(a.mem)
    print("%s: %d bytes used of %d, symbols: %s" % (src, used, end - start,
          ", ".join("%s=$%04X" % (k, v) for k, v in sorted(a.sym.items(), key=lambda kv: kv[1])
                    if k in ('RESET', 'IRQ', 'NMI', 'MAIN', 'FONT'))))


def cmd_cputests(argv):
    out, srcs = argv[0], argv[1:]
    tests = []
    for src in srcs:
        a, text = assemble_file(src)
        name = src
        checks = []
        for line in text.splitlines():
            m = re.match(r'^\s*;!\s*(.*)$', line)
            if not m:
                continue
            spec = m.group(1).strip()
            if spec.lower().startswith('name:'):
                name = spec[5:].strip()
                continue
            mm = re.match(r'^mem\s+(\S+)\s*=\s*(\S+)$', spec, re.I)
            if mm:
                checks.append(('CHK_MEM', a.ev(mm.group(1), 0, True), a.ev(mm.group(2), 0, True), 0))
                continue
            mm = re.match(r'^(a|x|y|s|p)\s*=\s*(\S+)$', spec, re.I)
            if mm:
                checks.append(('CHK_' + mm.group(1).upper(), 0, a.ev(mm.group(2), 0, True), 0))
                continue
            mm = re.match(r'^cycles\s+(\S+)\s+(\S+)\s*=\s*(\S+)$', spec, re.I)
            if mm:
                checks.append(('CHK_CYCLES', a.ev(mm.group(1), 0, True), a.ev(mm.group(3), 0, True),
                               a.ev(mm.group(2), 0, True)))
                continue
            raise SystemExit("%s: bad check spec %r" % (src, spec))
        lo, hi = min(a.mem), max(a.mem) + 1
        img = bytes(a.mem.get(i, 0) for i in range(lo, hi))
        tests.append(dict(name=name, src=src, lo=lo, img=img, start=a.sym['START'],
                          done=a.sym['DONE'], checks=checks))
    with open(out, "w") as f:
        f.write("/* GENERATED by tools/asm6502.py cputests - do not edit */\n#pragma once\n#include <stdint.h>\n")
        f.write("enum { CHK_MEM, CHK_A, CHK_X, CHK_Y, CHK_S, CHK_P, CHK_CYCLES };\n")
        f.write("typedef struct { int kind; uint16_t addr; uint16_t value; uint16_t addr2; } cpucheck_t;\n")
        f.write("typedef struct { const char *name; const char *src; uint16_t org; const uint8_t *img; int len;\n"
                "                 uint16_t start, done; const cpucheck_t *checks; int nchecks; } cputest_t;\n")
        for i, t in enumerate(tests):
            f.write(c_array("cputest_img_%d" % i, t['img']) + "\n")
            f.write("static const cpucheck_t cputest_chk_%d[] = {\n" % i)
            for c in t['checks']:
                f.write("  {%s, 0x%04X, 0x%04X, 0x%04X},\n" % (c[0], c[1] & 0xFFFF, c[2] & 0xFFFF, c[3] & 0xFFFF))
            f.write("};\n")
        f.write("static const cputest_t cputests[] = {\n")
        for i, t in enumerate(tests):
            f.write('  {"%s", "%s", 0x%04X, cputest_img_%d, %d, 0x%04X, 0x%04X, cputest_chk_%d, %d},\n'
                    % (t['name'], t['src'], t['lo'], i, len(t['img']), t['start'], t['done'], i, len(t['checks'])))
        f.write("};\n#define NUM_CPUTESTS %d\n" % len(tests))
    print("cputests: %d programs, %d checks" % (len(tests), sum(len(t['checks']) for t in tests)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    try:
        if sys.argv[1] == 'rom':
            cmd_rom(sys.argv[2:])
        elif sys.argv[1] == 'cputests':
            cmd_cputests(sys.argv[2:])
        else:
            sys.exit(__doc__)
    except AsmError as e:
        sys.exit("asm error: %s" % e)


if __name__ == '__main__':
    main()
