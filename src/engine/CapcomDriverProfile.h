#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// Source-ROM evidence only. Recognition does not imply that another game's
// instruments, envelope tables or driver-specific effects can be transplanted.
namespace CapcomDriverProfile {
using std::size_t;
enum class Family { Unknown, Later, EarlyList, Ambiguous };
struct Result {
  Family family = Family::Unknown;
  size_t dispatcher = 0;
};

inline bool Matches(const std::vector<uint8_t>& bytes, size_t offset, const std::vector<int>& pattern) {
  if (offset > bytes.size() || pattern.size() > bytes.size() - offset) return false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] >= 0 && bytes[offset + i] != pattern[i]) return false;
  }
  return true;
}

inline Result Inspect(const std::vector<uint8_t>& bytes) {
  const std::vector<int> early = {0x1c, 0xfd, 0xf6, -1, -1, 0x2d, 0xf6, -1, -1, 0x2d,
      0xad, 8, 0x90, 5, 0xcb, 0xc2, 0x3f, -1, -1, 0x6f};
  const std::vector<int> later = {0x1c, 0xfd, 0xf6, -1, -1, 0x2d, 0xf6, -1, -1, 0x2d,
      0xad, 8, 0x90, 0x10, 0xfb, -1, 0xf4, -1, 0xda, 0xa0, 0x8d, 0, 0xf7, 0xa0,
      0xbb, -1, 0xd0, 2, 0xbb, -1, 0x6f};
  Result result;
  auto word = [&](size_t at) { return bytes[at] | (bytes[at + 1] << 8); };
  for (size_t offset = 0; offset < bytes.size(); ++offset) {
    const bool old = Matches(bytes, offset, early);
    if (!old && !Matches(bytes, offset, later)) continue;
    const size_t table = offset + (old ? early.size() : later.size());
    if (table > bytes.size() || 64 > bytes.size() - table) continue;
    const int tableAddress = word(offset + 7);
    auto mapped = [&](int address, const std::vector<int>& pattern) {
      const int64_t location = static_cast<int64_t>(table) + address - tableAddress;
      return location >= 0 && Matches(bytes, static_cast<size_t>(location), pattern);
    };
    // Validate the operand reader and the actual dispatch targets. A byte
    // prefix appearing incidentally in music/sample data is insufficient.
    if (old) {
      if (!mapped(word(offset + 17), {0xf4, 0, 0xc4, 0xc0, 0xf4, 0x10, 0xc4, 0xc1,
                                     0x8d, 0, 0xf7, 0xc0, 0xbb, 0, 0xd0, 2, 0xbb, 0x10, 0x6f}) ||
          !mapped(word(table + 0x1e * 2), {0xd5, 0x40, 1, 0x6f}) ||
          !mapped(word(table + 0x1f * 2), {0x1c, 0x1c, 0xc4, 0xc3, 0xf5, 0, 2, 0x28})) continue;
    } else if (!mapped(word(table + 0x1e * 2), {0x6f}) ||
               !mapped(word(table + 0x1f * 2), {0x6f})) {
      continue;
    }
    if (result.family != Family::Unknown) return {Family::Ambiguous, 0};
    result = {old ? Family::EarlyList : Family::Later, offset};
  }
  return result;
}
}  // namespace CapcomDriverProfile
