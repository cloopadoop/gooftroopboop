#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "SNESSystem.h"
#include "snes9x/snes9x.h"
#include "snes9x/65c816.h"
#include "snes9x/apu/apu.h"
#include "snes9x/memmap.h"

namespace {
std::ofstream g_trace;
std::vector<unsigned> g_watches;
uint64_t g_instructions = 0;
unsigned g_frame = 0;
uint64_t g_rows = 0;

void Write32(std::ostream& output, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    output.put(static_cast<char>(value >> (8 * i)));
  }
}

struct Audio : SNESSoundOut {
  std::ofstream stream;
  uint32_t bytes = 0;
  explicit Audio(const char* path) : stream(path, std::ios::binary) {
    if (!stream) {
      throw std::runtime_error("Cannot create WAV");
    }
    stream.write("RIFF", 4);
    Write32(stream, 0);
    stream.write("WAVEfmt ", 8);
    Write32(stream, 16);
    Write32(stream, 0x00020001);
    Write32(stream, 32000);
    Write32(stream, 128000);
    Write32(stream, 0x00100004);
    stream.write("data", 4);
    Write32(stream, 0);
  }
  void write(const void* samples, unsigned long count) override {
    stream.write(static_cast<const char*>(samples), count);
    bytes += count;
  }
  void Finish() {
    stream.seekp(4);
    Write32(stream, bytes + 36);
    stream.seekp(40);
    Write32(stream, bytes);
    stream.flush();
    if (!stream) {
      throw std::runtime_error("WAV write failed");
    }
  }
};
}

// Called immediately before opcode fetch. Observation never reads memory-mapped I/O.
void PlaybackObserve() {
  if (++g_instructions > 500000000ULL) {
    throw std::runtime_error("Instruction limit exceeded");
  }
  for (auto address : g_watches) {
    if ((Registers.PBPC & 0xffffff) == address) {
      if (++g_rows > 1000000) {
        throw std::runtime_error("Trace limit exceeded");
      }
      g_trace << g_frame << ',' << g_instructions << ',' << std::hex << address << ','
              << Registers.A.W << ',' << Registers.X.W << ',' << Registers.Y.W << ','
              << Registers.S.W << ',' << Registers.D.W << ','
              << static_cast<unsigned>(Memory.RAM[0x10]) << ','
              << static_cast<unsigned>(Memory.RAM[0x11]) << ','
              << static_cast<unsigned>(Memory.RAM[0x12]) << std::dec << '\n';
    }
  }
}

int main(int argc, char** argv) {
  try {
    if (argc < 6) {
      throw std::runtime_error("Usage: cpu_playback ROM WAV TRACE FRAMES CHANNEL_MASK [PC_HEX ...]");
    }
    const auto frames = std::stoul(argv[4]);
    const auto mask = std::stoul(argv[5], nullptr, 0);
    if (frames == 0 || frames > 3600 || mask > 255) {
      throw std::runtime_error("Invalid bounds");
    }
    for (int i = 6; i < argc; ++i) {
      g_watches.push_back(std::stoul(argv[i], nullptr, 16));
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
      throw std::runtime_error("Cannot open ROM");
    }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(input)), {});
    if (rom.size() < 0x8000 || rom.size() > 0x400000 || rom.size() % 0x8000 != 0) {
      throw std::runtime_error("Expected unheadered ROM, 32 KiB aligned, at most 4 MiB");
    }
    g_trace.open(argv[3]);
    if (!g_trace) {
      throw std::runtime_error("Cannot create trace");
    }
    g_trace << "frame,instruction,pc,a,x,y,s,d,ram10,ram11,ram12\n";
    Audio audio(argv[2]);
    SNESSystem system;
    system.SoundInit(&audio);
    if (!system.Load(rom.data(), static_cast<uint32>(rom.size()), nullptr, 0)) {
      throw std::runtime_error("ROM load rejected");
    }
    system.soundEnableFlag = static_cast<uint8>(mask);
    // The wrapper's cached mask starts at zero; explicitly apply zero as well.
    S9xSetSoundControl(static_cast<uint8>(mask));
    for (g_frame = 0; g_frame < frames; ++g_frame) {
      system.CPULoop();
    }
    audio.Finish();
    g_trace.flush();
    if (!g_trace) {
      throw std::runtime_error("Trace write failed");
    }
    std::cout << "frames=" << frames << " instructions=" << g_instructions
              << " pcm_bytes=" << audio.bytes << " watched_hits=" << g_rows
              << " final_pc=" << std::hex << Registers.PBPC << std::dec << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
