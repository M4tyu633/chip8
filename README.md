# CHIP-8

A CHIP-8 interpreter written from scratch in C++17, with a Raylib front end
that doubles as a visual debugger. It runs natively and in the browser through
WebAssembly.

**[Play it in the browser](https://chip8-emulator-matthew.vercel.app)**

CHIP-8 is a virtual machine from 1977, designed so that hobbyists could write
games once and run them on any 8-bit micro that had an interpreter. There are
35 instructions, 4 KB of memory, sixteen 8-bit registers and a 64x32
monochrome display. It is small enough to hold in your head and awkward enough
to be interesting: no signed comparison, one flag register that doubles as a
general-purpose register, and a sprite instruction whose exact behaviour every
implementation got slightly differently.

## What is in here

```
src/chip8.h  src/chip8.cpp   the interpreter, with no platform dependencies
src/main.cpp                 Raylib window, renderer and debugger UI
tests/test_chip8.cpp         106 assertions over the core
tools/assemble.py            a CHIP-8 assembler, used to build the ROMs
tools/render_rom.cpp         headless runner that prints the screen as ASCII
roms/src/*.asm               ROM source
roms/*.ch8                   assembled ROMs
web/shell.html               the page the WebAssembly build is served in
```

The interpreter does not include Raylib and does not know what a window is.
Everything it does is visible in its own state, which the front end reads once
a frame. That split is why the tests and the headless runner can build and run
in CI where there is no GPU.

## The debugger

The right-hand panel is live machine state rather than a log:

- **V0 to VF**, in hex and decimal, with a register flashing amber for a
  moment after it is written.
- **PC, I, SP** and the two timers, with the timers highlighted while they are
  counting down.
- **The call stack**, which is what tells you a ROM is about to overflow it.
- **The eight bytes around I**, because I is almost always pointing at the
  next thing that matters: a sprite, a BCD result, or a block of registers
  about to be loaded.
- **The keypad**, lit to match what the core currently believes is held down.

Under the display is a disassembly that follows the program counter. CHIP-8
instructions are a fixed two bytes, so the listing can be walked backwards
from the PC without the usual guesswork about where instructions start.

`Space` pauses and `N` single-steps, which is the pair you actually want when
a ROM is misbehaving.

## Quirks

Programs were written against one specific interpreter, and the popular ones
disagreed. A ROM that renders perfectly under one set of rules can be
unplayable under another, so the differences are switches rather than
decisions, toggled live with `F1` to `F5`:

| Key | Behaviour | Default |
| --- | --- | --- |
| `F1` | `8xy6`/`8xyE` shift Vy into Vx, rather than shifting Vx in place | COSMAC (Vy) |
| `F2` | `Fx55`/`Fx65` leave I incremented past the last register | on |
| `F3` | `8xy1`/`8xy2`/`8xy3` reset VF as a side effect | on |
| `F4` | `Dxyn` waits for vertical blank, capping draws at one per frame | on |
| `F5` | Sprites are clipped at the screen edge rather than wrapping | clip |

The defaults are original COSMAC VIP behaviour, which is what the bundled ROMs
assume. Most ROMs written after about 1990 want `F1` and `F2` flipped.

## Controls

The 16-key pad maps onto the left block of a QWERTY keyboard, the same way
every CHIP-8 emulator has done it since the pad was laid out `1 2 3 C` /
`4 5 6 D` / `7 8 9 E` / `A 0 B F`:

```
1 2 3 4          1 2 3 C
Q W E R    ->    4 5 6 D
A S D F          7 8 9 E
Z X C V          A 0 B F
```

| Key | Action |
| --- | --- |
| `Space` | Pause and resume |
| `N` | Step one instruction while paused |
| `Tab` | Cycle the bundled ROMs |
| `Backspace` | Reset the current ROM |
| `[` `]` | Instructions per frame, 1 to 200 |
| `F1`-`F5` | Toggle the quirks above |

Native builds also accept a `.ch8` file dropped onto the window, or a path on
the command line. The browser build has a file picker instead.

## The ROMs

The three bundled ROMs are original, written for this project in the assembler
under `tools/`, so the repository carries no third-party binaries. Source is in
`roms/src`.

- **bounce** paces a ball off the display-wait quirk rather than the delay
  timer, and checks its bounds by equality, since CHIP-8 has no signed
  comparison and a one-pixel step can only ever overshoot an edge by one.
- **counter** counts 0 to 255 in decimal, exercising `Fx33` and `Fx65`. The
  awkward part is that `Fx65` always loads from V0 up, so reading the digits
  back necessarily clobbers the counter.
- **keypad** shows the hex digit of whichever key you press. It is a test for
  `Fx0A`, which must block until a key is pressed *and released* rather than
  completing on the press.

Rebuild them with:

```bash
python tools/assemble.py roms/src/bounce.asm roms/bounce.ch8
```

## Building

### Native

Needs CMake 3.16+ and a C++17 compiler. Raylib is fetched automatically.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/chip8 roms/bounce.ch8
```

### Web

Needs the [Emscripten SDK](https://emscripten.org/) on the path.

```bash
emcmake cmake -B build-web -G Ninja -DPLATFORM=Web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web
```

The output is a self-contained `index.html`, `index.js`, `index.wasm` and
`index.data` in `build-web`, deployable to any static host. The ROMs are baked
into the `.data` file, so the page works with no network requests after load.

### Tests

The core, the tests and the headless runner do not need Raylib, so configure
with the front end off and nothing is fetched or linked against a window
system. This is how CI builds them, on a runner with no GPU and no X11
headers.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCHIP8_BUILD_FRONTEND=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

The suite concentrates on the parts that are genuinely easy to get wrong:
flag registers written in the wrong order, each of the quirk switches, sprite
clipping and collision, and the `Fx0A` release semantics. `ADD VF, Vy` has its
own case, because when the destination register *is* VF the carry has to win
over the sum.

## Notes on the implementation

**VF is written last.** Every arithmetic opcode that sets a flag computes the
flag first, stores the result, then writes VF. Doing it the other way round is
correct for fifteen of the sixteen registers and wrong for VF itself.

**Display wait rewinds the PC.** When `Dxyn` is held back to the next frame,
the instruction has already been fetched and the PC has already advanced. The
step rewinds it so the same draw is retried rather than skipped.

**Fx0A completes on release.** Waiting for the press is the obvious reading
and it is wrong: a single held key would satisfy several consecutive `Fx0A`
instructions, which breaks any menu that asks for two inputs in a row.

**Sprite positions wrap even when the body clips.** The starting coordinate is
taken modulo the screen size, but a sprite that then runs off the edge is cut
off. That asymmetry is real hardware behaviour rather than an oversight.

**The random source is a hand-rolled xorshift** rather than `<random>`, so a
given seed produces the same sequence natively and in the browser. `Cxkk`
needs eight bits of noise, not statistical quality, and reproducibility is
worth more here than distribution.

## Licence

MIT. See [LICENSE](LICENSE).
