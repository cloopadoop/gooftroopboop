#include "CapcomDriverProfile.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using CapcomDriverProfile::Family;
using CapcomDriverProfile::Inspect;

std::vector<uint8_t> fixture(bool early) {
  std::vector<uint8_t> bytes(1024, 0);
  const std::vector<uint8_t> prefix = early ?
      std::vector<uint8_t>{0x1c,0xfd,0xf6,1,4,0x2d,0xf6,0,4,0x2d,0xad,8,0x90,5,0xcb,0xc2,0x3f,0x10,5,0x6f} :
      std::vector<uint8_t>{0x1c,0xfd,0xf6,1,4,0x2d,0xf6,0,4,0x2d,0xad,8,0x90,0x10,0xfb,0,0xf4,8,
                          0xda,0xa0,0x8d,0,0xf7,0xa0,0xbb,0,0xd0,2,0xbb,8,0x6f};
  std::copy(prefix.begin(), prefix.end(), bytes.begin() + 32);
  const size_t table = 32 + prefix.size();
  auto target = [&](int opcode, int address) {
    bytes[table + opcode * 2] = static_cast<uint8_t>(address);
    bytes[table + opcode * 2 + 1] = static_cast<uint8_t>(address >> 8);
  };
  if (early) {
    target(0x1e, 0x530);
    target(0x1f, 0x540);
    const std::vector<uint8_t> reader = {0xf4,0,0xc4,0xc0,0xf4,0x10,0xc4,0xc1,0x8d,0,0xf7,0xc0,
                                        0xbb,0,0xd0,2,0xbb,0x10,0x6f};
    const std::vector<uint8_t> state1 = {0xd5,0x40,1,0x6f};
    const std::vector<uint8_t> state2 = {0x1c,0x1c,0xc4,0xc3,0xf5,0,2,0x28};
    std::copy(reader.begin(), reader.end(), bytes.begin() + table + 0x110);
    std::copy(state1.begin(), state1.end(), bytes.begin() + table + 0x130);
    std::copy(state2.begin(), state2.end(), bytes.begin() + table + 0x140);
  } else {
    target(0x1e, 0x500);
    target(0x1f, 0x500);
    bytes[table + 0x100] = 0x6f;
  }
  return bytes;
}

int main(int argc, char** argv) {
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      std::ifstream stream(argv[i], std::ios::binary | std::ios::ate);
      if (!stream || stream.tellg() < 0 || stream.tellg() > 8 * 1024 * 1024) return 2;
      std::vector<uint8_t> bytes(static_cast<size_t>(stream.tellg()));
      stream.seekg(0);
      if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return 2;
      const auto result = Inspect(bytes);
      std::cout << static_cast<int>(result.family) << '\n';
    }
    return 0;
  }
  int checks = 0;
  auto check = [&](bool condition) {
    if (!condition) throw std::runtime_error("Driver profile regression failed");
    ++checks;
  };
  for (bool early : {false, true}) {
    auto source = fixture(early);
    check(Inspect(source).family == (early ? Family::EarlyList : Family::Later));
    for (size_t size = 0; size < 100; ++size) {
      check(Inspect(std::vector<uint8_t>(source.begin(), source.begin() + size)).family == Family::Unknown);
    }
    auto duplicate = source;
    duplicate.insert(duplicate.end(), source.begin(), source.end());
    check(Inspect(duplicate).family == Family::Ambiguous);
    auto broken = source;
    broken[32 + (early ? 20 : 31) + (early ? 0x110 : 0x100)] = 0;
    check(Inspect(broken).family == Family::Unknown);
  }
  std::cout << checks << " driver profile checks passed\n";
}
