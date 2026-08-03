#!/usr/bin/env python3
"""A reference CHIP-8 interpreter, used to test ROMs without a compiler.

`tools/render_rom.cpp` is the real smoke test, but it needs a native build, and
the WebAssembly build cannot open files off the host filesystem. This runs a
ROM in pure Python instead, so a game can be checked from a clean machine.

It is a reference, not a second implementation to maintain: it exists to answer
"does this ROM do the right thing", not to be fast or complete.

    python tools/simulate.py roms/brix.ch8 --frames 600
    python tools/simulate.py roms/brix.ch8 --hold 9:0-400   # hold keypad 9

`--hold KEY:START-END` presses a keypad key between two frame numbers, which is
how an input-driven ROM gets exercised without a human at the keyboard.
"""

from __future__ import annotations

import argparse
import random

W, H = 64, 32
FONT = [
    0xF0, 0x90, 0x90, 0x90, 0xF0, 0x20, 0x60, 0x20, 0x20, 0x70,
    0xF0, 0x10, 0xF0, 0x80, 0xF0, 0xF0, 0x10, 0xF0, 0x10, 0xF0,
    0x90, 0x90, 0xF0, 0x10, 0x10, 0xF0, 0x80, 0xF0, 0x10, 0xF0,
    0xF0, 0x80, 0xF0, 0x90, 0xF0, 0xF0, 0x10, 0x20, 0x40, 0x40,
    0xF0, 0x90, 0xF0, 0x90, 0xF0, 0xF0, 0x90, 0xF0, 0x10, 0xF0,
    0xF0, 0x90, 0xF0, 0x90, 0x90, 0xE0, 0x90, 0xE0, 0x90, 0xE0,
    0xF0, 0x80, 0x80, 0x80, 0xF0, 0xE0, 0x90, 0x90, 0x90, 0xE0,
    0xF0, 0x80, 0xF0, 0x80, 0xF0, 0xF0, 0x80, 0xF0, 0x80, 0x80,
]


class Chip8:
    def __init__(self, rom: bytes, seed: int = 1) -> None:
        self.mem = bytearray(4096)
        self.mem[0x50:0x50 + len(FONT)] = bytes(FONT)
        self.mem[0x200:0x200 + len(rom)] = rom
        self.v = bytearray(16)
        self.i = 0
        self.pc = 0x200
        self.stack: list[int] = []
        self.dt = 0
        self.st = 0
        self.display = bytearray(W * H)
        self.keys = [False] * 16
        self.rng = random.Random(seed)
        self.halted = False
        self.waiting_for_key = False
        self.display_wait = True
        self.drew = False

    def step(self) -> None:
        if self.halted or self.pc + 1 >= 4096:
            self.halted = True
            return
        op = (self.mem[self.pc] << 8) | self.mem[self.pc + 1]
        self.pc += 2
        nnn, n = op & 0x0FFF, op & 0x000F
        x, y = (op >> 8) & 0xF, (op >> 4) & 0xF
        kk = op & 0x00FF
        v = self.v
        top = op >> 12

        if op == 0x00E0:
            self.display = bytearray(W * H)
        elif op == 0x00EE:
            if not self.stack:
                self.halted = True
            else:
                self.pc = self.stack.pop()
        elif top == 0x1:
            if nnn == self.pc - 2:
                self.halted = True  # a tight self-jump is a hang
            self.pc = nnn
        elif top == 0x2:
            self.stack.append(self.pc)
            self.pc = nnn
        elif top == 0x3:
            self.pc += 2 if v[x] == kk else 0
        elif top == 0x4:
            self.pc += 2 if v[x] != kk else 0
        elif top == 0x5:
            self.pc += 2 if v[x] == v[y] else 0
        elif top == 0x6:
            v[x] = kk
        elif top == 0x7:
            v[x] = (v[x] + kk) & 0xFF
        elif top == 0x8:
            if n == 0x0:
                v[x] = v[y]
            elif n == 0x1:
                v[x] |= v[y]
            elif n == 0x2:
                v[x] &= v[y]
            elif n == 0x3:
                v[x] ^= v[y]
            elif n == 0x4:
                total = v[x] + v[y]
                v[x] = total & 0xFF
                v[0xF] = 1 if total > 0xFF else 0
            elif n == 0x5:
                flag = 1 if v[x] >= v[y] else 0
                v[x] = (v[x] - v[y]) & 0xFF
                v[0xF] = flag
            elif n == 0x6:
                flag = v[x] & 1
                v[x] >>= 1
                v[0xF] = flag
            elif n == 0x7:
                flag = 1 if v[y] >= v[x] else 0
                v[x] = (v[y] - v[x]) & 0xFF
                v[0xF] = flag
            elif n == 0xE:
                flag = (v[x] >> 7) & 1
                v[x] = (v[x] << 1) & 0xFF
                v[0xF] = flag
        elif top == 0x9:
            self.pc += 2 if v[x] != v[y] else 0
        elif top == 0xA:
            self.i = nnn
        elif top == 0xB:
            self.pc = (nnn + v[0]) & 0xFFF
        elif top == 0xC:
            v[x] = self.rng.randrange(256) & kk
        elif top == 0xD:
            self.draw(v[x], v[y], n)
        elif top == 0xE:
            pressed = self.keys[v[x] & 0xF]
            if kk == 0x9E:
                self.pc += 2 if pressed else 0
            elif kk == 0xA1:
                self.pc += 2 if not pressed else 0
        elif top == 0xF:
            if kk == 0x07:
                v[x] = self.dt
            elif kk == 0x0A:
                held = [k for k, down in enumerate(self.keys) if down]
                if held:
                    v[x] = held[0]
                    self.waiting_for_key = False
                else:
                    self.pc -= 2  # block here until something is pressed
                    self.waiting_for_key = True
            elif kk == 0x15:
                self.dt = v[x]
            elif kk == 0x18:
                self.st = v[x]
            elif kk == 0x1E:
                self.i = (self.i + v[x]) & 0xFFF
            elif kk == 0x29:
                self.i = 0x50 + (v[x] & 0xF) * 5
            elif kk == 0x33:
                self.mem[self.i] = v[x] // 100
                self.mem[self.i + 1] = (v[x] // 10) % 10
                self.mem[self.i + 2] = v[x] % 10
            elif kk == 0x55:
                self.mem[self.i:self.i + x + 1] = v[0:x + 1]
            elif kk == 0x65:
                v[0:x + 1] = self.mem[self.i:self.i + x + 1]
        else:
            self.halted = True

    def draw(self, sx: int, sy: int, rows: int) -> None:
        # Sprites clip at the edges rather than wrapping, which is what the
        # bundled ROMs are written against.
        self.v[0xF] = 0
        self.drew = True
        for row in range(rows):
            py = (sy % H) + row
            if py >= H:
                break
            bits = self.mem[(self.i + row) & 0xFFF]
            for bit in range(8):
                if not (bits >> (7 - bit)) & 1:
                    continue
                px = (sx % W) + bit
                if px >= W:
                    break
                idx = py * W + px
                if self.display[idx]:
                    self.v[0xF] = 1
                self.display[idx] ^= 1

    def run_frame(self, ipf: int) -> None:
        # display_wait, which the emulator defaults to on: a draw is held to
        # one per frame, so a ROM's real speed depends on how many times it
        # draws per iteration, not only on its delay-timer pacing. Ignoring it
        # here would time the ROM against an interpreter nobody runs.
        self.drew = False
        for _ in range(ipf):
            if self.halted:
                return
            self.step()
            if self.waiting_for_key or (self.drew and self.display_wait):
                break
        self.dt = max(0, self.dt - 1)
        self.st = max(0, self.st - 1)

    def render(self) -> str:
        return "\n".join(
            "".join("#" if self.display[y * W + x] else "." for x in range(W))
            for y in range(H)
        )


def parse_hold(spec: str) -> tuple[int, int, int]:
    key, _, span = spec.partition(":")
    start, _, end = span.partition("-")
    return int(key, 16), int(start), int(end)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--ipf", type=int, default=11)
    ap.add_argument("--hold", action="append", default=[],
                    help="KEY:START-END, e.g. 9:0-400")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--no-display-wait", action="store_true")
    args = ap.parse_args()

    with open(args.rom, "rb") as handle:
        cpu = Chip8(handle.read())
    cpu.display_wait = not args.no_display_wait

    holds = [parse_hold(h) for h in args.hold]
    for frame in range(args.frames):
        cpu.keys = [False] * 16
        for key, start, end in holds:
            if start <= frame < end:
                cpu.keys[key] = True
        cpu.run_frame(args.ipf)
        if cpu.halted:
            print(f"HALTED at frame {frame}, PC={cpu.pc:03X}")
            break

    if not args.quiet:
        print(cpu.render())
    lit = sum(cpu.display)
    print(f"\nframes={args.frames} lit={lit} PC={cpu.pc:03X} "
          f"halted={cpu.halted} waiting_for_key={cpu.waiting_for_key}")
    print("V:", " ".join(f"{n:X}={cpu.v[n]:02X}" for n in range(16)))
    return 1 if cpu.halted or lit == 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
