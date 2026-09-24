#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "RawFile.h"
#include "CapcomDriverProfile.h"
#include "formats/CapcomSnes/CapcomTrackTraversal.h"
#include "nlohmann/json.hpp"

// Data-only repair proposals. Callers must validate and explicitly review the
// returned copy before importing. Never changes the input ASM or source ROM.
namespace AsmSourceRepair {

inline uint32_t Word(const std::vector<uint8_t>& data, size_t offset) {
  return (data.at(offset) << 8) | data.at(offset + 1);
}

inline bool Propose(const std::string& text, const std::vector<uint8_t>& input, uint32_t base,
                    const std::map<uint32_t, unsigned>& lines, const std::vector<uint8_t>& rom,
                    std::vector<uint8_t>& output, nlohmann::json& report, std::string& error) {
  if (input.size() < 40 || input.size() > 0x32e0 || base != 0xd20 ||
      rom.size() < input.size() || rom.size() > 8 * 1024 * 1024) {
    error = "Source repair requires a bounded sequence and a source ROM of at most 8 MiB";
    return false;
  }
  const auto profile = CapcomDriverProfile::Inspect(rom);
  if (profile.family == CapcomDriverProfile::Family::EarlyList) {
    report = {{"sourceDriver", "early-list"}, {"sourceDriverVerified", true},
              {"automaticPortSupported", false}};
    error = "Verified early Capcom driver: commands 1E/1F change state, and pan/volume behavior differs. "
            "Automatic conversion to the target driver is not supported; no bytes were changed.";
    return false;
  }
  if (profile.family != CapcomDriverProfile::Family::Later) {
    error = "Source ROM has no unambiguous verified later Capcom driver and handler table; no repair was applied";
    return false;
  }
  std::map<unsigned, unsigned> aliases;
  std::string declarations;
  std::istringstream declarationStream(text);
  std::string declaration;
  while (std::getline(declarationStream, declaration)) {
    declarations += declaration.substr(0, declaration.find(';')) + "\n";
  }
  const std::regex alias(R"(!instrument_([0-9a-fA-F]{1,2})\s*=\s*#?\$([0-9a-fA-F]{1,2})\b)");
  for (std::sregex_iterator it(declarations.begin(), declarations.end(), alias), end; it != end; ++it) {
    const auto from = static_cast<unsigned>(std::stoul((*it)[1], nullptr, 16));
    const auto to = static_cast<unsigned>(std::stoul((*it)[2], nullptr, 16));
    if (to > 23 || (aliases.count(from) && aliases[from] != to)) {
      error = "Source repair requires consistent target instrument aliases in 0..23";
      return false;
    }
    aliases[from] = to;
  }
  std::vector<std::string> sourceLines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    sourceLines.push_back(line);
  }
  // Use only unique 24-byte anchors, with at least three separated matches.
  // A filename, title, guessed instrument, or plausible sound is not evidence.
  std::map<size_t, std::vector<size_t>> candidates;
  for (size_t offset = 16; offset + 24 <= input.size(); offset += 4) {
    const auto first = std::search(rom.begin(), rom.end(), input.begin() + offset, input.begin() + offset + 24);
    if (first == rom.end()) {
      continue;
    }
    const auto again = std::search(first + 1, rom.end(), input.begin() + offset, input.begin() + offset + 24);
    const auto position = static_cast<size_t>(first - rom.begin());
    if (again != rom.end() || position < offset || position - offset + input.size() > rom.size()) {
      continue;
    }
    auto& anchors = candidates[position - offset];
    if (anchors.empty() || offset >= anchors.back() + 24) {
      anchors.push_back(offset);
    }
  }
  std::vector<std::pair<size_t, size_t>> ranked;
  for (const auto& [start, anchors] : candidates) {
    if (anchors.size() >= 3) {
      ranked.emplace_back(anchors.size(), start);
    }
  }
  std::sort(ranked.rbegin(), ranked.rend());
  if (ranked.empty() || (ranked.size() > 1 && ranked[0].first == ranked[1].first)) {
    error = "No unambiguous source sequence match; no repair was applied";
    return false;
  }
  const size_t start = ranked[0].second;
  const size_t header = input[0] == 0 ? 1 : 0;
  const uint32_t firstPointer = Word(rom, start + header);
  if (firstPointer < header + 16) {
    error = "Source sequence header cannot establish its ARAM base";
    return false;
  }
  const uint32_t sourceBase = firstPointer - static_cast<uint32_t>(header + 16);
  if (sourceBase + input.size() > 65536) {
    error = "Source sequence exceeds ARAM";
    return false;
  }
  std::vector<uint8_t> source(rom.begin() + start, rom.begin() + start + input.size());
  size_t equal = 0;
  for (size_t i = 0; i < input.size(); ++i) {
    equal += input[i] == source[i];
  }
  output = input;
  report = {{"sourceDriver", "later"}, {"sourceDriverVerified", true},
            {"sourceOffset", start}, {"sourceBase", sourceBase}, {"exactAnchors", ranked[0].first},
            {"changes", nlohmann::json::array()}, {"requiresReview", true}};
  auto change = [&](size_t offset, uint8_t value, const char* rule, size_t sourceOffset) {
    if (output[offset] != value) {
      for (auto& previous : report["changes"]) {
        if (previous["offset"] == offset) {
          previous["after"] = value;
          previous["rule"] = previous["rule"].get<std::string>() + " + " + rule;
          output[offset] = value;
          return;
        }
      }
      report["changes"].push_back({{"offset", offset}, {"before", output[offset]}, {"after", value},
                                    {"rule", rule}, {"sourceOffset", sourceOffset}});
      output[offset] = value;
    }
  };
  if (equal * 2 >= input.size()) {
    std::vector<uint8_t> aram(65536);
    std::copy(source.begin(), source.end(), aram.begin() + sourceBase);
    VirtFile raw(aram.data(), static_cast<uint32_t>(aram.size()), "source-repair");
    std::map<size_t, uint8_t> commands;
    for (unsigned track = 0; track < 8; ++track) {
      const auto pointer = Word(source, header + track * 2);
      if (pointer < sourceBase + header + 16 || pointer >= sourceBase + source.size()) {
        error = "Matched source header has out-of-range channel pointers";
        return false;
      }
      CapcomTrackTraversalResult traversal;
      if (!CapcomTrackTraversal::Traverse(&raw, pointer, &traversal, &error, true)) {
        error = "Matched source does not have verified control flow";
        return false;
      }
      for (const auto& step : traversal.steps) {
        const auto address = step.cmd.origAbsOffset;
        if (address < sourceBase + header + 16 || address >= sourceBase + source.size()) {
          error = "Matched source leaves the aligned passage";
          return false;
        }
        commands[address - sourceBase] = source[address - sourceBase];
      }
    }
    for (const auto& [offset, number] : lines) {
      if (offset < header + 16 || commands.count(offset) || offset + 4 > source.size()) {
        continue;
      }
      const auto op = input[offset];
      const size_t pointer = offset + (op == 0x16 ? 1 : 2);
      if ((op < 0xe || op > 0x16) || Word(input, pointer) != ((Word(source, pointer) + base - sourceBase) & 65535)) {
        continue;
      }
      change(pointer, source[pointer], "non-address-relocated", start + pointer);
      change(pointer + 1, source[pointer + 1], "non-address-relocated", start + pointer + 1);
    }
    for (const auto& [offset, op] : commands) {
      if (op == 0x18 && output[offset] != op && lines.count(offset)) {
        const auto& authored = sourceLines.at(lines.at(offset) - 1);
        const auto comment = authored.find(';');
        if (comment != std::string::npos && authored.find("[18] Pan", comment) != std::string::npos) {
          change(offset, op, "source-and-comment-confirmed-pan", start + offset);
        }
      }
      if (op >= 0xe && op <= 0x16) {
        const size_t pointer = offset + (op == 0x16 ? 1 : 2);
        if (pointer + 2 > source.size()) {
          error = "Truncated source control flow";
          return false;
        }
        const auto target = (Word(source, pointer) + base - sourceBase) & 65535;
        change(pointer, static_cast<uint8_t>(target >> 8), "source-flow-target", start + pointer);
        change(pointer + 1, static_cast<uint8_t>(target), "source-flow-target", start + pointer + 1);
      }
      if (op == 8 && offset + 1 < source.size() && output[offset] == 8 && aliases.count(source[offset + 1])) {
        const auto from = source[offset + 1];
        // Preserve explicit valid author substitutions, not just source bytes.
        if (output[offset + 1] == from || output[offset + 1] > 23) {
          change(offset + 1, static_cast<uint8_t>(aliases[from]), "source-instrument-alias", start + offset + 1);
        }
      }
    }
  } else {
    // Expanded arrangements are not globally aligned. Require several nearby
    // unique anchors and the exact false-relocation equation for each repair.
    for (size_t offset = 18; offset + 48 < input.size(); ++offset) {
      if (input[offset] != 8 || input[offset + 1] <= 23 || input[offset - 2] != 0x18 ||
          input[offset - 1] < 0xe || input[offset - 1] > 0x15 || !lines.count(offset - 1)) {
        continue;
      }
      std::map<size_t, unsigned> matches;
      for (size_t anchor = offset + 4; anchor < offset + 48 && anchor + 24 <= input.size(); anchor += 4) {
        const auto hit = std::search(rom.begin(), rom.end(), input.begin() + anchor, input.begin() + anchor + 24);
        if (hit == rom.end() || std::search(hit + 1, rom.end(), input.begin() + anchor,
                                          input.begin() + anchor + 24) != rom.end()) {
          continue;
        }
        const auto location = static_cast<size_t>(hit - rom.begin());
        if (location >= anchor - offset + 2) {
          ++matches[location - (anchor - offset)];
        }
      }
      std::vector<size_t> valid;
      for (const auto& [location, count] : matches) {
        if (count >= 3 && location + 3 <= rom.size() &&
            std::equal(input.begin() + offset - 2, input.begin() + offset + 1, rom.begin() + location - 2) &&
            Word(input, offset + 1) == ((Word(rom, location + 1) + base - sourceBase) & 65535) &&
            aliases.count(rom[location + 1])) {
          valid.push_back(location);
        }
      }
      if (valid.size() == 1) {
        const auto location = valid.front();
        change(offset + 1, static_cast<uint8_t>(aliases[rom[location + 1]]),
               "local-source-relocation-and-alias", location + 1);
        change(offset + 2, rom[location + 2], "local-source-relocation-and-alias", location + 2);
      }
    }
  }
  if (report["changes"].empty()) {
    error = "No source-backed repairs established; original input remains unchanged";
    return false;
  }
  return true;
}
} // namespace AsmSourceRepair
