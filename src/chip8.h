// CHIP-8 interpreter core.
//
// Deliberately free of any dependency on Raylib or on the platform. The core
// only ever touches its own state, so the same object can be driven by the
// desktop frontend, by the WebAssembly build, or by the test harness with no
// changes. Anything that has to reach outside (drawing, sound, key state) is
// exposed as data the frontend reads.

#ifndef CHIP8_H
#define CHIP8_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chip8 {

constexpr int kDisplayWidth = 64;
constexpr int kDisplayHeight = 32;
constexpr int kDisplayPixels = kDisplayWidth * kDisplayHeight;

constexpr std::uint16_t kMemorySize = 4096;
constexpr std::uint16_t kProgramStart = 0x200;  // where a ROM is loaded
constexpr std::uint16_t kFontStart = 0x050;     // conventional font address
constexpr std::size_t kStackSize = 16;
constexpr std::size_t kRegisterCount = 16;
constexpr std::size_t kKeyCount = 16;

// The original interpreter lived in the low 512 bytes, so a ROM can never be
// larger than what is left above it.
constexpr std::size_t kMaxRomSize = kMemorySize - kProgramStart;

// Hardware behaviours that real CHIP-8 implementations disagreed on. Programs
// were written against one specific interpreter, so a ROM that renders
// correctly under one set of rules can be unplayable under another. Every
// option below is set to the original COSMAC VIP behaviour, which is what the
// bundled ROMs assume.
struct Quirks {
  // 8xy1/8xy2/8xy3 (OR/AND/XOR) reset VF to zero as a side effect.
  bool vf_reset = true;
  // Fx55/Fx65 leave I incremented past the last register touched.
  bool memory_increment = true;
  // 8xy6/8xyE shift Vy into Vx. When false they shift Vx in place, which is
  // what most later interpreters and nearly all modern ROMs expect.
  bool shift_uses_vy = true;
  // Dxyn stalls until the next vertical blank, capping draws at one per frame.
  // This is what stops fast programs from tearing on original hardware.
  bool display_wait = true;
  // A sprite that runs off an edge is cut off rather than wrapping around.
  bool clip_sprites = true;
  // Bnnn jumps to nnn + V0. When false, Bxnn jumps to xnn + Vx (a SUPER-CHIP
  // reinterpretation of the same opcode).
  bool jump_uses_v0 = true;
};

// Why a step ended. The frontend uses this to decide whether to keep running
// and what to show the user.
enum class StepResult {
  kOk,
  kWaitingForKey,        // Fx0A parked the CPU until a key is released
  kWaitingForVBlank,     // display_wait held a draw back to the next frame
  kIllegalInstruction,   // decoded to nothing; the CPU has halted
};

class Chip8 {
 public:
  Chip8();

  // Clears memory, registers, the display and the stack, then reloads the
  // font. Any ROM already loaded is lost.
  void Reset();

  // Copies a ROM to 0x200 and resets first. Returns false and changes nothing
  // if the ROM is empty or too large to fit.
  bool LoadRom(const std::uint8_t* data, std::size_t size);
  bool LoadRomFile(const std::string& path);

  // Executes one instruction. Returns why it stopped. Safe to call after a
  // halt: it becomes a no-op rather than running off the end of memory.
  StepResult Step();

  // Runs up to `instructions` steps, then ticks the timers once. Call this
  // once per rendered frame with the ROM's instructions-per-frame budget.
  // Stops early on a halt or a wait, so a paused CPU does not burn the budget.
  void RunFrame(int instructions);

  // Decrements the delay and sound timers, both at 60 Hz, and releases a
  // display_wait stall. Called by RunFrame; call it directly only if you are
  // stepping instructions by hand.
  void TickTimers();

  // Key state is edge-sensitive: Fx0A completes on key *release*, not press,
  // so the frontend must report both transitions rather than a level.
  void SetKey(int key, bool pressed);
  bool KeyPressed(int key) const;

  // --- state the frontend renders -----------------------------------------
  // One byte per pixel, 0 or 1, row-major. A byte rather than a bitfield
  // because the debugger and the texture upload both want random access more
  // than they want the 8x saving.
  const std::array<std::uint8_t, kDisplayPixels>& display() const {
    return display_;
  }
  const std::array<std::uint8_t, kMemorySize>& memory() const {
    return memory_;
  }
  const std::array<std::uint8_t, kRegisterCount>& v() const { return v_; }
  const std::array<std::uint16_t, kStackSize>& stack() const { return stack_; }

  std::uint16_t pc() const { return pc_; }
  std::uint16_t index() const { return index_; }
  std::uint8_t sp() const { return sp_; }
  std::uint8_t delay_timer() const { return delay_timer_; }
  std::uint8_t sound_timer() const { return sound_timer_; }
  bool halted() const { return halted_; }
  bool beeping() const { return sound_timer_ > 0; }
  std::uint64_t instructions_executed() const { return instructions_; }

  // The opcode the last Step actually ran, and where it ran from. Both are
  // for the debugger; neither affects execution.
  std::uint16_t last_opcode() const { return last_opcode_; }
  std::uint16_t last_pc() const { return last_pc_; }

  // Set true whenever the display changed, so the frontend can skip
  // re-uploading an unchanged texture. The frontend clears it.
  bool display_dirty = false;

  Quirks quirks;

  // Disassembles one instruction for the debugger's listing. Pure formatting;
  // it never touches CPU state.
  static std::string Disassemble(std::uint16_t opcode);

  // Seeds the Cxkk random source. Fixing the seed makes a run reproducible,
  // which is the only way to test a ROM that uses RND.
  void SetRandomSeed(std::uint32_t seed);

 private:
  std::uint16_t Fetch();
  void ExecuteOpcode(std::uint16_t opcode);
  void DrawSprite(std::uint8_t vx, std::uint8_t vy, std::uint8_t height);
  std::uint8_t NextRandom();

  std::array<std::uint8_t, kMemorySize> memory_{};
  std::array<std::uint8_t, kDisplayPixels> display_{};
  std::array<std::uint8_t, kRegisterCount> v_{};
  std::array<std::uint16_t, kStackSize> stack_{};
  std::array<bool, kKeyCount> keys_{};

  std::uint16_t pc_ = kProgramStart;
  std::uint16_t index_ = 0;
  std::uint8_t sp_ = 0;
  std::uint8_t delay_timer_ = 0;
  std::uint8_t sound_timer_ = 0;

  bool halted_ = false;
  StepResult last_result_ = StepResult::kOk;

  // Fx0A: which register receives the key, and whether we have already seen
  // the press we are waiting to see released.
  bool waiting_for_key_ = false;
  std::uint8_t key_destination_ = 0;
  int pending_key_ = -1;

  // Dxyn under display_wait: set when a draw has happened this frame, cleared
  // by TickTimers.
  bool drew_this_frame_ = false;

  std::uint16_t last_opcode_ = 0;
  std::uint16_t last_pc_ = kProgramStart;
  std::uint64_t instructions_ = 0;

  // A small self-contained PRNG rather than <random>, so that a given seed
  // produces the same sequence on desktop and in the browser. Cxkk only needs
  // eight bits of noise, not statistical quality.
  std::uint32_t rng_state_ = 0x2545F491u;
};

}  // namespace chip8

#endif  // CHIP8_H
