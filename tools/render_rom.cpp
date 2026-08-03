// Headless ROM runner.
//
// Loads a ROM, runs it for a number of frames with no window, and prints the
// display as ASCII. Used as a smoke test in CI, where there is no GPU to open
// a Raylib window against, and handy for checking a ROM without launching the
// emulator.
//
//   render_rom roms/bounce.ch8 [frames] [instructions-per-frame]

#include "../src/chip8.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: render_rom <rom> [frames] [instructions-per-frame]\n");
    return 2;
  }

  const int frames = argc > 2 ? std::atoi(argv[2]) : 60;
  const int ipf = argc > 3 ? std::atoi(argv[3]) : 11;

  chip8::Chip8 cpu;
  cpu.SetRandomSeed(1);  // fixed, so CI output is reproducible
  if (!cpu.LoadRomFile(argv[1])) {
    std::fprintf(stderr, "could not load ROM: %s\n", argv[1]);
    return 1;
  }

  for (int i = 0; i < frames; ++i) cpu.RunFrame(ipf);

  int lit = 0;
  for (int y = 0; y < chip8::kDisplayHeight; ++y) {
    std::string row;
    for (int x = 0; x < chip8::kDisplayWidth; ++x) {
      const bool on = cpu.display()[y * chip8::kDisplayWidth + x] != 0;
      lit += on ? 1 : 0;
      row += on ? '#' : '.';
    }
    std::printf("%s\n", row.c_str());
  }

  std::printf("\nframes=%d  instructions=%llu  lit=%d  PC=%03X  I=%03X\n",
              frames, static_cast<unsigned long long>(cpu.instructions_executed()),
              lit, cpu.pc(), cpu.index());
  std::printf("halted=%s  delay=%u  sound=%u\n", cpu.halted() ? "yes" : "no",
              cpu.delay_timer(), cpu.sound_timer());

  // A ROM that halted, or that drew nothing at all, is a failure worth
  // catching in CI rather than eyeballing.
  if (cpu.halted()) return 1;
  return lit > 0 ? 0 : 1;
}
