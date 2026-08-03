#!/usr/bin/env python3
"""A small CHIP-8 assembler.

Exists so the bundled ROMs can be original work kept in readable source form
rather than opaque binaries copied from elsewhere. It covers the standard
instruction set, labels, and a DB directive for sprite data.

    python tools/assemble.py roms/src/bounce.asm roms/bounce.ch8

Syntax is one instruction per line, ';' starts a comment, and a label is a
bare name followed by ':'. Numbers are decimal, or hex with a 0x or # prefix.
Registers are V0 through VF, case-insensitive.
"""

from __future__ import annotations

import re
import sys

PROGRAM_START = 0x200
MAX_ROM = 4096 - PROGRAM_START


class AsmError(Exception):
    def __init__(self, line_no: int, message: str) -> None:
        super().__init__(f"line {line_no}: {message}")


def parse_number(token: str, line_no: int) -> int:
    """Accepts 0x1F, #1F, $1F, 0b1010 or plain decimal."""
    t = token.strip()
    try:
        if t.lower().startswith("0x") or t.startswith("#") or t.startswith("$"):
            return int(t.lstrip("#$").replace("0x", "").replace("0X", ""), 16)
        if t.lower().startswith("0b"):
            return int(t[2:], 2)
        return int(t, 10)
    except ValueError:
        raise AsmError(line_no, f"not a number: {token!r}") from None


def parse_register(token: str, line_no: int) -> int:
    t = token.strip().upper()
    if not re.fullmatch(r"V[0-9A-F]", t):
        raise AsmError(line_no, f"not a register: {token!r}")
    return int(t[1], 16)


def is_register(token: str) -> bool:
    return bool(re.fullmatch(r"[Vv][0-9A-Fa-f]"
                             , token.strip()))


class Assembler:
    def __init__(self) -> None:
        self.labels: dict[str, int] = {}
        # (address, line_no, mnemonic, operands) held over for the second pass,
        # because a label may be referenced before it is defined.
        self.pending: list[tuple[int, int, str, list[str]]] = []
        self.words: dict[int, int] = {}
        self.address = PROGRAM_START

    # -- pass 1: record labels and reserve space ---------------------------
    def collect(self, source: str) -> None:
        for line_no, raw in enumerate(source.splitlines(), start=1):
            line = raw.split(";")[0].strip()
            if not line:
                continue

            while ":" in line:
                label, _, rest = line.partition(":")
                label = label.strip()
                if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", label):
                    raise AsmError(line_no, f"bad label {label!r}")
                if label in self.labels:
                    raise AsmError(line_no, f"duplicate label {label!r}")
                self.labels[label] = self.address
                line = rest.strip()
            if not line:
                continue

            parts = re.split(r"[\s,]+", line)
            mnemonic = parts[0].upper()
            operands = [p for p in parts[1:] if p]

            if mnemonic == "DB":
                # Raw bytes, padded to a whole word so the next instruction
                # stays two-byte aligned.
                values = [parse_number(op, line_no) & 0xFF for op in operands]
                if not values:
                    raise AsmError(line_no, "DB needs at least one byte")
                if len(values) % 2:
                    values.append(0)
                for i in range(0, len(values), 2):
                    self.words[self.address] = (values[i] << 8) | values[i + 1]
                    self.address += 2
                continue

            self.pending.append((self.address, line_no, mnemonic, operands))
            self.address += 2

            if self.address > PROGRAM_START + MAX_ROM:
                raise AsmError(line_no, "program overflows 4K of memory")

    # -- pass 2: encode, now that every label address is known -------------
    def encode_all(self) -> None:
        for address, line_no, mnemonic, operands in self.pending:
            self.words[address] = self.encode(line_no, mnemonic, operands)

    def value(self, token: str, line_no: int) -> int:
        name = token.strip()
        if name in self.labels:
            return self.labels[name]
        return parse_number(name, line_no)

    def encode(self, line_no: int, m: str, ops: list[str]) -> int:
        def need(n: int) -> None:
            if len(ops) != n:
                raise AsmError(line_no, f"{m} takes {n} operand(s), got {len(ops)}")

        if m == "CLS":
            need(0)
            return 0x00E0
        if m == "RET":
            need(0)
            return 0x00EE
        if m == "JP":
            if len(ops) == 1:
                return 0x1000 | (self.value(ops[0], line_no) & 0x0FFF)
            if len(ops) == 2 and ops[0].upper() == "V0":
                return 0xB000 | (self.value(ops[1], line_no) & 0x0FFF)
            raise AsmError(line_no, "JP addr  or  JP V0, addr")
        if m == "CALL":
            need(1)
            return 0x2000 | (self.value(ops[0], line_no) & 0x0FFF)
        if m == "SE":
            need(2)
            x = parse_register(ops[0], line_no)
            if is_register(ops[1]):
                return 0x5000 | (x << 8) | (parse_register(ops[1], line_no) << 4)
            return 0x3000 | (x << 8) | (self.value(ops[1], line_no) & 0xFF)
        if m == "SNE":
            need(2)
            x = parse_register(ops[0], line_no)
            if is_register(ops[1]):
                return 0x9000 | (x << 8) | (parse_register(ops[1], line_no) << 4)
            return 0x4000 | (x << 8) | (self.value(ops[1], line_no) & 0xFF)
        if m == "LD":
            need(2)
            dst, src = ops[0].upper(), ops[1].upper()
            if dst == "I":
                return 0xA000 | (self.value(ops[1], line_no) & 0x0FFF)
            if dst == "DT":
                return 0xF015 | (parse_register(ops[1], line_no) << 8)
            if dst == "ST":
                return 0xF018 | (parse_register(ops[1], line_no) << 8)
            if dst == "F":
                return 0xF029 | (parse_register(ops[1], line_no) << 8)
            if dst == "B":
                return 0xF033 | (parse_register(ops[1], line_no) << 8)
            if dst == "[I]":
                return 0xF055 | (parse_register(ops[1], line_no) << 8)
            x = parse_register(ops[0], line_no)
            if src == "DT":
                return 0xF007 | (x << 8)
            if src == "K":
                return 0xF00A | (x << 8)
            if src == "[I]":
                return 0xF065 | (x << 8)
            if is_register(ops[1]):
                return 0x8000 | (x << 8) | (parse_register(ops[1], line_no) << 4)
            return 0x6000 | (x << 8) | (self.value(ops[1], line_no) & 0xFF)
        if m == "ADD":
            need(2)
            if ops[0].upper() == "I":
                return 0xF01E | (parse_register(ops[1], line_no) << 8)
            x = parse_register(ops[0], line_no)
            if is_register(ops[1]):
                return 0x8004 | (x << 8) | (parse_register(ops[1], line_no) << 4)
            return 0x7000 | (x << 8) | (self.value(ops[1], line_no) & 0xFF)

        two_reg = {"OR": 0x8001, "AND": 0x8002, "XOR": 0x8003,
                   "SUB": 0x8005, "SHR": 0x8006, "SUBN": 0x8007, "SHL": 0x800E}
        if m in two_reg:
            # SHR/SHL take an optional second register; it matters only under
            # the COSMAC shift quirk, and defaults to Vx when omitted.
            if len(ops) == 1 and m in ("SHR", "SHL"):
                ops = [ops[0], ops[0]]
            need(2)
            return (two_reg[m] | (parse_register(ops[0], line_no) << 8)
                    | (parse_register(ops[1], line_no) << 4))

        if m == "RND":
            need(2)
            return (0xC000 | (parse_register(ops[0], line_no) << 8)
                    | (self.value(ops[1], line_no) & 0xFF))
        if m == "DRW":
            need(3)
            return (0xD000 | (parse_register(ops[0], line_no) << 8)
                    | (parse_register(ops[1], line_no) << 4)
                    | (self.value(ops[2], line_no) & 0xF))
        if m == "SKP":
            need(1)
            return 0xE09E | (parse_register(ops[0], line_no) << 8)
        if m == "SKNP":
            need(1)
            return 0xE0A1 | (parse_register(ops[0], line_no) << 8)

        raise AsmError(line_no, f"unknown instruction {m!r}")

    def output(self) -> bytes:
        if not self.words:
            return b""
        end = max(self.words) + 2
        rom = bytearray(end - PROGRAM_START)
        for address, word in self.words.items():
            offset = address - PROGRAM_START
            rom[offset] = (word >> 8) & 0xFF
            rom[offset + 1] = word & 0xFF
        return bytes(rom)


def assemble(source: str) -> bytes:
    asm = Assembler()
    asm.collect(source)
    asm.encode_all()
    return asm.output()


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2

    with open(argv[1], "r", encoding="utf-8") as handle:
        source = handle.read()

    try:
        rom = assemble(source)
    except AsmError as error:
        print(f"{argv[1]}: {error}", file=sys.stderr)
        return 1

    with open(argv[2], "wb") as handle:
        handle.write(rom)

    print(f"{argv[1]} -> {argv[2]}  ({len(rom)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
