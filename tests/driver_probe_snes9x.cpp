// Optional bounded handler probe using externally supplied Snes9x BAPU.
// Inputs are private synthetic SPC snapshots prepared from an owned source.
// This is a CPU/state experiment, not evidence of complete game playback.
#include "apu/bapu/snes/snes.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

SSettings Settings{};
namespace SNES { CPU cpu; }
void reference_dsp_write(int, unsigned, unsigned) {}
void S9xMSU1Generate(uint64) {}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "Usage: driver-probe input.spc sentinel-address output.json\n");
    return 2;
  }
  unsigned sentinel = 0;
  try {
    if (std::strspn(argv[2], "0123456789abcdefABCDEFxX") != std::strlen(argv[2])) return 2;
    size_t consumed = 0;
    const auto parsed = std::stoul(argv[2], &consumed, 0);
    if (consumed != std::strlen(argv[2]) || parsed < 0x0200 || parsed > 0xffbe) return 2;
    sentinel = static_cast<unsigned>(parsed);
  } catch (...) { return 2; }
  std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
  const auto size = input.tellg();
  if (!input || size < 0x10200 || size > 0x20000) return 2;
  std::vector<unsigned char> image(static_cast<size_t>(size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(image.data()), image.size())) return 2;
  if (std::memcmp(image.data(), "SNES-SPC700 Sound File Data", 25) ||
      image[0x100 + sentinel] != 0x2f || image[0x101 + sentinel] != 0xfe) {
    std::fprintf(stderr, "Invalid snapshot or missing bounded return sentinel\n");
    return 2;
  }
  // Do not overwrite an existing evidence file.
  if (std::ifstream(argv[3]).good()) return 2;
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
  for (unsigned address : {0xf1, 0xf2, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc})
    smp.mmio_write(address, smp.apuram[address]);
  SNES::dsp.spc_dsp.load(image.data() + 0x10100);
  Resampler samples(8192);
  SNES::dsp.spc_dsp.set_output(&samples);
  std::array<unsigned char, 65536> before;
  std::memcpy(before.data(), smp.apuram, before.size());
  std::array<unsigned char, 128> dspBefore;
  for (unsigned address = 0; address < dspBefore.size(); ++address)
    dspBefore[address] = SNES::dsp.spc_dsp.read(address);
  // Every path is bounded by emulated cycles, even a malformed handler.
  constexpr int cycleBudget = 4096;
  smp.clock -= cycleBudget;
  smp.enter();
  SNES::dsp.synchronize();
  const bool returned = smp.regs.pc == sentinel && smp.regs.sp == static_cast<uint8>(image[0x2b] + 2);
  std::ofstream output(argv[3], std::ios::binary);
  if (!output) return 2;
  output << "{\"cycle_budget\":" << cycleBudget
         << ",\"returned\":" << (returned ? "true" : "false")
         << ",\"pc\":" << unsigned(smp.regs.pc)
         << ",\"a\":" << unsigned(smp.regs.B.a)
         << ",\"x\":" << unsigned(smp.regs.x)
         << ",\"y\":" << unsigned(smp.regs.B.y)
         << ",\"p\":" << unsigned(smp.regs.p)
         << ",\"sp\":" << unsigned(smp.regs.sp) << ",\"changes\":[";
  bool first = true;
  for (unsigned address = 0; address < before.size(); ++address) {
    if (before[address] == smp.apuram[address]) continue;
    if (!first) output << ',';
    first = false;
    output << "{\"address\":" << address << ",\"before\":" << unsigned(before[address])
           << ",\"after\":" << unsigned(smp.apuram[address]) << '}';
  }
  output << "],\"dsp_changes\":[";
  first = true;
  for (unsigned address = 0; address < dspBefore.size(); ++address) {
    const unsigned value = SNES::dsp.spc_dsp.read(address);
    if (value == dspBefore[address]) continue;
    if (!first) output << ',';
    first = false;
    output << "{\"address\":" << address << ",\"before\":" << unsigned(dspBefore[address])
           << ",\"after\":" << value << '}';
  }
  output << "]}\n";
  if (!output) return 2;
  return returned ? 0 : 1;
}
