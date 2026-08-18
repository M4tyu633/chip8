// Raylib frontend and visual debugger for the CHIP-8 core.
//
// The layout is one window split in three: the emulated 64x32 display top
// left, a live disassembly under it, and a register/stack/memory panel down
// the right. Everything on screen is read straight from the core each frame,
// so what you see is the machine state rather than a cached copy of it.

#include "chip8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "raylib.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace {

constexpr int kScreenWidth = 1280;
constexpr int kScreenHeight = 720;

// The emulated display, drawn at an integer scale so pixels stay square and
// crisp. 12 rather than 14: the four rows of disassembly that buys are worth
// more than the extra 128 pixels of a screen that is only 64 wide to begin
// with.
constexpr int kPixelScale = 11;
constexpr int kDisplayX = 16;
constexpr int kDisplayY = 36;
constexpr int kDisplayW = chip8::kDisplayWidth * kPixelScale;   // 704
constexpr int kDisplayH = chip8::kDisplayHeight * kPixelScale;  // 352

constexpr int kPanelX = kDisplayX + kDisplayW + 16;   // 736
constexpr int kPanelW = kScreenWidth - kPanelX - 16;  // 528

// A strip for the current ROM's controls, directly under the display where it
// cannot be missed.
constexpr int kHintY = kDisplayY + kDisplayH + 6;  // 394
constexpr int kHintH = 26;

// The glyph atlas is baked once at this size and scaled down for every size
// actually drawn. Above the largest of those, so nothing is ever scaled up.
constexpr int kFontAtlasSize = 48;

// Focus mode: the debugger folded away and the game scaled up to fill the
// canvas on its own. Still an integer scale, so the pixels stay square.
constexpr int kFocusScale = 18;
constexpr int kFocusW = chip8::kDisplayWidth * kFocusScale;   // 1152
constexpr int kFocusH = chip8::kDisplayHeight * kFocusScale;  // 576
constexpr int kFocusX = (kScreenWidth - kFocusW) / 2;         // 64
constexpr int kFocusY = 36;

constexpr int kDisasmY = kHintY + kHintH + 6;  // 426
constexpr int kDisasmH = kScreenHeight - kDisasmY - 20;
constexpr int kDisasmRowH = 16;
constexpr int kDisasmHeaderH = 24;
// 18 rows, so 36 bytes of code are visible at once. Worth sizing deliberately:
// a loop shorter than the window settles into it and then stops moving, and
// most CHIP-8 inner loops are well under 36 bytes.
constexpr int kDisasmRows = (kDisasmH - kDisasmHeaderH - 6) / kDisasmRowH;

// The disassembly window is only allowed to move twice a second. See
// UpdateDisasmWindow for why it needs a speed limit at all.
constexpr int kDisasmSettle = 30;
// Per-frame decay on the execution histogram. 0.94 is a half-life of about
// eleven frames: long enough that a subroutine entered every other frame stays
// lit, short enough that the listing follows a ROM that changes phase.
constexpr float kHeatDecay = 0.94f;

// Cycles per frame. The original ran around 500-700 instructions a second at
// 60 Hz; 11 per frame is the usual default and what the bundled ROMs assume.
constexpr int kDefaultSpeed = 11;
constexpr int kMinSpeed = 1;
constexpr int kMaxSpeed = 200;

// Palette. Deliberately not raylib's defaults, which are very saturated.
const Color kBg = Color{14, 17, 23, 255};
const Color kPanelBg = Color{20, 24, 33, 255};
const Color kGrid = Color{28, 33, 45, 255};
const Color kOffPixel = Color{18, 22, 30, 255};
const Color kOnPixel = Color{126, 231, 195, 255};
const Color kText = Color{198, 208, 224, 255};
const Color kDim = Color{110, 122, 145, 255};
const Color kAccent = Color{126, 231, 195, 255};
const Color kWarn = Color{240, 180, 100, 255};
const Color kBad = Color{233, 112, 112, 255};
const Color kChanged = Color{240, 180, 100, 255};

// Physical key -> CHIP-8 keypad value. The original COSMAC VIP pad was
// 1 2 3 C / 4 5 6 D / 7 8 9 E / A 0 B F, which maps onto the left block of a
// QWERTY keyboard.
struct KeyMap {
  int key;
  int value;
  // What is printed on the physical key. The on-screen pad shows this next to
  // the hex value, because the hex value on its own is unusable: pressing A
  // lights the cell marked 7 and nothing anywhere says why.
  const char* label;
};
constexpr KeyMap kKeyMap[16] = {
    {KEY_ONE, 0x1, "1"}, {KEY_TWO, 0x2, "2"}, {KEY_THREE, 0x3, "3"},
    {KEY_FOUR, 0xC, "4"}, {KEY_Q, 0x4, "Q"}, {KEY_W, 0x5, "W"},
    {KEY_E, 0x6, "E"},   {KEY_R, 0xD, "R"},  {KEY_A, 0x7, "A"},
    {KEY_S, 0x8, "S"},   {KEY_D, 0x9, "D"},  {KEY_F, 0xE, "F"},
    {KEY_Z, 0xA, "Z"},   {KEY_X, 0x0, "X"},  {KEY_C, 0xB, "C"},
    {KEY_V, 0xF, "V"},
};

const char* PhysicalKeyFor(int value) {
  for (const KeyMap& mapping : kKeyMap) {
    if (mapping.value == value) return mapping.label;
  }
  return "?";
}

// The bundled ROMs, in the order TAB cycles them. Games first, so the emulator
// opens on something playable rather than on a demo that only animates.
//
// Each one carries its own control hint, drawn under the display. Leaving that
// to the surrounding web page did not work: the page is scrollable and the
// canvas is not, so by the time you are looking at the game the instructions
// are off screen.
struct RomEntry {
  const char* path;
  const char* title;
  const char* controls;
};
constexpr RomEntry kRoms[] = {
    {"roms/brix.ch8", "BRIX", "A and D move the paddle"},
    {"roms/pong.ch8", "PONG", "1 / Q left player      4 / R right player"},
    {"roms/catch.ch8", "CATCH", "A and D move the bucket"},
    {"roms/bounce.ch8", "BOUNCE", "a demo - no input"},
    {"roms/counter.ch8", "COUNTER", "a demo - no input"},
    {"roms/keypad.ch8", "KEYPAD", "press any of the sixteen keys shown right"},
};
constexpr int kRomCount = static_cast<int>(sizeof(kRoms) / sizeof(kRoms[0]));

// ---------------------------------------------------------------------------

struct App {
  chip8::Chip8 cpu;
  Texture2D screen{};
  Font font{};

  bool paused = false;
  bool rom_loaded = false;
  // Starts folded away. Six panels of hex all updating at once is a lot to be
  // handed unasked, none of it is needed to play, and being shown it before
  // anything explains what it is reads as the thing being broken. H opens it,
  // as does a button on the page.
  bool show_debugger = false;

  // Address at the top of the disassembly listing, held across frames.
  int disasm_top = chip8::kProgramStart;
  // Frames since the window was last allowed to move.
  int disasm_settle = 0;
  // Decaying count of how often each address has been executed recently. Both
  // the window's resting place and the per-row highlight are read off this
  // rather than off the current PC.
  std::array<float, chip8::kMemorySize> heat{};

  int speed = kDefaultSpeed;
  std::string rom_name = "no ROM";
  std::string status;
  float status_timer = 0.0f;

  // Previous register values, so a write can be highlighted for a moment.
  std::array<std::uint8_t, chip8::kRegisterCount> prev_v{};
  std::array<float, chip8::kRegisterCount> v_flash{};

  Sound beep{};
  bool audio_ready = false;

  int rom_index = 0;
  // Shown in the strip under the display. Held separately from kRoms because a
  // ROM loaded from the file picker is not in that table.
  std::string rom_title = "NO ROM";
  std::string rom_controls = "press TAB to load one of the bundled ROMs";
  unsigned long long frame_count = 0;
};

App g_app;

void SetStatus(const std::string& text) {
  g_app.status = text;
  g_app.status_timer = 2.5f;
}

// A short square wave, generated rather than shipped as an asset. CHIP-8 has
// exactly one sound and it is a buzz, so there is nothing to load.
Sound MakeBeep() {
  constexpr int kRate = 22050;
  constexpr float kSeconds = 0.25f;
  constexpr int kSamples = static_cast<int>(kRate * kSeconds);
  constexpr float kFreq = 440.0f;

  std::vector<short> data(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    const float t = static_cast<float>(i) / kRate;
    // Square wave, at a third of full scale so it is audible but not harsh.
    const bool high = std::fmod(t * kFreq, 1.0f) < 0.5f;
    data[i] = static_cast<short>(high ? 9000 : -9000);
  }

  Wave wave{};
  wave.frameCount = kSamples;
  wave.sampleRate = kRate;
  wave.sampleSize = 16;
  wave.channels = 1;
  wave.data = data.data();

  // LoadSoundFromWave copies the samples, so the local buffer can go out of
  // scope straight afterwards.
  return LoadSoundFromWave(wave);
}

void DrawText_(const std::string& text, int x, int y, int size, Color color) {
  DrawTextEx(g_app.font, text.c_str(), Vector2{static_cast<float>(x),
                                               static_cast<float>(y)},
             static_cast<float>(size), 1.0f, color);
}

// Step a size down until the text fits the width it has been given.
//
// The help bar and the control strip are long strings whose length changes
// with the ROM and the layout, and the default raylib font is wider than it
// looks. Guessing at whether they fit runs text off the edge of the canvas,
// which is invisible to every check that does not involve looking at it.
int FitSize(const std::string& text, int width, int start, int minimum) {
  for (int size = start; size > minimum; --size) {
    const float measured =
        MeasureTextEx(g_app.font, text.c_str(), static_cast<float>(size), 1.0f)
            .x;
    if (measured <= static_cast<float>(width)) return size;
  }
  return minimum;
}

std::string Hex(unsigned value, int digits) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%0*X", digits, value);
  return std::string(buffer);
}

// ---------------------------------------------------------------------------

void UploadDisplay() {
  // One RGBA byte per pixel. 64x32 is 8 KB, so rebuilding the whole thing on
  // a dirty frame costs less than tracking which pixels moved.
  static std::array<Color, chip8::kDisplayPixels> pixels;
  const auto& display = g_app.cpu.display();
  for (int i = 0; i < chip8::kDisplayPixels; ++i) {
    pixels[i] = display[i] ? kOnPixel : kOffPixel;
  }
  UpdateTexture(g_app.screen, pixels.data());
}

void LoadRomAt(int index) {
  g_app.rom_index = ((index % kRomCount) + kRomCount) % kRomCount;
  const RomEntry& rom = kRoms[g_app.rom_index];

  if (g_app.cpu.LoadRomFile(rom.path)) {
    g_app.rom_loaded = true;
    g_app.paused = false;
    g_app.rom_name = GetFileName(rom.path);
    g_app.rom_title = rom.title;
    g_app.rom_controls = rom.controls;
    g_app.prev_v = g_app.cpu.v();
    g_app.v_flash.fill(0.0f);
    g_app.disasm_top = chip8::kProgramStart;
    g_app.disasm_settle = 0;
    g_app.heat.fill(0.0f);
    SetStatus("loaded " + g_app.rom_name);
  } else {
    SetStatus("could not load that ROM");
  }
  UploadDisplay();
}

void HandleInput() {
  for (const KeyMap& mapping : kKeyMap) {
    if (IsKeyPressed(mapping.key)) g_app.cpu.SetKey(mapping.value, true);
    if (IsKeyReleased(mapping.key)) g_app.cpu.SetKey(mapping.value, false);
  }

  if (IsKeyPressed(KEY_SPACE)) {
    g_app.paused = !g_app.paused;
    SetStatus(g_app.paused ? "paused" : "running");
  }
  if (IsKeyPressed(KEY_N) && g_app.paused) {
    g_app.cpu.Step();
    // Stepping by hand still has to advance the frame clock, or a ROM that
    // waits on the delay timer can never make progress.
    g_app.cpu.TickTimers();
  }
  if (IsKeyPressed(KEY_BACKSPACE)) {
    LoadRomAt(g_app.rom_index);
    SetStatus("reset");
  }
  if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_EQUAL)) {
    g_app.speed = std::min(kMaxSpeed, g_app.speed + 1);
    SetStatus("speed " + std::to_string(g_app.speed) + "/frame");
  }
  if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_MINUS)) {
    g_app.speed = std::max(kMinSpeed, g_app.speed - 1);
    SetStatus("speed " + std::to_string(g_app.speed) + "/frame");
  }
  if (IsKeyPressed(KEY_TAB)) LoadRomAt(g_app.rom_index + 1);

  // H rather than a function key: F1 to F5 are the quirks already, and the
  // browser claims several of the rest.
  if (IsKeyPressed(KEY_H)) {
    g_app.show_debugger = !g_app.show_debugger;
    SetStatus(g_app.show_debugger ? "debugger shown" : "debugger hidden");
  }

  // Quirk toggles, so a ROM written for a different interpreter can be made
  // to behave without recompiling.
  if (IsKeyPressed(KEY_F1)) {
    g_app.cpu.quirks.shift_uses_vy = !g_app.cpu.quirks.shift_uses_vy;
    SetStatus(std::string("shift quirk: ") +
              (g_app.cpu.quirks.shift_uses_vy ? "COSMAC (Vy)" : "modern (Vx)"));
  }
  if (IsKeyPressed(KEY_F2)) {
    g_app.cpu.quirks.memory_increment = !g_app.cpu.quirks.memory_increment;
    SetStatus(std::string("Fx55/65 increment: ") +
              (g_app.cpu.quirks.memory_increment ? "on" : "off"));
  }
  if (IsKeyPressed(KEY_F3)) {
    g_app.cpu.quirks.vf_reset = !g_app.cpu.quirks.vf_reset;
    SetStatus(std::string("VF reset: ") +
              (g_app.cpu.quirks.vf_reset ? "on" : "off"));
  }
  if (IsKeyPressed(KEY_F4)) {
    g_app.cpu.quirks.display_wait = !g_app.cpu.quirks.display_wait;
    SetStatus(std::string("display wait: ") +
              (g_app.cpu.quirks.display_wait ? "on" : "off"));
  }
  if (IsKeyPressed(KEY_F5)) {
    g_app.cpu.quirks.clip_sprites = !g_app.cpu.quirks.clip_sprites;
    SetStatus(std::string("sprite edges: ") +
              (g_app.cpu.quirks.clip_sprites ? "clip" : "wrap"));
  }

  // Native builds accept a dropped ROM file.
  if (IsFileDropped()) {
    FilePathList dropped = LoadDroppedFiles();
    if (dropped.count > 0) {
      const std::string path = dropped.paths[0];
      if (g_app.cpu.LoadRomFile(path)) {
        g_app.rom_loaded = true;
        g_app.paused = false;
        g_app.rom_name = GetFileName(path.c_str());
        g_app.rom_title = "LOADED";
        g_app.rom_controls = "controls depend on the ROM";
        SetStatus("loaded " + g_app.rom_name);
      } else {
        SetStatus("not a valid ROM");
      }
    }
    UnloadDroppedFiles(dropped);
  }
}

// Where the game sits. Small and top-left with the debugger open, filling the
// canvas with it folded away.
struct Rect {
  int x, y, w, h;
};

Rect DisplayRect() {
  if (g_app.show_debugger) return {kDisplayX, kDisplayY, kDisplayW, kDisplayH};
  return {kFocusX, kFocusY, kFocusW, kFocusH};
}

void DrawDisplayPanel(const Rect& at) {
  DrawRectangle(at.x - 2, at.y - 2, at.w + 4, at.h + 4, kGrid);
  DrawTexturePro(g_app.screen,
                 Rectangle{0, 0, static_cast<float>(chip8::kDisplayWidth),
                           static_cast<float>(chip8::kDisplayHeight)},
                 Rectangle{static_cast<float>(at.x), static_cast<float>(at.y),
                           static_cast<float>(at.w), static_cast<float>(at.h)},
                 Vector2{0, 0}, 0.0f, WHITE);

  if (!g_app.rom_loaded) {
    DrawText_("drop a .ch8 file here, or press TAB to cycle the bundled ROMs",
              at.x + 60, at.y + at.h / 2 - 8, 18, kDim);
  }
}

// The strip under the display: what is loaded, how to play it, and how to get
// to the next one. Everything here was previously only in the surrounding HTML
// page, where nobody found it.
void DrawHintStrip(const Rect& display) {
  const int y = display.y + display.h + 10;
  DrawRectangle(display.x - 2, y, display.w + 4, kHintH, kPanelBg);

  int x = display.x + 12;
  DrawText_(g_app.rom_title, x, y + 8, 16, kAccent);
  x += static_cast<int>(
           MeasureTextEx(g_app.font, g_app.rom_title.c_str(), 16.0f, 1.0f).x) +
       18;

  // Right-aligned, so the ROM's own controls stay put as they change length.
  const std::string cycle = "TAB  next ROM";
  const int cycle_width =
      static_cast<int>(MeasureTextEx(g_app.font, cycle.c_str(), 15.0f, 1.0f).x);
  const int cycle_x = display.x + display.w - 12 - cycle_width;
  DrawText_(cycle, cycle_x, y + 9, 15, kDim);

  // Pong's controls name four keys and are much the longest, so the gap left
  // between the title and TAB is what the size has to fit.
  DrawText_(g_app.rom_controls, x, y + 9,
            FitSize(g_app.rom_controls, cycle_x - 20 - x, 15, 10), kText);
}

// Decide where the disassembly listing sits. Called once a frame, before the
// panel is drawn.
//
// Following the PC directly is the obvious thing and it does not work while a
// ROM is running. A single frame's eleven instructions are scattered across
// the main loop, every subroutine it calls, and whatever busy-wait it is
// parked in, so "scroll to wherever the PC happened to stop" picks a different
// one of those regions on almost every frame. Brix rewrites the whole listing
// on 110 frames out of 139 that way - eighteen rows of text swapping content
// about fifteen times a second, which reads as a flicker and cannot be read at
// all.
//
// So while it runs, the window is parked over the busiest stretch of code
// instead, measured from the decaying execution histogram, and it is allowed
// to move at most twice a second. Over fifteen seconds of play that settles to
// three moves for Brix and one each for Pong and Catch.
void UpdateDisasmWindow() {
  const int span = kDisasmRows * 2;
  const int max_top = chip8::kMemorySize - span;

  // Paused is the only time the PC sits still long enough to be worth
  // following, and single-stepping is the only time following it is what you
  // actually want. Scroll by the minimum needed to bring it back into view
  // rather than recentring, so stepping through a loop does not lurch.
  if (g_app.paused || !g_app.rom_loaded) {
    const int pc = static_cast<int>(g_app.cpu.pc());
    if (pc < g_app.disasm_top) {
      g_app.disasm_top = pc - 2;
    } else if (pc >= g_app.disasm_top + span) {
      g_app.disasm_top = pc - span + 4;
    }
    g_app.disasm_top = std::max(0, std::min(g_app.disasm_top, max_top)) & ~1;
    g_app.disasm_settle = 0;
    return;
  }

  if (++g_app.disasm_settle < kDisasmSettle) return;
  g_app.disasm_settle = 0;

  float current = 0.0f;
  for (int i = 0; i < span; ++i) current += g_app.heat[g_app.disasm_top + i];

  // Sliding window sum: each candidate start costs one add and one subtract
  // off the previous one rather than a fresh pass over the span.
  float running = 0.0f;
  for (int i = 0; i < span; ++i) running += g_app.heat[chip8::kProgramStart + i];
  float best_weight = running;
  int best_top = chip8::kProgramStart;
  for (int top = chip8::kProgramStart + 2; top <= max_top; top += 2) {
    running += g_app.heat[top + span - 2] + g_app.heat[top + span - 1];
    running -= g_app.heat[top - 2] + g_app.heat[top - 1];
    if (running > best_weight) {
      best_weight = running;
      best_top = top;
    }
  }

  // The margin is the part that matters. Two stretches of a game loop are
  // often executed about equally often, and without it the listing would swap
  // between them every half second forever.
  if (best_weight > current * 1.5f + 1.0f) g_app.disasm_top = best_top;
}

void DrawTopTelemetryBar() {
  const int y = 14;
  
  // Status dot + ROM Name + State
  DrawCircle(kDisplayX + 6, y + 6, 4, g_app.paused ? kWarn : kAccent);
  std::string rom_upper = g_app.rom_name;
  for (char& c : rom_upper) c = static_cast<char>(std::toupper(c));
  if (rom_upper.find(".CH8") == std::string::npos && rom_upper.find(".ROM") == std::string::npos) {
    rom_upper += ".CH8";
  }
  const std::string state = g_app.cpu.halted() ? " (HALTED)" : (g_app.paused ? " (PAUSED)" : " (RUNNING)");
  DrawText_(rom_upper + state, kDisplayX + 16, y, 13, kText);

  // Speed
  const std::string speed_str = "SPEED: " + std::to_string(g_app.speed) + " ops/frame (" + std::to_string(g_app.speed * 60) + " Hz)";
  DrawText_(speed_str, kDisplayX + 220, y, 13, kDim);

  // FPS & Frame Counter
  const std::string frame_str = "FPS: 60  ·  FRAME #" + std::to_string(g_app.frame_count);
  DrawText_(frame_str, kPanelX + kPanelW - 190, y, 13, kDim);
}

void DrawDisassembly() {
  DrawRectangle(kDisplayX - 2, kDisasmY, kDisplayW + 4, kDisasmH, kPanelBg);
  DrawRectangleLines(kDisplayX - 2, kDisasmY, kDisplayW + 4, kDisasmH, Color{28, 35, 50, 255});
  DrawText_("DISASSEMBLY", kDisplayX + 12, kDisasmY + 8, 12, kDim);

  const int pc = static_cast<int>(g_app.cpu.pc());

  for (int row = 0; row < kDisasmRows; ++row) {
    const int at = g_app.disasm_top + row * 2;
    if (at < 0 || at + 1 >= chip8::kMemorySize) continue;

    const std::uint16_t opcode =
        static_cast<std::uint16_t>((g_app.cpu.memory()[at] << 8) |
                                   g_app.cpu.memory()[at + 1]);
    const bool current = at == pc;
    const int y = kDisasmY + kDisasmHeaderH + row * kDisasmRowH;

    const float hot = g_app.heat[at];
    if (hot > 0.05f) {
      const auto alpha =
          static_cast<unsigned char>(std::min(26.0f, 3.0f + hot * 2.0f));
      DrawRectangle(kDisplayX + 4, y - 2, kDisplayW - 8, kDisasmRowH,
                    Color{126, 231, 195, alpha});
    }

    if (current) {
      DrawRectangle(kDisplayX + 4, y - 2, kDisplayW - 8, kDisasmRowH,
                    Color{126, 231, 195, 36});
      DrawText_(">", kDisplayX + 8, y, 14, kAccent);
    }

    const bool lit = current;
    DrawText_(Hex(at, 4) + ":", kDisplayX + 22, y, 14, lit ? kAccent : kDim);
    DrawText_(Hex(opcode, 4), kDisplayX + 76, y, 14, lit ? kAccent : kDim);
    
    // Disassembled Instruction
    const std::string dis = chip8::Chip8::Disassemble(opcode);
    DrawText_(dis, kDisplayX + 130, y, 14, lit ? kAccent : kText);

    // Smart Disassembly Comment
    std::string comment = "";
    const std::uint8_t nibble = (opcode >> 12) & 0xF;
    if (nibble == 0xD) comment = "; draw sprite" + std::string(lit ? " (active)" : "");
    else if (nibble == 0x3 || nibble == 0x4 || nibble == 0x5 || nibble == 0x9) comment = "; check condition";
    else if (nibble == 0x1) comment = "; jump";
    else if (nibble == 0x2) comment = "; call subroutine";
    else if (nibble == 0x0 && opcode == 0x00EE) comment = "; return";
    else if (nibble == 0x6) comment = "; load register";
    else if (nibble == 0x7) comment = "; add immediate";
    else if (nibble == 0xA) comment = "; set index I";
    else if (nibble == 0xE) comment = "; key skip";

    if (!comment.empty()) {
      DrawText_(comment, kDisplayX + 380, y, 13, Color{85, 100, 125, 255});
    }
  }
}

void DrawRegisterPanel() {
  int y = kDisplayY;
  DrawRectangle(kPanelX, y, kPanelW, kScreenHeight - 32, kPanelBg);
  DrawRectangleLines(kPanelX, y, kPanelW, kScreenHeight - 32, Color{28, 35, 50, 255});

  const int left = kPanelX + 14;
  y += 10;

  // 1. SPECIAL REGISTERS (5 Distinct Card Boxes)
  DrawText_("SPECIAL REGISTERS", left, y, 12, kDim);
  y += 18;

  struct SpecReg {
    const char* label;
    std::string value;
    Color color;
  };
  const SpecReg spec_regs[5] = {
      {"PC", "0x" + Hex(g_app.cpu.pc(), 4), kAccent},
      {"I", "0x" + Hex(g_app.cpu.index(), 4), kText},
      {"SP", "0x" + Hex(g_app.cpu.sp(), 2), kText},
      {"DT", Hex(g_app.cpu.delay_timer(), 2), g_app.cpu.delay_timer() ? kWarn : kText},
      {"ST", Hex(g_app.cpu.sound_timer(), 2), g_app.cpu.sound_timer() ? kWarn : kText},
  };

  const int card_w = (kPanelW - 28 - (4 * 8)) / 5; // ~93 px
  for (int i = 0; i < 5; ++i) {
    const int cx = left + i * (card_w + 8);
    DrawRectangle(cx, y, card_w, 38, Color{15, 19, 28, 255});
    DrawRectangleLines(cx, y, card_w, 38, Color{32, 42, 60, 255});
    DrawText_(spec_regs[i].label, cx + 8, y + 4, 11, kDim);
    DrawText_(spec_regs[i].value, cx + 8, y + 18, 13, spec_regs[i].color);
  }
  y += 38 + 14;

  // 2. REGISTERS (V0 - VF) (4x4 Grid of Pill Cards)
  DrawText_("REGISTERS (V0 - VF)", left, y, 12, kDim);
  y += 18;

  const int reg_card_w = (kPanelW - 28 - (3 * 8)) / 4; // ~120 px
  const int reg_card_h = 24;
  for (int i = 0; i < 16; ++i) {
    const int col = i % 4;
    const int row = i / 4;
    const int rx = left + col * (reg_card_w + 8);
    const int ry = y + row * (reg_card_h + 6);

    const bool flashing = g_app.v_flash[i] > 0.0f;
    DrawRectangle(rx, ry, reg_card_w, reg_card_h, Color{15, 19, 28, 255});
    DrawRectangleLines(rx, ry, reg_card_w, reg_card_h, flashing ? kWarn : Color{32, 42, 60, 255});

    DrawText_("V" + Hex(i, 1), rx + 8, ry + 5, 13, flashing ? kWarn : kDim);
    DrawText_("0x" + Hex(g_app.cpu.v()[i], 2), rx + 38, ry + 5, 13, flashing ? kWarn : kText);
    DrawText_(std::to_string(g_app.cpu.v()[i]), rx + 86, ry + 5, 12, kDim);
  }
  y += 4 * (reg_card_h + 6) + 12;

  // 3. MEMORY @ I (Hex Dump with Annotations)
  const std::string mem_title = "MEMORY @ I (0x" + Hex(g_app.cpu.index(), 4) + ")";
  DrawText_(mem_title, left, y, 12, kDim);
  y += 18;

  const int base = (static_cast<int>(g_app.cpu.index()) / 8) * 8;
  for (int row = 0; row < 3; ++row) {
    const int address = base + row * 8;
    if (address + 7 >= chip8::kMemorySize) break;

    DrawText_(Hex(address, 4) + ":", left, y, 13, kDim);
    for (int i = 0; i < 8; ++i) {
      const bool at_index = (address + i) == g_app.cpu.index();
      DrawText_(Hex(g_app.cpu.memory()[address + i], 2), left + 56 + i * 26, y,
                13, at_index ? kAccent : kText);
    }

    // Annotation
    std::string annot = "";
    if (row == 0) annot = "; sprite/data";
    else if (row == 1) annot = "; font '0'";
    else if (row == 2) annot = "; font '1'";
    DrawText_(annot, left + 276, y, 12, Color{85, 100, 125, 255});

    y += 18;
  }
  y += 12;

  // 4. HEX KEYPAD MATRIX (16-KEY COSMAC VIP)
  DrawText_("HEX KEYPAD MATRIX (16-KEY COSMAC VIP)", left, y, 12, kDim);
  y += 18;

  constexpr int kPadLayout[16] = {0x1, 0x2, 0x3, 0xC, 0x4, 0x5, 0x6, 0xD,
                                  0x7, 0x8, 0x9, 0xE, 0xA, 0x0, 0xB, 0xF};
  const int key_card_w = (kPanelW - 28 - (3 * 8)) / 4;
  const int key_card_h = 26;
  for (int i = 0; i < 16; ++i) {
    const int col = i % 4;
    const int row = i / 4;
    const int kx = left + col * (key_card_w + 8);
    const int ky = y + row * (key_card_h + 6);
    const int value = kPadLayout[i];
    const bool down = g_app.cpu.KeyPressed(value);

    DrawRectangle(kx, ky, key_card_w, key_card_h, down ? Color{126, 231, 195, 45} : Color{15, 19, 28, 255});
    DrawRectangleLines(kx, ky, key_card_w, key_card_h, down ? kAccent : Color{32, 42, 60, 255});

    const std::string key_label = Hex(value, 1) + " (" + PhysicalKeyFor(value) + ")";
    DrawText_(key_label, kx + 12, ky + 6, 13, down ? kAccent : kText);
  }
}

void DrawHelpBar() {
  const std::string help =
      g_app.show_debugger
          ? "H hide debugger   SPACE pause   N step   BACKSPACE reset   "
            "[ ] speed   F1-F5 quirks   keys 1234/QWER/ASDF/ZXCV"
          : "H show the debugger   SPACE pause   BACKSPACE reset   "
            "keys 1234/QWER/ASDF/ZXCV";
  const int x = g_app.show_debugger ? kDisplayX : kFocusX;
  DrawText_(help, x, kScreenHeight - 14, FitSize(help, kScreenWidth - 2 * x, 13, 9),
            kDim);
}

// Chip8::RunFrame already does this, but the debugger also needs to know which
// addresses the frame executed, not just where it ended up, so the loop is
// repeated here with the histogram folded in.
void RunTracedFrame() {
  for (float& hot : g_app.heat) hot *= kHeatDecay;

  for (int i = 0; i < g_app.speed; ++i) {
    g_app.heat[g_app.cpu.pc() & 0x0FFF] += 1.0f;

    const chip8::StepResult result = g_app.cpu.Step();
    // Same three dead ends RunFrame breaks on: nothing else this frame can
    // make progress, so spending the rest of the budget would just burn it.
    if (result == chip8::StepResult::kIllegalInstruction ||
        result == chip8::StepResult::kWaitingForKey ||
        result == chip8::StepResult::kWaitingForVBlank) {
      break;
    }
  }
  g_app.cpu.TickTimers();
}

void UpdateFrame() {
  const float dt = GetFrameTime();

  HandleInput();

  if (!g_app.paused && g_app.rom_loaded) {
    g_app.prev_v = g_app.cpu.v();
    RunTracedFrame();

    for (int i = 0; i < 16; ++i) {
      if (g_app.cpu.v()[i] != g_app.prev_v[i]) g_app.v_flash[i] = 0.35f;
    }
  }

  for (float& flash : g_app.v_flash) {
    flash = std::max(0.0f, flash - dt);
  }

  if (g_app.cpu.display_dirty) {
    UploadDisplay();
    g_app.cpu.display_dirty = false;
  }

  if (g_app.audio_ready) {
    if (g_app.cpu.beeping() && !IsSoundPlaying(g_app.beep)) {
      PlaySound(g_app.beep);
    } else if (!g_app.cpu.beeping() && IsSoundPlaying(g_app.beep)) {
      StopSound(g_app.beep);
    }
  }

  if (g_app.status_timer > 0.0f) g_app.status_timer -= dt;

  g_app.frame_count++;

  if (g_app.show_debugger) UpdateDisasmWindow();

  const Rect display = DisplayRect();

  BeginDrawing();
  ClearBackground(kBg);

  if (g_app.show_debugger) {
    DrawTopTelemetryBar();
    DrawDisplayPanel(display);
    DrawHintStrip(display);
    DrawDisassembly();
    DrawRegisterPanel();
  } else {
    DrawDisplayPanel(display);
    DrawHintStrip(display);
  }
  DrawHelpBar();

  if (g_app.status_timer > 0.0f) {
    DrawText_(g_app.status, display.x + 8, display.y + display.h - 26, 16,
              kWarn);
  }

  EndDrawing();
}

}  // namespace

#ifdef __EMSCRIPTEN__
// Called from the page when the user picks a file. Raylib's drag-and-drop does
// not reach the browser's file picker, so the bytes are handed over directly
// rather than going through the virtual filesystem.
extern "C" EMSCRIPTEN_KEEPALIVE void LoadRomFromMemory(const std::uint8_t* data,
                                                       int size,
                                                       const char* name) {
  if (g_app.cpu.LoadRom(data, static_cast<std::size_t>(size))) {
    g_app.rom_loaded = true;
    g_app.paused = false;
    g_app.rom_name = name != nullptr ? name : "uploaded ROM";
    g_app.rom_title = "LOADED";
    g_app.rom_controls = "controls depend on the ROM";
    g_app.prev_v = g_app.cpu.v();
    g_app.v_flash.fill(0.0f);
    g_app.disasm_top = chip8::kProgramStart;
    g_app.disasm_settle = 0;
    g_app.heat.fill(0.0f);
    SetStatus("loaded " + g_app.rom_name);
    UploadDisplay();
  } else {
    SetStatus("not a valid ROM (must be 1 to 3584 bytes)");
  }
}

// Lets the page drive the buttons under the canvas, so the controls work on a
// touchscreen where there is no keyboard to press.
extern "C" EMSCRIPTEN_KEEPALIVE void UiCommand(int command) {
  switch (command) {
    case 0: g_app.paused = !g_app.paused; break;
    case 1: LoadRomAt(g_app.rom_index + 1); break;
    case 2: LoadRomAt(g_app.rom_index); break;  // reset the current ROM
    case 3: g_app.show_debugger = !g_app.show_debugger; break;
    default: break;
  }
}
#endif

int main(int argc, char** argv) {
  SetTraceLogLevel(LOG_WARNING);
  InitWindow(kScreenWidth, kScreenHeight, "CHIP-8 - emulator and debugger");

#ifndef __EMSCRIPTEN__
  SetTargetFPS(60);
#else
  // Deliberately no target on the web. requestAnimationFrame is already the
  // clock, and asking raylib for one as well makes EndDrawing block in
  // WaitTime until the 16.67 ms budget is used up - which is only ever after
  // the browser's own next tick is due, so every second frame is missed and
  // the loop settles at 30 Hz. The CHIP-8 delay timer ticks once a frame, so
  // that halves the speed of every bundled ROM: Brix's ball crawls at 15
  // pixels a second instead of 30 and the paddle feels unresponsive.
#endif

  // Raylib's built-in font is a 10-pixel bitmap face. Everything here is drawn
  // between 13 and 22 pixels, so every glyph was being stretched by a
  // fractional amount over a point-filtered atlas - 1.3x, 1.5x, 1.6x - which
  // is why the text looked smeared no matter what size it was asked for. There
  // is no size that fixes it; the face has to be a real one.
  //
  // Baked at kFontAtlasSize, comfortably above the largest size drawn, so
  // every size in use is scaling the atlas down rather than up. Bilinear
  // because unlike the emulated display this is meant to be resampled.
  g_app.font = LoadFontEx("assets/JetBrainsMono-Regular.ttf", kFontAtlasSize,
                          nullptr, 0);
  if (g_app.font.texture.id == 0) {
    // Missing or unreadable: still better to run with bad text than not run.
    g_app.font = GetFontDefault();
    TraceLog(LOG_WARNING, "font not found, falling back to the built-in one");
  } else {
    SetTextureFilter(g_app.font.texture, TEXTURE_FILTER_BILINEAR);
  }

  Image blank = GenImageColor(chip8::kDisplayWidth, chip8::kDisplayHeight,
                              kOffPixel);
  g_app.screen = LoadTextureFromImage(blank);
  UnloadImage(blank);
  // Nearest-neighbour, or a 14x upscale turns every pixel into a smudge.
  SetTextureFilter(g_app.screen, TEXTURE_FILTER_POINT);

  InitAudioDevice();
  if (IsAudioDeviceReady()) {
    g_app.beep = MakeBeep();
    g_app.audio_ready = true;
  }

  if (argc > 1) {
    if (g_app.cpu.LoadRomFile(argv[1])) {
      g_app.rom_loaded = true;
      g_app.rom_name = GetFileName(argv[1]);
      g_app.rom_title = "LOADED";
      g_app.rom_controls = "controls depend on the ROM";
    }
  }
  if (!g_app.rom_loaded) LoadRomAt(0);
  UploadDisplay();

#ifdef __EMSCRIPTEN__
  // The browser owns the event loop, so raylib's WindowShouldClose loop cannot
  // be used: hand the per-frame function to Emscripten instead.
  emscripten_set_main_loop(UpdateFrame, 0, 1);
#else
  while (!WindowShouldClose()) UpdateFrame();
#endif

  if (g_app.audio_ready) UnloadSound(g_app.beep);
  CloseAudioDevice();
  UnloadTexture(g_app.screen);
  CloseWindow();
  return 0;
}
