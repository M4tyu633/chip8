#include "chip8.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace chip8 {
namespace {

// The standard 4x5 hexadecimal font. Fx29 points I at the glyph for a digit,
// so these must stay in order 0 through F. Each row uses only the high nibble
// because a glyph is four pixels wide.
constexpr std::uint8_t kFont[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0,  // 0
    0x20, 0x60, 0x20, 0x20, 0x70,  // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0,  // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0,  // 3
    0x90, 0x90, 0xF0, 0x10, 0x10,  // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0,  // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0,  // 6
    0xF0, 0x10, 0x20, 0x40, 0x40,  // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0,  // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0,  // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90,  // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0,  // B
    0xF0, 0x80, 0x80, 0x80, 0xF0,  // C
    0xE0, 0x90, 0x90, 0x90, 0xE0,  // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0,  // E
    0xF0, 0x80, 0xF0, 0x80, 0x80,  // F
};

}  // namespace

Chip8::Chip8() { Reset(); }

void Chip8::Reset() {
  memory_.fill(0);
  display_.fill(0);
  v_.fill(0);
  stack_.fill(0);
  keys_.fill(false);

  std::memcpy(memory_.data() + kFontStart, kFont, sizeof(kFont));

  pc_ = kProgramStart;
  index_ = 0;
  sp_ = 0;
  delay_timer_ = 0;
  sound_timer_ = 0;

  halted_ = false;
  last_result_ = StepResult::kOk;
  waiting_for_key_ = false;
  key_destination_ = 0;
  pending_key_ = -1;
  drew_this_frame_ = false;

  last_opcode_ = 0;
  last_pc_ = kProgramStart;
  instructions_ = 0;
  display_dirty = true;
}

bool Chip8::LoadRom(const std::uint8_t* data, std::size_t size) {
  // Checked before Reset so that a rejected ROM leaves the previous one
  // running rather than wiping the machine.
  if (data == nullptr || size == 0 || size > kMaxRomSize) return false;

  Reset();
  std::memcpy(memory_.data() + kProgramStart, data, size);
  return true;
}

bool Chip8::LoadRomFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return false;

  const std::streamsize size = file.tellg();
  if (size <= 0 || static_cast<std::size_t>(size) > kMaxRomSize) return false;

  file.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) return false;

  return LoadRom(buffer.data(), buffer.size());
}

void Chip8::SetRandomSeed(std::uint32_t seed) {
  // Zero is a fixed point of xorshift, so it would produce nothing but zeroes.
  rng_state_ = seed != 0 ? seed : 0x2545F491u;
}

std::uint8_t Chip8::NextRandom() {
  // xorshift32. Small, no library dependency, and identical on every target,
  // which matters because the WebAssembly build has to match the desktop one
  // for a seeded run to be reproducible.
  rng_state_ ^= rng_state_ << 13;
  rng_state_ ^= rng_state_ >> 17;
  rng_state_ ^= rng_state_ << 5;
  return static_cast<std::uint8_t>(rng_state_ >> 24);
}

void Chip8::SetKey(int key, bool pressed) {
  if (key < 0 || key >= static_cast<int>(kKeyCount)) return;

  const bool was_pressed = keys_[key];
  keys_[key] = pressed;

  if (!waiting_for_key_) return;

  // Fx0A completes on release, not press. Waiting for the release is what
  // stops a single long keypress from satisfying several Fx0A instructions in
  // a row, which is how the original hardware behaved and what menu-driven
  // ROMs rely on.
  if (pressed && pending_key_ < 0) {
    pending_key_ = key;
  } else if (!pressed && was_pressed && pending_key_ == key) {
    v_[key_destination_] = static_cast<std::uint8_t>(key);
    waiting_for_key_ = false;
    pending_key_ = -1;
  }
}

bool Chip8::KeyPressed(int key) const {
  if (key < 0 || key >= static_cast<int>(kKeyCount)) return false;
  return keys_[key];
}

void Chip8::TickTimers() {
  if (delay_timer_ > 0) --delay_timer_;
  if (sound_timer_ > 0) --sound_timer_;
  // A new frame has started, so a draw held back by display_wait can proceed.
  drew_this_frame_ = false;
}

std::uint16_t Chip8::Fetch() {
  // Big-endian: every CHIP-8 instruction is two bytes, high byte first.
  const std::uint16_t high = memory_[pc_ & 0x0FFF];
  const std::uint16_t low = memory_[(pc_ + 1) & 0x0FFF];
  pc_ = static_cast<std::uint16_t>((pc_ + 2) & 0x0FFF);
  return static_cast<std::uint16_t>((high << 8) | low);
}

StepResult Chip8::Step() {
  if (halted_) return StepResult::kIllegalInstruction;

  // Fx0A parks the CPU. The program counter has already moved past the
  // instruction, so simply returning here re-enters the wait each step until
  // SetKey clears the flag.
  if (waiting_for_key_) return StepResult::kWaitingForKey;

  const std::uint16_t pc_before = pc_;
  const std::uint16_t opcode = Fetch();

  last_pc_ = pc_before;
  last_opcode_ = opcode;
  last_result_ = StepResult::kOk;

  ExecuteOpcode(opcode);

  if (last_result_ == StepResult::kWaitingForVBlank) {
    // The draw did not happen. Rewind so the same Dxyn runs again next frame.
    pc_ = pc_before;
    return StepResult::kWaitingForVBlank;
  }

  ++instructions_;

  if (halted_) return StepResult::kIllegalInstruction;
  if (waiting_for_key_) return StepResult::kWaitingForKey;
  return StepResult::kOk;
}

void Chip8::RunFrame(int instructions) {
  for (int i = 0; i < instructions; ++i) {
    const StepResult result = Step();
    // Every one of these means further stepping this frame cannot make
    // progress, so spending the rest of the budget would just burn cycles.
    if (result == StepResult::kIllegalInstruction ||
        result == StepResult::kWaitingForKey ||
        result == StepResult::kWaitingForVBlank) {
      break;
    }
  }
  TickTimers();
}

void Chip8::DrawSprite(std::uint8_t vx, std::uint8_t vy, std::uint8_t height) {
  // The starting position wraps even when the sprite body is clipped. This
  // asymmetry is real hardware behaviour, not an oversight.
  const int start_x = vx % kDisplayWidth;
  const int start_y = vy % kDisplayHeight;

  v_[0xF] = 0;

  for (int row = 0; row < height; ++row) {
    const int y = start_y + row;
    if (quirks.clip_sprites) {
      if (y >= kDisplayHeight) break;
    }
    const int wrapped_y = y % kDisplayHeight;

    const std::uint8_t sprite_byte = memory_[(index_ + row) & 0x0FFF];

    for (int bit = 0; bit < 8; ++bit) {
      // Sprites are drawn most significant bit first, so bit 7 is the
      // leftmost pixel.
      if ((sprite_byte & (0x80 >> bit)) == 0) continue;

      const int x = start_x + bit;
      if (quirks.clip_sprites) {
        if (x >= kDisplayWidth) break;
      }
      const int wrapped_x = x % kDisplayWidth;

      std::uint8_t& pixel = display_[wrapped_y * kDisplayWidth + wrapped_x];
      // VF is the collision flag: set when a lit pixel is turned off. It is
      // set once for the whole sprite, never cleared mid-draw.
      if (pixel) v_[0xF] = 1;
      pixel ^= 1;
    }
  }

  display_dirty = true;
}

void Chip8::ExecuteOpcode(std::uint16_t opcode) {
  // Standard field names from the CHIP-8 reference: nnn is a 12-bit address,
  // kk an 8-bit immediate, n a 4-bit immediate, x and y register indices.
  const std::uint16_t nnn = opcode & 0x0FFF;
  const std::uint8_t kk = static_cast<std::uint8_t>(opcode & 0x00FF);
  const std::uint8_t n = static_cast<std::uint8_t>(opcode & 0x000F);
  const std::uint8_t x = static_cast<std::uint8_t>((opcode & 0x0F00) >> 8);
  const std::uint8_t y = static_cast<std::uint8_t>((opcode & 0x00F0) >> 4);

  switch (opcode & 0xF000) {
    case 0x0000:
      switch (opcode) {
        case 0x00E0:  // CLS
          display_.fill(0);
          display_dirty = true;
          break;
        case 0x00EE:  // RET
          if (sp_ == 0) {
            halted_ = true;  // stack underflow
            break;
          }
          pc_ = stack_[--sp_];
          break;
        default:
          // 0nnn (SYS) called machine code on real hardware. There is no
          // machine to call, and no surviving ROM needs it, so treat it as a
          // no-op rather than halting on ROMs that pad with zeroes.
          break;
      }
      break;

    case 0x1000:  // JP nnn
      pc_ = nnn;
      break;

    case 0x2000:  // CALL nnn
      if (sp_ >= kStackSize) {
        halted_ = true;  // stack overflow
        break;
      }
      stack_[sp_++] = pc_;
      pc_ = nnn;
      break;

    case 0x3000:  // SE Vx, kk
      if (v_[x] == kk) pc_ = (pc_ + 2) & 0x0FFF;
      break;

    case 0x4000:  // SNE Vx, kk
      if (v_[x] != kk) pc_ = (pc_ + 2) & 0x0FFF;
      break;

    case 0x5000:  // SE Vx, Vy
      if (n != 0) {
        halted_ = true;
        break;
      }
      if (v_[x] == v_[y]) pc_ = (pc_ + 2) & 0x0FFF;
      break;

    case 0x6000:  // LD Vx, kk
      v_[x] = kk;
      break;

    case 0x7000:  // ADD Vx, kk  (no carry flag, by design)
      v_[x] = static_cast<std::uint8_t>(v_[x] + kk);
      break;

    case 0x8000:
      switch (n) {
        case 0x0:  // LD Vx, Vy
          v_[x] = v_[y];
          break;
        case 0x1:  // OR
          v_[x] |= v_[y];
          if (quirks.vf_reset) v_[0xF] = 0;
          break;
        case 0x2:  // AND
          v_[x] &= v_[y];
          if (quirks.vf_reset) v_[0xF] = 0;
          break;
        case 0x3:  // XOR
          v_[x] ^= v_[y];
          if (quirks.vf_reset) v_[0xF] = 0;
          break;
        case 0x4: {  // ADD Vx, Vy  (VF = carry)
          const std::uint16_t sum =
              static_cast<std::uint16_t>(v_[x]) + static_cast<std::uint16_t>(v_[y]);
          v_[x] = static_cast<std::uint8_t>(sum);
          // VF is written last: when x is 0xF the flag must win, not the sum.
          v_[0xF] = sum > 0xFF ? 1 : 0;
          break;
        }
        case 0x5: {  // SUB Vx, Vy  (VF = NOT borrow)
          const std::uint8_t flag = v_[x] >= v_[y] ? 1 : 0;
          v_[x] = static_cast<std::uint8_t>(v_[x] - v_[y]);
          v_[0xF] = flag;
          break;
        }
        case 0x6: {  // SHR
          const std::uint8_t source = quirks.shift_uses_vy ? v_[y] : v_[x];
          const std::uint8_t flag = source & 0x1;
          v_[x] = static_cast<std::uint8_t>(source >> 1);
          v_[0xF] = flag;
          break;
        }
        case 0x7: {  // SUBN Vx, Vy  (VF = NOT borrow)
          const std::uint8_t flag = v_[y] >= v_[x] ? 1 : 0;
          v_[x] = static_cast<std::uint8_t>(v_[y] - v_[x]);
          v_[0xF] = flag;
          break;
        }
        case 0xE: {  // SHL
          const std::uint8_t source = quirks.shift_uses_vy ? v_[y] : v_[x];
          const std::uint8_t flag = (source & 0x80) ? 1 : 0;
          v_[x] = static_cast<std::uint8_t>(source << 1);
          v_[0xF] = flag;
          break;
        }
        default:
          halted_ = true;
          break;
      }
      break;

    case 0x9000:  // SNE Vx, Vy
      if (n != 0) {
        halted_ = true;
        break;
      }
      if (v_[x] != v_[y]) pc_ = (pc_ + 2) & 0x0FFF;
      break;

    case 0xA000:  // LD I, nnn
      index_ = nnn;
      break;

    case 0xB000:  // JP V0, nnn  /  JP Vx, xnn
      pc_ = static_cast<std::uint16_t>(
          (nnn + (quirks.jump_uses_v0 ? v_[0] : v_[x])) & 0x0FFF);
      break;

    case 0xC000:  // RND Vx, kk
      v_[x] = static_cast<std::uint8_t>(NextRandom() & kk);
      break;

    case 0xD000:  // DRW Vx, Vy, n
      if (quirks.display_wait && drew_this_frame_) {
        // Signal to Step, which rewinds the PC so this instruction is retried
        // on the next frame rather than being skipped.
        last_result_ = StepResult::kWaitingForVBlank;
        break;
      }
      DrawSprite(v_[x], v_[y], n);
      drew_this_frame_ = true;
      break;

    case 0xE000:
      switch (kk) {
        case 0x9E:  // SKP Vx
          if (KeyPressed(v_[x] & 0x0F)) pc_ = (pc_ + 2) & 0x0FFF;
          break;
        case 0xA1:  // SKNP Vx
          if (!KeyPressed(v_[x] & 0x0F)) pc_ = (pc_ + 2) & 0x0FFF;
          break;
        default:
          halted_ = true;
          break;
      }
      break;

    case 0xF000:
      switch (kk) {
        case 0x07:  // LD Vx, DT
          v_[x] = delay_timer_;
          break;
        case 0x0A:  // LD Vx, K  (blocks until a key is pressed and released)
          waiting_for_key_ = true;
          key_destination_ = x;
          pending_key_ = -1;
          break;
        case 0x15:  // LD DT, Vx
          delay_timer_ = v_[x];
          break;
        case 0x18:  // LD ST, Vx
          sound_timer_ = v_[x];
          break;
        case 0x1E:  // ADD I, Vx
          index_ = static_cast<std::uint16_t>((index_ + v_[x]) & 0x0FFF);
          break;
        case 0x29:  // LD F, Vx  (point I at the font glyph for a digit)
          index_ = static_cast<std::uint16_t>(kFontStart + (v_[x] & 0x0F) * 5);
          break;
        case 0x33: {  // LD B, Vx  (binary-coded decimal, hundreds first)
          const std::uint8_t value = v_[x];
          memory_[index_ & 0x0FFF] = static_cast<std::uint8_t>(value / 100);
          memory_[(index_ + 1) & 0x0FFF] =
              static_cast<std::uint8_t>((value / 10) % 10);
          memory_[(index_ + 2) & 0x0FFF] = static_cast<std::uint8_t>(value % 10);
          break;
        }
        case 0x55:  // LD [I], Vx  (store V0..Vx)
          for (int i = 0; i <= x; ++i) {
            memory_[(index_ + i) & 0x0FFF] = v_[i];
          }
          if (quirks.memory_increment) {
            index_ = static_cast<std::uint16_t>((index_ + x + 1) & 0x0FFF);
          }
          break;
        case 0x65:  // LD Vx, [I]  (load V0..Vx)
          for (int i = 0; i <= x; ++i) {
            v_[i] = memory_[(index_ + i) & 0x0FFF];
          }
          if (quirks.memory_increment) {
            index_ = static_cast<std::uint16_t>((index_ + x + 1) & 0x0FFF);
          }
          break;
        default:
          halted_ = true;
          break;
      }
      break;

    default:
      halted_ = true;
      break;
  }
}

std::string Chip8::Disassemble(std::uint16_t opcode) {
  const std::uint16_t nnn = opcode & 0x0FFF;
  const std::uint8_t kk = static_cast<std::uint8_t>(opcode & 0x00FF);
  const std::uint8_t n = static_cast<std::uint8_t>(opcode & 0x000F);
  const std::uint8_t x = static_cast<std::uint8_t>((opcode & 0x0F00) >> 8);
  const std::uint8_t y = static_cast<std::uint8_t>((opcode & 0x00F0) >> 4);

  char buffer[32];
  auto fmt = [&buffer](const char* format, auto... args) -> std::string {
    std::snprintf(buffer, sizeof(buffer), format, args...);
    return std::string(buffer);
  };

  switch (opcode & 0xF000) {
    case 0x0000:
      if (opcode == 0x00E0) return "CLS";
      if (opcode == 0x00EE) return "RET";
      return fmt("SYS  %03X", nnn);
    case 0x1000: return fmt("JP   %03X", nnn);
    case 0x2000: return fmt("CALL %03X", nnn);
    case 0x3000: return fmt("SE   V%X,%02X", x, kk);
    case 0x4000: return fmt("SNE  V%X,%02X", x, kk);
    case 0x5000: return fmt("SE   V%X,V%X", x, y);
    case 0x6000: return fmt("LD   V%X,%02X", x, kk);
    case 0x7000: return fmt("ADD  V%X,%02X", x, kk);
    case 0x8000:
      switch (n) {
        case 0x0: return fmt("LD   V%X,V%X", x, y);
        case 0x1: return fmt("OR   V%X,V%X", x, y);
        case 0x2: return fmt("AND  V%X,V%X", x, y);
        case 0x3: return fmt("XOR  V%X,V%X", x, y);
        case 0x4: return fmt("ADD  V%X,V%X", x, y);
        case 0x5: return fmt("SUB  V%X,V%X", x, y);
        case 0x6: return fmt("SHR  V%X,V%X", x, y);
        case 0x7: return fmt("SUBN V%X,V%X", x, y);
        case 0xE: return fmt("SHL  V%X,V%X", x, y);
        default:  return fmt("??   %04X", opcode);
      }
    case 0x9000: return fmt("SNE  V%X,V%X", x, y);
    case 0xA000: return fmt("LD   I,%03X", nnn);
    case 0xB000: return fmt("JP   V0,%03X", nnn);
    case 0xC000: return fmt("RND  V%X,%02X", x, kk);
    case 0xD000: return fmt("DRW  V%X,V%X,%X", x, y, n);
    case 0xE000:
      if (kk == 0x9E) return fmt("SKP  V%X", x);
      if (kk == 0xA1) return fmt("SKNP V%X", x);
      return fmt("??   %04X", opcode);
    case 0xF000:
      switch (kk) {
        case 0x07: return fmt("LD   V%X,DT", x);
        case 0x0A: return fmt("LD   V%X,K", x);
        case 0x15: return fmt("LD   DT,V%X", x);
        case 0x18: return fmt("LD   ST,V%X", x);
        case 0x1E: return fmt("ADD  I,V%X", x);
        case 0x29: return fmt("LD   F,V%X", x);
        case 0x33: return fmt("LD   B,V%X", x);
        case 0x55: return fmt("LD   [I],V%X", x);
        case 0x65: return fmt("LD   V%X,[I]", x);
        default:   return fmt("??   %04X", opcode);
      }
    default: return fmt("??   %04X", opcode);
  }
}

}  // namespace chip8
