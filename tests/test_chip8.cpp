// Unit tests for the CHIP-8 core.
//
// No framework: the core has no dependencies and neither should its tests, so
// the whole thing builds with a single compiler invocation on any target,
// including Emscripten running under Node.
//
// The cases here concentrate on the parts that are genuinely easy to get
// wrong: flag registers written in the wrong order, the quirk switches, sprite
// clipping and collision, and the Fx0A release semantics. The straightforward
// opcodes are covered too, but they are not where emulators break.

#include "../src/chip8.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

void CheckEq(int actual, int expected, const std::string& what) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::printf("  FAIL: %s (got %d, expected %d)\n", what.c_str(), actual,
                expected);
  }
}

// Assembles a program at 0x200 out of raw opcodes and loads it.
void Load(chip8::Chip8& cpu, const std::vector<std::uint16_t>& program) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(program.size() * 2);
  for (const std::uint16_t op : program) {
    bytes.push_back(static_cast<std::uint8_t>(op >> 8));
    bytes.push_back(static_cast<std::uint8_t>(op & 0xFF));
  }
  Check(cpu.LoadRom(bytes.data(), bytes.size()), "ROM loaded");
}

// Runs n instructions, ignoring the frame budget and timers.
void Steps(chip8::Chip8& cpu, int n) {
  for (int i = 0; i < n; ++i) cpu.Step();
}

void TestLoadAndArithmetic() {
  std::printf("load and arithmetic\n");
  chip8::Chip8 cpu;
  Load(cpu, {
                0x600A,  // LD V0, 0x0A
                0x6105,  // LD V1, 0x05
                0x8014,  // ADD V0, V1  -> 0x0F, no carry
            });
  Steps(cpu, 3);
  CheckEq(cpu.v()[0], 0x0F, "V0 = 0x0A + 0x05");
  CheckEq(cpu.v()[0xF], 0, "no carry set");

  // 7xkk wraps and must NOT touch VF.
  chip8::Chip8 wrap;
  Load(wrap, {0x60FF, 0x6F07, 0x7002});
  Steps(wrap, 3);
  CheckEq(wrap.v()[0], 0x01, "7xkk wraps 0xFF + 2");
  CheckEq(wrap.v()[0xF], 0x07, "7xkk leaves VF alone");
}

void TestCarryAndBorrowFlags() {
  std::printf("carry and borrow flags\n");

  chip8::Chip8 carry;
  Load(carry, {0x60FF, 0x6102, 0x8014});  // 0xFF + 0x02 overflows
  Steps(carry, 3);
  CheckEq(carry.v()[0], 0x01, "sum wraps to 0x01");
  CheckEq(carry.v()[0xF], 1, "carry flag set");

  chip8::Chip8 borrow;
  Load(borrow, {0x6002, 0x6105, 0x8015});  // 2 - 5 borrows
  Steps(borrow, 3);
  CheckEq(borrow.v()[0], 0xFD, "difference wraps");
  CheckEq(borrow.v()[0xF], 0, "VF cleared on borrow");

  chip8::Chip8 subn;
  Load(subn, {0x6002, 0x6105, 0x8017});  // V0 = V1 - V0 = 3
  Steps(subn, 3);
  CheckEq(subn.v()[0], 0x03, "SUBN result");
  CheckEq(subn.v()[0xF], 1, "SUBN sets VF when no borrow");

  // The order-of-writes trap: when x is 0xF the flag must win, not the sum.
  chip8::Chip8 vf_dest;
  Load(vf_dest, {0x6FFF, 0x6102, 0x8F14});  // ADD VF, V1 with VF = 0xFF
  Steps(vf_dest, 3);
  CheckEq(vf_dest.v()[0xF], 1, "ADD into VF leaves the carry, not the sum");
}

void TestShiftQuirk() {
  std::printf("shift quirk\n");

  chip8::Chip8 vip;  // default: shift reads Vy
  Load(vip, {0x6000, 0x6103, 0x8016});
  Steps(vip, 3);
  CheckEq(vip.v()[0], 0x01, "COSMAC SHR shifts Vy into Vx");
  CheckEq(vip.v()[0xF], 1, "COSMAC SHR flag is Vy bit 0");

  chip8::Chip8 modern;
  modern.quirks.shift_uses_vy = false;
  Load(modern, {0x6003, 0x6100, 0x8016});
  Steps(modern, 3);
  CheckEq(modern.v()[0], 0x01, "modern SHR shifts Vx in place");
  CheckEq(modern.v()[0xF], 1, "modern SHR flag is Vx bit 0");
}

void TestVfResetQuirk() {
  std::printf("VF reset quirk\n");

  chip8::Chip8 on;
  Load(on, {0x6FFF, 0x600C, 0x610A, 0x8011});  // OR with VF pre-set
  Steps(on, 4);
  CheckEq(on.v()[0], 0x0E, "OR result");
  CheckEq(on.v()[0xF], 0, "OR resets VF on COSMAC");

  chip8::Chip8 off;
  off.quirks.vf_reset = false;
  Load(off, {0x6FFF, 0x600C, 0x610A, 0x8011});
  Steps(off, 4);
  CheckEq(off.v()[0xF], 0xFF, "OR leaves VF when the quirk is off");
}

void TestMemoryIncrementQuirk() {
  std::printf("memory increment quirk\n");

  chip8::Chip8 on;
  Load(on, {0xA400, 0x6001, 0x6102, 0x6203, 0xF255});  // store V0..V2
  Steps(on, 5);
  CheckEq(on.memory()[0x400], 1, "stored V0");
  CheckEq(on.memory()[0x402], 3, "stored V2");
  CheckEq(on.index(), 0x403, "I advanced past the last register");

  chip8::Chip8 off;
  off.quirks.memory_increment = false;
  Load(off, {0xA400, 0x6001, 0xF055});
  Steps(off, 3);
  CheckEq(off.index(), 0x400, "I unchanged when the quirk is off");

  // Round-trip through Fx65.
  chip8::Chip8 load_back;
  Load(load_back, {0xA400, 0x6009, 0xF055, 0xA400, 0x6000, 0xF065});
  Steps(load_back, 6);
  CheckEq(load_back.v()[0], 9, "Fx65 reads back what Fx55 wrote");
}

void TestBcd() {
  std::printf("BCD\n");
  chip8::Chip8 cpu;
  Load(cpu, {0xA400, 0x60FF, 0xF033});  // 255 -> 2, 5, 5
  Steps(cpu, 3);
  CheckEq(cpu.memory()[0x400], 2, "BCD hundreds");
  CheckEq(cpu.memory()[0x401], 5, "BCD tens");
  CheckEq(cpu.memory()[0x402], 5, "BCD units");

  chip8::Chip8 small;
  Load(small, {0xA400, 0x6007, 0xF033});  // 7 -> 0, 0, 7
  Steps(small, 3);
  CheckEq(small.memory()[0x400], 0, "BCD hundreds of 7");
  CheckEq(small.memory()[0x402], 7, "BCD units of 7");
}

void TestSkipsAndJumps() {
  std::printf("skips, jumps and subroutines\n");

  chip8::Chip8 skip;
  Load(skip, {0x6042, 0x3042, 0x6099, 0x6111});  // SE skips the LD V0,99
  Steps(skip, 3);
  CheckEq(skip.v()[0], 0x42, "SE skipped the next instruction");
  CheckEq(skip.v()[1], 0x11, "and executed the one after");

  chip8::Chip8 call;
  Load(call, {0x2206, 0x0000, 0x0000, 0x6007, 0x00EE});
  call.Step();  // CALL 0x206
  CheckEq(call.pc(), 0x206, "CALL jumped");
  CheckEq(call.sp(), 1, "return address pushed");
  call.Step();  // LD V0, 7
  call.Step();  // RET
  CheckEq(call.pc(), 0x202, "RET returned past the CALL");
  CheckEq(call.sp(), 0, "stack popped");

  // Bnnn with the default quirk adds V0.
  chip8::Chip8 jump;
  Load(jump, {0x6004, 0xB300});
  Steps(jump, 2);
  CheckEq(jump.pc(), 0x304, "JP V0,nnn");
}

void TestFontAndIndex() {
  std::printf("font and index\n");
  chip8::Chip8 cpu;
  Load(cpu, {0x600A, 0xF029});  // point I at glyph 'A'
  Steps(cpu, 2);
  CheckEq(cpu.index(), chip8::kFontStart + 10 * 5, "Fx29 addresses glyph A");
  // First row of 'A' is 0xF0 in the standard font.
  CheckEq(cpu.memory()[cpu.index()], 0xF0, "glyph data present");

  chip8::Chip8 zero;
  Load(zero, {0x6000, 0xF029});
  Steps(zero, 2);
  CheckEq(zero.index(), chip8::kFontStart, "Fx29 addresses glyph 0");
}

void TestSpriteDrawAndCollision() {
  std::printf("sprite draw and collision\n");

  // Draw glyph 0 at the origin, then draw it again: XOR must erase it and
  // report a collision.
  chip8::Chip8 cpu;
  Load(cpu, {0x6000, 0x6100, 0x6200, 0xF229, 0xD015, 0xD015});
  Steps(cpu, 5);

  int lit = 0;
  for (const std::uint8_t p : cpu.display()) lit += p;
  CheckEq(lit, 14, "glyph 0 lights 14 pixels");
  CheckEq(cpu.v()[0xF], 0, "no collision on a clean draw");
  Check(cpu.display()[0] == 1, "top-left pixel of glyph 0 is lit");

  // display_wait holds the second draw until the next frame.
  cpu.TickTimers();
  cpu.Step();
  lit = 0;
  for (const std::uint8_t p : cpu.display()) lit += p;
  CheckEq(lit, 0, "redrawing the same sprite erases it");
  CheckEq(cpu.v()[0xF], 1, "collision flag set");
}

void TestSpriteClippingQuirk() {
  std::printf("sprite clipping quirk\n");

  // A solid 8-pixel row drawn at x = 60, so four pixels overhang the right
  // edge. The sprite data is the last word of the ROM, at 0x208, rather than
  // being poked into memory afterwards.
  const std::vector<std::uint16_t> program = {
      0xA208,  // LD I, 0x208   (the data word below)
      0x603C,  // LD V0, 60
      0x6100,  // LD V1, 0
      0xD011,  // DRW V0, V1, 1
      0xFF00,  // data: one solid row
  };

  chip8::Chip8 clip;
  Load(clip, program);
  Steps(clip, 4);

  int lit = 0;
  for (const std::uint8_t p : clip.display()) lit += p;
  CheckEq(lit, 4, "clipped sprite loses the overhanging pixels");
  CheckEq(clip.display()[0], 0, "nothing wrapped to column 0");

  chip8::Chip8 wrap;
  wrap.quirks.clip_sprites = false;
  Load(wrap, program);
  Steps(wrap, 4);

  lit = 0;
  for (const std::uint8_t p : wrap.display()) lit += p;
  CheckEq(lit, 8, "wrapping sprite keeps all 8 pixels");
  CheckEq(wrap.display()[0], 1, "and wraps around to column 0");
}

void TestDisplayWait() {
  std::printf("display wait\n");
  chip8::Chip8 cpu;
  Load(cpu, {0x6000, 0x6100, 0xF029, 0xD015, 0xD015});
  Steps(cpu, 4);  // through the first DRW

  const std::uint16_t pc_after_first_draw = cpu.pc();
  const chip8::StepResult result = cpu.Step();
  Check(result == chip8::StepResult::kWaitingForVBlank,
        "second draw in the same frame stalls");
  CheckEq(cpu.pc(), pc_after_first_draw, "stalled draw rewinds the PC");

  cpu.TickTimers();
  const chip8::StepResult after = cpu.Step();
  Check(after == chip8::StepResult::kOk, "the draw proceeds next frame");
}

void TestKeypadAndWait() {
  std::printf("keypad and Fx0A\n");

  chip8::Chip8 skip;
  Load(skip, {0x6005, 0xE09E, 0x6100, 0x6122});
  skip.SetKey(5, true);
  Steps(skip, 2);
  CheckEq(skip.pc(), 0x206, "SKP skips while the key is held");

  // Fx0A completes on release, not on press.
  chip8::Chip8 wait;
  Load(wait, {0xF00A, 0x6122});
  Check(wait.Step() == chip8::StepResult::kWaitingForKey, "Fx0A parks the CPU");
  wait.SetKey(0x7, true);
  Check(wait.Step() == chip8::StepResult::kWaitingForKey,
        "still parked while the key is held");
  wait.SetKey(0x7, false);
  Check(wait.Step() == chip8::StepResult::kOk, "released key resumes the CPU");
  CheckEq(wait.v()[0], 0x7, "the key value landed in Vx");
}

void TestTimers() {
  std::printf("timers\n");
  chip8::Chip8 cpu;
  Load(cpu, {0x603C, 0xF015, 0xF018});
  Steps(cpu, 3);
  CheckEq(cpu.delay_timer(), 60, "delay timer set");
  Check(cpu.beeping(), "sound timer makes it beep");

  for (int i = 0; i < 60; ++i) cpu.TickTimers();
  CheckEq(cpu.delay_timer(), 0, "delay timer counted down to zero");
  Check(!cpu.beeping(), "beep stopped");

  cpu.TickTimers();
  CheckEq(cpu.delay_timer(), 0, "timer does not underflow past zero");
}

void TestRandomIsSeeded() {
  std::printf("seeded RND\n");
  chip8::Chip8 a, b;
  a.SetRandomSeed(12345);
  b.SetRandomSeed(12345);
  Load(a, {0xC0FF, 0xC1FF});
  Load(b, {0xC0FF, 0xC1FF});
  // LoadRom resets, which must not clobber the seed.
  a.SetRandomSeed(12345);
  b.SetRandomSeed(12345);
  Steps(a, 2);
  Steps(b, 2);
  CheckEq(a.v()[0], b.v()[0], "same seed gives the same first value");
  CheckEq(a.v()[1], b.v()[1], "and the same second value");

  // The mask must be applied.
  chip8::Chip8 masked;
  masked.SetRandomSeed(999);
  Load(masked, {0xC00F});
  masked.SetRandomSeed(999);
  Steps(masked, 1);
  Check((masked.v()[0] & 0xF0) == 0, "RND respects the kk mask");
}

void TestHaltingAndBounds() {
  std::printf("halting and bounds\n");

  chip8::Chip8 bad;
  Load(bad, {0x8FFF});  // no such 8xy_ variant
  bad.Step();
  Check(bad.halted(), "illegal instruction halts the CPU");
  Check(bad.Step() == chip8::StepResult::kIllegalInstruction,
        "stepping a halted CPU stays halted");

  chip8::Chip8 underflow;
  Load(underflow, {0x00EE});  // RET with an empty stack
  underflow.Step();
  Check(underflow.halted(), "stack underflow halts rather than corrupting");

  // A ROM larger than the space above 0x200 must be rejected outright.
  chip8::Chip8 big;
  std::vector<std::uint8_t> too_big(chip8::kMaxRomSize + 1, 0);
  Check(!big.LoadRom(too_big.data(), too_big.size()), "oversized ROM rejected");
  Check(!big.LoadRom(nullptr, 0), "empty ROM rejected");

  std::vector<std::uint8_t> exact(chip8::kMaxRomSize, 0);
  Check(big.LoadRom(exact.data(), exact.size()), "exactly-full ROM accepted");
}

void TestDisassembler() {
  std::printf("disassembler\n");
  Check(chip8::Chip8::Disassemble(0x00E0) == "CLS", "CLS");
  Check(chip8::Chip8::Disassemble(0x00EE) == "RET", "RET");
  Check(chip8::Chip8::Disassemble(0x1234) == "JP   234", "JP");
  Check(chip8::Chip8::Disassemble(0xD015) == "DRW  V0,V1,5", "DRW");
  Check(chip8::Chip8::Disassemble(0xF165) == "LD   V1,[I]", "Fx65");
}

}  // namespace

int main() {
  std::printf("CHIP-8 core tests\n\n");

  TestLoadAndArithmetic();
  TestCarryAndBorrowFlags();
  TestShiftQuirk();
  TestVfResetQuirk();
  TestMemoryIncrementQuirk();
  TestBcd();
  TestSkipsAndJumps();
  TestFontAndIndex();
  TestSpriteDrawAndCollision();
  TestSpriteClippingQuirk();
  TestDisplayWait();
  TestKeypadAndWait();
  TestTimers();
  TestRandomIsSeeded();
  TestHaltingAndBounds();
  TestDisassembler();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
