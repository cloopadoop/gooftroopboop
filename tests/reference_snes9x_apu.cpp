// Optional reference renderer. Build against a separately supplied Snes9x BAPU
// source checkout; no emulator code, ROM, SPC, or recordings are bundled here.
// Uses Snes9x's SMP CPU, not the editor's Snes_Spc execution implementation.
// The DSPs share ancestry, so this is not independent DSP conformance evidence.
#include "apu/bapu/snes/snes.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

SSettings Settings{};
static std::ofstream trace;
static uint64_t cycleEnd = 0;
namespace SNES {
CPU cpu;
}
void reference_dsp_write(int clock, unsigned address, unsigned value) {
  trace << static_cast<int64_t>(cycleEnd) + clock << ',' << address << ',' << value << '\n';
}
void S9xMSU1Generate(uint64) {}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "Usage: reference-apu input.spc output.pcm seconds\n");
    return 2;
  }
  const int seconds = std::atoi(argv[3]);
  if (seconds < 1 || seconds > 120) {
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<unsigned char> image((std::istreambuf_iterator<char>(input)), {});
  if (image.size() < 0x10200 || std::memcmp(image.data(), "SNES-SPC700 Sound File Data", 25)) {
    std::fprintf(stderr, "Invalid SPC input\n");
    return 2;
  }
  Settings.InterpolationMethod = 2; // Hardware Gaussian, not enhanced interpolation.
  SNES::smp.power();
  SNES::dsp.power();
  SNES::cpu.reset();
  auto& smp = SNES::smp;
  std::memcpy(smp.apuram, image.data() + 0x100, 65536);
  smp.regs.pc = image[0x25] | (image[0x26] << 8);
  smp.regs.B.a = image[0x27];
  smp.regs.x = image[0x28];
  smp.regs.B.y = image[0x29];
  smp.regs.p = image[0x2a];
  smp.regs.sp = image[0x2b];
  for (unsigned address : {0xf1, 0xf2, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc}) {
    smp.mmio_write(address, smp.apuram[address]);
  }
  smp.timer0.stage3_ticks = smp.apuram[0xfd] & 15;
  smp.timer1.stage3_ticks = smp.apuram[0xfe] & 15;
  smp.timer2.stage3_ticks = smp.apuram[0xff] & 15;
  // SPC files do not store timer prescaler phase. Both players must start
  // from the same convention: the first prescaler tick is one clock away.
  smp.timer0.stage1_ticks = 127;
  smp.timer1.stage1_ticks = 127;
  smp.timer2.stage1_ticks = 15;
  SNES::dsp.spc_dsp.load(image.data() + 0x10100);
  // Match the documented player initialization, not its execution implementation.
  const unsigned echoStart = image[0x1016d] * 256;
  const unsigned echoLength = (image[0x1017d] & 15) * 2048;
  if (!(image[0x1016c] & 0x20)) {
    for (unsigned i = echoStart; i < std::min(65536u, echoStart + echoLength); ++i) {
      smp.apuram[i] = 0xff;
    }
  }
  for (unsigned port = 0; port < 4; ++port) {
    SNES::cpu.port_write(port, image[0x1f4 + port]);
  }
  SNES::cpu.port_write(1, 0);
  SNES::cpu.port_write(3, 0);
  SNES::cpu.port_write(0, 0xfb);
  Resampler samples(8192);
  SNES::dsp.spc_dsp.set_output(&samples);
  std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
  trace.open(std::string(argv[2]) + ".trace.csv");
  if (!output) {
    return 2;
  }
  int remaining = seconds * 32000 * 2;
  std::vector<int16_t> buffer(8192);
  while (remaining > 0) {
    smp.clock -= 1024 * 32;
    cycleEnd += 1024 * 32;
    smp.enter();
    SNES::dsp.synchronize();
    const int count = std::min(remaining, samples.space_filled());
    if (count <= 0 || !samples.pull(buffer.data(), count)) {
      std::fprintf(stderr, "Reference audio failed to progress\n");
      return 1;
    }
    output.write(reinterpret_cast<const char*>(buffer.data()), count * sizeof(int16_t));
    remaining -= count;
  }
  return output.good() ? 0 : 1;
}
