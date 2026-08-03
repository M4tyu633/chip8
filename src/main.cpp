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
// crisp. 64 x 17 = 1088 is too wide next to the panel, so 14 it is.
constexpr int kPixelScale = 14;
constexpr int kDisplayX = 16;
constexpr int kDisplayY = 16;
constexpr int kDisplayW = chip8::kDisplayWidth * kPixelScale;   // 896
constexpr int kDisplayH = chip8::kDisplayHeight * kPixelScale;  // 448

constexpr int kPanelX = kDisplayX + kDisplayW + 16;  // 928
constexpr int kPanelW = kScreenWidth - kPanelX - 16;  // 336

constexpr int kDisasmY = kDisplayY + kDisplayH + 14;  // 478
constexpr int kDisasmH = kScreenHeight - kDisasmY - 16;

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
};
constexpr KeyMap kKeyMap[16] = {
    {KEY_ONE, 0x1},   {KEY_TWO, 0x2}, {KEY_THREE, 0x3}, {KEY_FOUR, 0xC},
    {KEY_Q, 0x4},     {KEY_W, 0x5},   {KEY_E, 0x6},     {KEY_R, 0xD},
    {KEY_A, 0x7},     {KEY_S, 0x8},   {KEY_D, 0x9},     {KEY_F, 0xE},
    {KEY_Z, 0xA},     {KEY_X, 0x0},   {KEY_C, 0xB},     {KEY_V, 0xF},
};

// ---------------------------------------------------------------------------

struct App {
  chip8::Chip8 cpu;
  Texture2D screen{};
  Font font{};

  bool paused = false;
  bool rom_loaded = false;
  int speed = kDefaultSpeed;
  std::string rom_name = "no ROM";
  std::string status;
  float status_timer = 0.0f;

  // Previous register values, so a write can be highlighted for a moment.
  std::array<std::uint8_t, chip8::kRegisterCount> prev_v{};
  std::array<float, chip8::kRegisterCount> v_flash{};

  Sound beep{};
  bool audio_ready = false;

  std::vector<std::string> rom_paths;
  int rom_index = 0;
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
  if (g_app.rom_paths.empty()) return;
  const int count = static_cast<int>(g_app.rom_paths.size());
  g_app.rom_index = ((index % count) + count) % count;

  const std::string& path = g_app.rom_paths[g_app.rom_index];
  if (g_app.cpu.LoadRomFile(path)) {
    g_app.rom_loaded = true;
    g_app.paused = false;
    g_app.rom_name = GetFileName(path.c_str());
    g_app.prev_v = g_app.cpu.v();
    g_app.v_flash.fill(0.0f);
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
        SetStatus("loaded " + g_app.rom_name);
      } else {
        SetStatus("not a valid ROM");
      }
    }
    UnloadDroppedFiles(dropped);
  }
}

void DrawDisplayPanel() {
  DrawRectangle(kDisplayX - 2, kDisplayY - 2, kDisplayW + 4, kDisplayH + 4,
                kGrid);
  DrawTexturePro(g_app.screen,
                 Rectangle{0, 0, static_cast<float>(chip8::kDisplayWidth),
                           static_cast<float>(chip8::kDisplayHeight)},
                 Rectangle{static_cast<float>(kDisplayX),
                           static_cast<float>(kDisplayY),
                           static_cast<float>(kDisplayW),
                           static_cast<float>(kDisplayH)},
                 Vector2{0, 0}, 0.0f, WHITE);

  if (!g_app.rom_loaded) {
    DrawText_("drop a .ch8 file here, or press TAB to cycle the bundled ROMs",
              kDisplayX + 60, kDisplayY + kDisplayH / 2 - 8, 18, kDim);
  }
}

void DrawDisassembly() {
  DrawRectangle(kDisplayX - 2, kDisasmY, kDisplayW + 4, kDisasmH, kPanelBg);
  DrawText_("DISASSEMBLY", kDisplayX + 10, kDisasmY + 8, 14, kDim);

  // Centre the listing on the PC. CHIP-8 instructions are a fixed two bytes,
  // so unlike a variable-length ISA the listing can simply be walked
  // backwards from the PC without guessing where instructions start.
  const int rows = (kDisasmH - 34) / 18;
  const int before = rows / 2;
  int address = static_cast<int>(g_app.cpu.pc()) - before * 2;
  if (address < 0) address = 0;

  for (int row = 0; row < rows; ++row) {
    const int at = address + row * 2;
    if (at < 0 || at + 1 >= chip8::kMemorySize) continue;

    const std::uint16_t opcode =
        static_cast<std::uint16_t>((g_app.cpu.memory()[at] << 8) |
                                   g_app.cpu.memory()[at + 1]);
    const bool current = at == static_cast<int>(g_app.cpu.pc());
    const int y = kDisasmY + 30 + row * 18;

    if (current) {
      DrawRectangle(kDisplayX + 4, y - 2, kDisplayW - 8, 18,
                    Color{126, 231, 195, 28});
      DrawText_(">", kDisplayX + 10, y, 15, kAccent);
    }

    const Color colour = current ? kAccent : kDim;
    DrawText_(Hex(at, 3), kDisplayX + 28, y, 15, colour);
    DrawText_(Hex(opcode, 4), kDisplayX + 84, y, 15,
              current ? kAccent : kText);
    DrawText_(chip8::Chip8::Disassemble(opcode), kDisplayX + 150, y, 15,
              colour);
  }
}

void DrawRegisterPanel() {
  int y = kDisplayY;
  DrawRectangle(kPanelX, y, kPanelW, kScreenHeight - 32, kPanelBg);

  const int left = kPanelX + 14;
  y += 12;

  DrawText_("REGISTERS", left, y, 14, kDim);
  y += 22;

  // V0 to VF in two columns of eight.
  for (int i = 0; i < 16; ++i) {
    const int column = i / 8;
    const int row = i % 8;
    const int x = left + column * 150;
    const int ry = y + row * 20;

    const bool flashing = g_app.v_flash[i] > 0.0f;
    DrawText_("V" + Hex(i, 1), x, ry, 15, kDim);
    DrawText_(Hex(g_app.cpu.v()[i], 2), x + 30, ry, 15,
              flashing ? kChanged : kText);
    DrawText_(std::to_string(g_app.cpu.v()[i]), x + 62, ry, 14, kDim);
  }
  y += 8 * 20 + 12;

  DrawLine(left, y, kPanelX + kPanelW - 14, y, kGrid);
  y += 12;

  struct Row {
    const char* label;
    std::string value;
    Color colour;
  };
  const std::vector<Row> rows = {
      {"PC", Hex(g_app.cpu.pc(), 3), kAccent},
      {"I", Hex(g_app.cpu.index(), 3), kText},
      {"SP", Hex(g_app.cpu.sp(), 2), kText},
      {"DT", Hex(g_app.cpu.delay_timer(), 2),
       g_app.cpu.delay_timer() ? kWarn : kText},
      {"ST", Hex(g_app.cpu.sound_timer(), 2),
       g_app.cpu.sound_timer() ? kWarn : kText},
  };
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const int x = left + static_cast<int>(i % 3) * 110;
    const int ry = y + static_cast<int>(i / 3) * 20;
    DrawText_(rows[i].label, x, ry, 15, kDim);
    DrawText_(rows[i].value, x + 30, ry, 15, rows[i].colour);
  }
  y += 2 * 20 + 12;

  DrawLine(left, y, kPanelX + kPanelW - 14, y, kGrid);
  y += 12;

  DrawText_("NEXT", left, y, 14, kDim);
  const std::uint16_t next = static_cast<std::uint16_t>(
      (g_app.cpu.memory()[g_app.cpu.pc() & 0x0FFF] << 8) |
      g_app.cpu.memory()[(g_app.cpu.pc() + 1) & 0x0FFF]);
  DrawText_(Hex(next, 4) + "  " + chip8::Chip8::Disassemble(next), left + 54, y,
            15, kText);
  y += 26;

  DrawText_("STACK", left, y, 14, kDim);
  y += 20;
  if (g_app.cpu.sp() == 0) {
    DrawText_("empty", left, y, 15, kDim);
    y += 20;
  } else {
    for (int i = 0; i < g_app.cpu.sp() && i < 8; ++i) {
      const int x = left + (i % 4) * 78;
      const int ry = y + (i / 4) * 20;
      DrawText_(Hex(i, 1) + ":" + Hex(g_app.cpu.stack()[i], 3), x, ry, 15,
                kText);
    }
    y += ((g_app.cpu.sp() + 3) / 4) * 20;
  }
  y += 8;

  DrawLine(left, y, kPanelX + kPanelW - 14, y, kGrid);
  y += 12;

  // Memory around I, which is almost always what you want to see: it is where
  // the next sprite, BCD result or register block lives.
  DrawText_("MEMORY @ I", left, y, 14, kDim);
  y += 20;
  const int base = (static_cast<int>(g_app.cpu.index()) / 8) * 8;
  for (int row = 0; row < 4; ++row) {
    const int address = base + row * 8;
    if (address + 7 >= chip8::kMemorySize) break;

    DrawText_(Hex(address, 3), left, y, 14, kDim);
    for (int i = 0; i < 8; ++i) {
      const bool at_index = (address + i) == g_app.cpu.index();
      DrawText_(Hex(g_app.cpu.memory()[address + i], 2), left + 46 + i * 28, y,
                14, at_index ? kAccent : kText);
    }
    y += 18;
  }
  y += 10;

  DrawLine(left, y, kPanelX + kPanelW - 14, y, kGrid);
  y += 12;

  // The 16-key pad, lit to match what the core currently believes is held.
  DrawText_("KEYPAD", left, y, 14, kDim);
  y += 20;
  constexpr int kPadLayout[16] = {0x1, 0x2, 0x3, 0xC, 0x4, 0x5, 0x6, 0xD,
                                  0x7, 0x8, 0x9, 0xE, 0xA, 0x0, 0xB, 0xF};
  for (int i = 0; i < 16; ++i) {
    const int x = left + (i % 4) * 34;
    const int ry = y + (i / 4) * 30;
    const int value = kPadLayout[i];
    const bool down = g_app.cpu.KeyPressed(value);
    DrawRectangle(x, ry, 28, 24, down ? kAccent : kGrid);
    DrawText_(Hex(value, 1), x + 10, ry + 4, 15, down ? kBg : kDim);
  }
  y += 4 * 30 + 6;

  // Status line at the bottom of the panel.
  const int bottom = kScreenHeight - 32 - 46;
  DrawText_(g_app.rom_name, left, bottom, 14, kDim);
  const std::string state = g_app.cpu.halted() ? "HALTED"
                            : g_app.paused     ? "PAUSED"
                                               : "RUNNING";
  DrawText_(state + "   " + std::to_string(g_app.speed) + "/frame", left,
            bottom + 18, 14,
            g_app.cpu.halted() ? kBad : (g_app.paused ? kWarn : kAccent));
}

void DrawHelpBar() {
  const std::string help =
      "SPACE pause   N step   TAB next ROM   BACKSPACE reset   [ ] speed   "
      "F1-F5 quirks   keys 1234/QWER/ASDF/ZXCV";
  DrawText_(help, kDisplayX, kScreenHeight - 14, 13, kDim);
}

void UpdateFrame() {
  const float dt = GetFrameTime();

  HandleInput();

  if (!g_app.paused && g_app.rom_loaded) {
    g_app.prev_v = g_app.cpu.v();
    g_app.cpu.RunFrame(g_app.speed);

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

  BeginDrawing();
  ClearBackground(kBg);

  DrawDisplayPanel();
  DrawDisassembly();
  DrawRegisterPanel();
  DrawHelpBar();

  if (g_app.status_timer > 0.0f) {
    DrawText_(g_app.status, kDisplayX + 8, kDisplayY + kDisplayH - 26, 16,
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
    g_app.prev_v = g_app.cpu.v();
    g_app.v_flash.fill(0.0f);
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
    default: break;
  }
}
#endif

int main(int argc, char** argv) {
  SetTraceLogLevel(LOG_WARNING);
  InitWindow(kScreenWidth, kScreenHeight, "CHIP-8 - emulator and debugger");
  SetTargetFPS(60);

  // The default font is a bitmap face that turns to mush at these sizes, so
  // ask for the built-in one at a size that stays legible.
  g_app.font = GetFontDefault();

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

  g_app.rom_paths = {"roms/bounce.ch8", "roms/counter.ch8", "roms/keypad.ch8"};

  if (argc > 1) {
    if (g_app.cpu.LoadRomFile(argv[1])) {
      g_app.rom_loaded = true;
      g_app.rom_name = GetFileName(argv[1]);
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
