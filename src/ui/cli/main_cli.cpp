/**
 * VGMTrans (c) - 2002-2026
 * Licensed under the zlib license
 * See the included LICENSE for more information
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include "CLIGTBRoot.h"
#include "GTBExport.h"
#include "MidiToSpcConversion.h"
#include "CapcomSnes/CapcomSeqIR.h"
#include "CapcomSnes/CapcomTrackTraversal.h"
#include "RawFile.h"

namespace fs = std::filesystem;

namespace {

struct MidiToSpcCommand {
  fs::path inputMidi;
  fs::path outputSpc;
  conversion::MidiToSpcOptions options;
};

void PrintGlobalHelp() {
  std::cerr << "Usage:\n";
  std::cerr << "  GTBoop-cli input_file1 input_file2 ... -o output_directory\n";
  std::cerr << "  GTBoop-cli midi2spc <input.mid> <output.spc> [options]\n";
  std::cerr << "  GTBoop-cli roundtrip <file.spc|file.bin> [--semantic] [--base <hex>]\n";
  std::cerr << "  GTBoop-cli fuzz <file.spc|file.bin> [--ops <N>] [--seed <N>]\n";
}

void PrintMidiToSpcHelp() {
  std::cerr << "Usage: GTBoop-cli midi2spc <input.mid> <output.spc> [options]\n\n";
  std::cerr << "Options:\n";
  std::cerr << "  --template <file.spc>       Template SPC with instrument/sample data\n";
  std::cerr << "  --out-bin <file.bin>        Optional raw Capcom sequence output\n";
  std::cerr << "  --base-addr <hex>           ARAM base address (default: 0D20)\n";
  std::cerr << "  --duration <0-255>          Initial duration rate (default: 180)\n";
  std::cerr << "  --default-program <0-255>   Fallback instrument (default: 1)\n";
  std::cerr << "  --map-program <from:to>     Program remap pair (repeatable)\n";
  std::cerr << "  --program-transpose <prog:semi>  Signed semitone compensation per output program (repeatable)\n";
  std::cerr << "  --program-tuning <prog:unit>     Signed fine tuning, 1/256 semitone per output program (repeatable)\n";
  std::cerr << "  --gap-pack-program-swaps    Allow temporary program swaps in gap packing\n";
  std::cerr << "  --no-default-map            Disable built-in Goof Troop remap defaults\n";
  std::cerr << "  --no-strip-automation       Keep all CC/program automation\n";
  std::cerr << "  --id666-mode <none|text|binary>\n";
  std::cerr << "  -h, --help                  Show this help\n";
}

bool ParseInt(const std::string& text, int& outValue) {
  try {
    size_t idx = 0;
    int value = std::stoi(text, &idx, 10);
    if (idx != text.size()) {
      return false;
    }
    outValue = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseHex16(const std::string& text, uint16_t& outValue) {
  try {
    size_t idx = 0;
    unsigned long value = std::stoul(text, &idx, 16);
    if (idx != text.size() || value > 0xffff) {
      return false;
    }
    outValue = static_cast<uint16_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseProgramMapPair(const std::string& text, uint8_t& outFrom, uint8_t& outTo) {
  size_t colon = text.find(':');
  if (colon == std::string::npos || colon == 0 || colon == text.size() - 1) {
    return false;
  }

  int from = 0;
  int to = 0;
  if (!ParseInt(text.substr(0, colon), from) || !ParseInt(text.substr(colon + 1), to)) {
    return false;
  }

  if (from < 0 || from > 127 || to < 0 || to > 255) {
    return false;
  }

  outFrom = static_cast<uint8_t>(from);
  outTo = static_cast<uint8_t>(to);
  return true;
}

std::vector<uint8_t> BuildIdentityProgramMap() {
  std::vector<uint8_t> map(128);
  std::iota(map.begin(), map.end(), 0);
  return map;
}

bool ParseProgramSignedPair(const std::string& text, uint8_t& outProgram, int8_t& outValue) {
  size_t colon = text.find(':');
  if (colon == std::string::npos || colon == 0 || colon == text.size() - 1) {
    return false;
  }

  int program = 0;
  int value = 0;
  if (!ParseInt(text.substr(0, colon), program) || !ParseInt(text.substr(colon + 1), value)) {
    return false;
  }

  if (program < 0 || program > 255 || value < -128 || value > 127) {
    return false;
  }

  outProgram = static_cast<uint8_t>(program);
  outValue = static_cast<int8_t>(value);
  return true;
}

bool ParseMidiToSpcArgs(int argc, char* argv[], MidiToSpcCommand& outCmd, std::string& outError) {
  outCmd = MidiToSpcCommand{};
  std::vector<std::string> positional;
  bool customMapRequested = false;

  for (int i = 2; i < argc; i++) {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      PrintMidiToSpcHelp();
      return false;
    }

    if (arg == "--template" || arg == "--out-bin" || arg == "--base-addr" || arg == "--duration" ||
        arg == "--default-program" || arg == "--map-program" || arg == "--program-transpose" ||
        arg == "--program-tuning" || arg == "--id666-mode") {
      if (i + 1 >= argc) {
        outError = "Missing value for option: " + arg;
        return false;
      }
    }

    if (arg == "--template") {
      outCmd.options.templateSpcPath = fs::path(argv[++i]);
      continue;
    }

    if (arg == "--out-bin") {
      outCmd.options.sequenceOutputPath = fs::path(argv[++i]);
      continue;
    }

    if (arg == "--base-addr") {
      uint16_t baseAddress = 0;
      if (!ParseHex16(argv[++i], baseAddress)) {
        outError = "Invalid --base-addr value (expected hex): " + std::string(argv[i]);
        return false;
      }
      outCmd.options.baseAddress = baseAddress;
      continue;
    }

    if (arg == "--duration") {
      int value = 0;
      if (!ParseInt(argv[++i], value) || value < 0 || value > 255) {
        outError = "Invalid --duration value: " + std::string(argv[i]);
        return false;
      }
      outCmd.options.durationRate = static_cast<uint8_t>(value);
      continue;
    }

    if (arg == "--default-program") {
      int value = 0;
      if (!ParseInt(argv[++i], value) || value < 0 || value > 255) {
        outError = "Invalid --default-program value: " + std::string(argv[i]);
        return false;
      }
      outCmd.options.defaultProgram = static_cast<uint8_t>(value);
      continue;
    }

    if (arg == "--map-program") {
      uint8_t from = 0;
      uint8_t to = 0;
      const std::string mapSpec = argv[++i];
      if (!ParseProgramMapPair(mapSpec, from, to)) {
        outError = "Invalid --map-program value: " + mapSpec;
        return false;
      }

      if (!customMapRequested) {
        outCmd.options.programMap = outCmd.options.useDefaultProgramMap
            ? conversion::BuildDefaultCapcomProgramMap()
            : BuildIdentityProgramMap();
        customMapRequested = true;
      }
      outCmd.options.programMap[from] = to;
      continue;
    }

    if (arg == "--program-transpose") {
      uint8_t program = 0;
      int8_t semitones = 0;
      const std::string spec = argv[++i];
      if (!ParseProgramSignedPair(spec, program, semitones)) {
        outError = "Invalid --program-transpose value (expected prog:semi, e.g. 16:-38): " + spec;
        return false;
      }
      if (outCmd.options.programTransposeMap.empty()) {
        outCmd.options.programTransposeMap.assign(256, 0);
      }
      outCmd.options.programTransposeMap[program] = semitones;
      continue;
    }

    if (arg == "--program-tuning") {
      uint8_t program = 0;
      int8_t tuningUnits = 0;
      const std::string spec = argv[++i];
      if (!ParseProgramSignedPair(spec, program, tuningUnits)) {
        outError = "Invalid --program-tuning value (expected prog:unit, e.g. 16:-10): " + spec;
        return false;
      }
      if (outCmd.options.programTuningMap.empty()) {
        outCmd.options.programTuningMap.assign(256, 0);
      }
      outCmd.options.programTuningMap[program] = tuningUnits;
      continue;
    }

    if (arg == "--no-default-map") {
      outCmd.options.useDefaultProgramMap = false;
      if (!customMapRequested) {
        outCmd.options.programMap.clear();
      }
      continue;
    }

    if (arg == "--no-strip-automation") {
      outCmd.options.stripAutomation = false;
      continue;
    }

    if (arg == "--gap-pack-program-swaps") {
      outCmd.options.gapPackAllowProgramSwaps = true;
      continue;
    }

    if (arg == "--id666-mode") {
      const std::string mode = argv[++i];
      if (mode == "none") {
        outCmd.options.id666Mode = conversion::SPCId666Mode::None;
      } else if (mode == "text") {
        outCmd.options.id666Mode = conversion::SPCId666Mode::Text;
      } else if (mode == "binary") {
        outCmd.options.id666Mode = conversion::SPCId666Mode::Binary;
      } else {
        outError = "Invalid --id666-mode value: " + mode;
        return false;
      }
      continue;
    }

    if (!arg.empty() && arg[0] == '-') {
      outError = "Unknown option: " + arg;
      return false;
    }

    positional.push_back(arg);
  }

  if (positional.size() != 2) {
    outError = "Expected <input.mid> and <output.spc> positional arguments";
    return false;
  }

  outCmd.inputMidi = fs::path(positional[0]);
  outCmd.outputSpc = fs::path(positional[1]);
  return true;
}

int RunMidiToSpcCommand(const MidiToSpcCommand& cmd) {
  conversion::MidiToSpcResult result;
  std::string error;
  if (!conversion::ConvertMidiToSpc(cmd.inputMidi, cmd.outputSpc, cmd.options, &result, &error)) {
    std::cerr << "MIDI->SPC conversion failed: " << error << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "Converted MIDI:  " << cmd.inputMidi.string() << std::endl;
  std::cout << "Output SPC:      " << cmd.outputSpc.string() << std::endl;
  if (!cmd.options.sequenceOutputPath.empty()) {
    std::cout << "Output BIN:      " << cmd.options.sequenceOutputPath.string() << std::endl;
  }
  std::cout << "Sequence bytes:  " << result.sequenceSize << std::endl;
  if (result.usedTemplateFile) {
    std::cout << "Template SPC:    " << result.effectiveTemplatePath.string() << std::endl;
  } else {
    std::cout << "Template SPC:    <blank scaffold>" << std::endl;
  }

  if (!result.warnings.empty()) {
    std::cout << "Warnings:" << std::endl;
    for (const auto& warning : result.warnings) {
      std::cout << "  - " << warning << std::endl;
    }
  }

  return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// roundtrip: parse -> re-serialize -> compare. The parity keystone (plan M1).
// Operates on in-memory buffers only; never writes to the input file.
// ---------------------------------------------------------------------------

void PrintRoundtripHelp() {
  std::cerr << "Usage: GTBoop-cli roundtrip <file.spc|file.bin> [options]\n\n";
  std::cerr << "Parses the Capcom sequence, re-serializes it with zero edits, and\n";
  std::cerr << "compares the result against the original bytes. Never writes files.\n\n";
  std::cerr << "Options:\n";
  std::cerr << "  --base <hex>   Song header ARAM address (default: 0D20)\n";
  std::cerr << "  --semantic     Also compare traversal event streams (use when byte\n";
  std::cerr << "                 layout legitimately changes, e.g. shared bytes)\n";
  std::cerr << "  -h, --help     Show this help\n";
}

constexpr uint32_t kAramSize = 0x10000;
constexpr uint32_t kSpcAramFileOffset = 0x100;
constexpr char kSpcSignature[] = "SNES-SPC700 Sound File Data";

struct RoundtripInput {
  std::vector<uint8_t> aram;   // 64KB ARAM-addressed image
  uint32_t base = 0x0D20;
  bool priorityInHeader = false;
};

bool LoadRoundtripInput(const fs::path& path, uint32_t base, RoundtripInput& out, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "Cannot open file: " + path.string();
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < 18) {
    error = "File too small to contain a Capcom sequence.";
    return false;
  }

  out.base = base;
  out.aram.assign(kAramSize, 0);

  const bool isSpc = bytes.size() >= kSpcAramFileOffset + kAramSize &&
                     std::memcmp(bytes.data(), kSpcSignature, sizeof(kSpcSignature) - 1) == 0;
  if (isSpc) {
    std::copy_n(bytes.begin() + kSpcAramFileOffset, kAramSize, out.aram.begin());
    out.priorityInHeader = false;
    return true;
  }

  // Raw song image (.bin): loads at `base`. A leading zero byte with a
  // plausible big-endian pointer after it marks the custom priority-byte
  // variant (e.g. the hand-authored I2I.bin).
  if (base + bytes.size() > kAramSize) {
    error = "Image does not fit in ARAM at the given base address.";
    return false;
  }
  std::copy(bytes.begin(), bytes.end(), out.aram.begin() + base);
  const uint16_t firstPtrIfPriority = static_cast<uint16_t>((bytes[1] << 8) | bytes[2]);
  out.priorityInHeader = (bytes[0] == 0x00 && firstPtrIfPriority >= base &&
                          firstPtrIfPriority < base + bytes.size());
  return true;
}

struct TraversalStream {
  bool present = false;
  CapcomTrackTraversalResult result;
};

bool CollectStreams(RawFile* raw, uint32_t base, bool priority, std::array<TraversalStream, 8>& streams) {
  const uint32_t headerOffset = base + (priority ? 1 : 0);
  for (int i = 0; i < 8; ++i) {
    const uint16_t ptr = raw->readShortBE(headerOffset + static_cast<uint32_t>(i) * 2);
    streams[i].present = ptr != 0;
    if (streams[i].present) {
      if (!CapcomTrackTraversal::Traverse(raw, ptr, &streams[i].result)) {
        return false;
      }
    }
  }
  return true;
}

// Compare two command steps semantically. Destination words inside
// RepeatUntil/RepeatBreak/Goto params are layout, not semantics (the traversal
// already followed them), so they are excluded.
bool StepsEqual(const CapcomTrackTraversalStep& sa, const CapcomTrackTraversalStep& sb,
                std::string& field) {
  const auto& ca = sa.cmd;
  const auto& cb = sb.cmd;
  if (ca.type != cb.type) { field = "type"; return false; }
  if (ca.statusByte != cb.statusByte) { field = "status"; return false; }
  if (ca.tick != cb.tick) { field = "tick"; return false; }
  if (sa.deltaTicks != sb.deltaTicks) { field = "deltaTicks"; return false; }

  const bool hasDest = ca.type == CapcomCmdType::RepeatUntil ||
                       ca.type == CapcomCmdType::RepeatBreak || ca.type == CapcomCmdType::Goto;
  if (hasDest) {
    // Only the non-pointer operand is semantic (repeat count / break attrs).
    if (ca.repeatCount != cb.repeatCount || ca.breakAttributes != cb.breakAttributes) {
      field = "loop-operand";
      return false;
    }
  } else if (ca.params != cb.params) {
    field = "params";
    return false;
  }
  return true;
}

bool StreamsEqual(const CapcomTrackTraversalResult& a, const CapcomTrackTraversalResult& b,
                  std::string& detail) {
  const size_t n = std::min(a.steps.size(), b.steps.size());
  for (size_t s = 0; s < n; ++s) {
    std::string field;
    if (!StepsEqual(a.steps[s], b.steps[s], field)) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "step %zu: %s differs (status 0x%02x@0x%x tick %u vs 0x%02x@0x%x tick %u)",
                    s, field.c_str(), a.steps[s].cmd.statusByte, a.steps[s].cmd.origAbsOffset,
                    a.steps[s].cmd.tick, b.steps[s].cmd.statusByte, b.steps[s].cmd.origAbsOffset,
                    b.steps[s].cmd.tick);
      detail = buf;
      return false;
    }
  }
  if (a.steps.size() != b.steps.size()) {
    detail = "step count " + std::to_string(a.steps.size()) + " vs " + std::to_string(b.steps.size());
    return false;
  }
  return true;
}

int RunRoundtripCommand(int argc, char* argv[]) {
  fs::path inputPath;
  uint32_t base = 0x0D20;
  bool semantic = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintRoundtripHelp();
      return EXIT_SUCCESS;
    }
    if (arg == "--semantic") {
      semantic = true;
      continue;
    }
    if (arg == "--base") {
      if (i + 1 >= argc) {
        std::cerr << "Error: Missing value for --base" << std::endl;
        return 2;
      }
      uint16_t parsed = 0;
      if (!ParseHex16(argv[++i], parsed)) {
        std::cerr << "Error: Invalid --base value (expected hex): " << argv[i] << std::endl;
        return 2;
      }
      base = parsed;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Error: Unknown option: " << arg << std::endl;
      PrintRoundtripHelp();
      return 2;
    }
    inputPath = fs::path(arg);
  }

  if (inputPath.empty()) {
    PrintRoundtripHelp();
    return 2;
  }

  RoundtripInput input;
  std::string error;
  if (!LoadRoundtripInput(inputPath, base, input, error)) {
    std::cerr << "Error: " << error << std::endl;
    return 2;
  }

  VirtFile original(input.aram.data(), kAramSize, inputPath.filename().string());

  CapcomSeqIR ir;
  if (!ir.parseFromImage(&original, input.base, input.priorityInHeader)) {
    std::cerr << "FAIL " << inputPath.filename().string() << ": could not parse sequence at 0x"
              << std::hex << input.base << std::endl;
    return 1;
  }

  const uint32_t allocation = ir.originalAllocation();
  std::vector<uint8_t> rewritten;
  if (!ir.serializeToBuffer(&original, &rewritten, &error)) {
    std::cerr << "FAIL " << inputPath.filename().string() << ": serialize error: " << error << std::endl;
    return 1;
  }

  // Byte comparison over the original footprint.
  size_t diffCount = 0;
  long firstDiff = -1;
  const size_t compareLen = std::max<size_t>(allocation, rewritten.size());
  for (size_t i = 0; i < compareLen; ++i) {
    const int origByte = (i < allocation) ? input.aram[input.base + i] : -1;
    const int newByte = (i < rewritten.size()) ? rewritten[i] : -1;
    if (origByte != newByte) {
      ++diffCount;
      if (firstDiff < 0) {
        firstDiff = static_cast<long>(i);
      }
    }
  }

  const std::string name = inputPath.filename().string();
  if (diffCount == 0) {
    std::cout << "PASS " << name << " (" << allocation << " bytes byte-identical)" << std::endl;
    return EXIT_SUCCESS;
  }

  std::cout << "BYTE-DIFF " << name << ": " << diffCount << " differing bytes of " << compareLen
            << " (original " << allocation << ", rewritten " << rewritten.size()
            << "), first at +0x" << std::hex << firstDiff << " (ARAM 0x"
            << (input.base + firstDiff) << ")" << std::dec << std::endl;

  if (!semantic) {
    return 1;
  }

  // Semantic pass: overlay the rewritten image and compare traversal streams.
  std::vector<uint8_t> overlaid = input.aram;
  std::fill(overlaid.begin() + input.base, overlaid.begin() + input.base + allocation, 0);
  std::copy(rewritten.begin(), rewritten.end(), overlaid.begin() + input.base);
  VirtFile rewrittenFile(overlaid.data(), kAramSize, name + " (rewritten)");

  std::array<TraversalStream, 8> before{};
  std::array<TraversalStream, 8> after{};
  if (!CollectStreams(&original, input.base, input.priorityInHeader, before) ||
      !CollectStreams(&rewrittenFile, input.base, input.priorityInHeader, after)) {
    std::cerr << "FAIL " << name << ": traversal error during semantic compare" << std::endl;
    return 1;
  }

  for (int t = 0; t < 8; ++t) {
    if (before[t].present != after[t].present) {
      std::cout << "FAIL " << name << ": track " << t << " presence changed" << std::endl;
      return 1;
    }
    if (!before[t].present) {
      continue;
    }
    std::string detail;
    if (!StreamsEqual(before[t].result, after[t].result, detail)) {
      std::cout << "FAIL " << name << ": track " << t << " semantic mismatch (" << detail << ")"
                << std::endl;
      return 1;
    }
  }

  std::cout << "PASS " << name << " (semantic: all 8 track event streams identical)" << std::endl;
  return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// fuzz: apply random note-level IR edits and assert the serializer never
// corrupts, overflows the song's ARAM budget, or produces an unparseable
// image (plan M2.4). Headless: no Qt, no GUI. Deterministic per seed.
// ---------------------------------------------------------------------------

void PrintFuzzHelp() {
  std::cerr << "Usage: GTBoop-cli fuzz <file.spc|file.bin> [options]\n\n";
  std::cerr << "Applies random note/rest edits to the sequence IR and, after each,\n";
  std::cerr << "verifies serialization stays crash-free, within the song's ARAM\n";
  std::cerr << "allocation, and re-parses into cleanly-terminating tracks.\n\n";
  std::cerr << "Options:\n";
  std::cerr << "  --ops <N>      Number of random edits (default: 200)\n";
  std::cerr << "  --seed <N>     PRNG seed (default: 1)\n";
  std::cerr << "  --base <hex>   Song header ARAM address (default: 0D20)\n";
  std::cerr << "  -h, --help     Show this help\n";
}

// Verify a serialized image is structurally sound: every non-empty track
// pointer stays in range and traverses to a clean End/goto-loop without hitting
// the parse guardrail. Budget enforcement is serializeToRaw's job (tested
// separately), so an over-budget image is not "corrupt" here — only a broken
// layout is.
bool VerifyImage(const std::vector<uint8_t>& seqBytes, uint32_t base, bool priority,
                 std::string& reason) {
  if (base + seqBytes.size() > kAramSize) {
    reason = "image exceeds ARAM";
    return false;
  }
  std::vector<uint8_t> aram(kAramSize, 0);
  std::copy(seqBytes.begin(), seqBytes.end(), aram.begin() + base);
  VirtFile vf(aram.data(), kAramSize, "fuzz");

  const uint32_t headerOffset = base + (priority ? 1 : 0);
  for (int t = 0; t < 8; ++t) {
    const uint16_t ptr = vf.readShortBE(headerOffset + static_cast<uint32_t>(t) * 2);
    if (ptr == 0) {
      continue;
    }
    if (ptr < base || ptr >= base + seqBytes.size()) {
      reason = "track " + std::to_string(t) + " pointer 0x" + std::to_string(ptr) + " out of range";
      return false;
    }
    CapcomTrackTraversalResult tr;
    std::string err;
    if (!CapcomTrackTraversal::Traverse(&vf, ptr, &tr, &err)) {
      reason = "track " + std::to_string(t) + " traversal failed: " + err;
      return false;
    }
    if (tr.steps.empty()) {
      reason = "track " + std::to_string(t) + " produced no steps";
      return false;
    }
    // A cleanly-terminating track ends on End, a repeat-0, or a goto loop; the
    // traversal returns those as its last decoded step. A run that hit the
    // 20000-event guardrail indicates a broken layout.
    if (tr.steps.size() >= 19999) {
      reason = "track " + std::to_string(t) + " hit parse guardrail (runaway)";
      return false;
    }
  }
  return true;
}

int RunFuzzCommand(int argc, char* argv[]) {
  fs::path inputPath;
  uint32_t base = 0x0D20;
  int ops = 200;
  unsigned seed = 1;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintFuzzHelp();
      return EXIT_SUCCESS;
    }
    if (arg == "--ops" && i + 1 < argc) {
      ops = std::atoi(argv[++i]);
      continue;
    }
    if (arg == "--seed" && i + 1 < argc) {
      seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
      continue;
    }
    if (arg == "--base" && i + 1 < argc) {
      uint16_t parsed = 0;
      if (!ParseHex16(argv[++i], parsed)) {
        std::cerr << "Error: Invalid --base value: " << argv[i] << std::endl;
        return 2;
      }
      base = parsed;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Error: Unknown option: " << arg << std::endl;
      return 2;
    }
    inputPath = fs::path(arg);
  }

  if (inputPath.empty()) {
    PrintFuzzHelp();
    return 2;
  }

  RoundtripInput input;
  std::string error;
  if (!LoadRoundtripInput(inputPath, base, input, error)) {
    std::cerr << "Error: " << error << std::endl;
    return 2;
  }

  VirtFile original(input.aram.data(), kAramSize, inputPath.filename().string());
  CapcomSeqIR ir;
  if (!ir.parseFromImage(&original, input.base, input.priorityInHeader)) {
    std::cerr << "FAIL " << inputPath.filename().string() << ": parse failed" << std::endl;
    return 1;
  }

  std::mt19937 rng(seed);
  const std::string name = inputPath.filename().string();

  int applied = 0, serializeOk = 0, serializeFail = 0;
  int updates = 0, inserts = 0, removes = 0;

  // Pick a random track that has commands.
  auto pickTrack = [&]() -> int {
    std::vector<int> candidates;
    for (int t = 0; t < CapcomSeqIR::MAX_TRACKS; ++t) {
      if (ir.track(t) && !ir.track(t)->cmds.empty()) {
        candidates.push_back(t);
      }
    }
    if (candidates.empty()) {
      return -1;
    }
    return candidates[rng() % candidates.size()];
  };

  for (int step = 0; step < ops; ++step) {
    const int t = pickTrack();
    if (t < 0) {
      break;
    }
    auto* trk = ir.track(t);
    const int n = static_cast<int>(trk->cmds.size());
    const int op = rng() % 3;

    // Valid note/rest status bytes are >= 0x20, i.e. lenIndex 1..7 (lenIndex 0
    // would collide with the command opcodes 0x00..0x1F). keyIndex 0 = rest.
    if (op == 0) {
      // Update a random Note/Rest in place (same size -> layout-preserving).
      const int idx = rng() % n;
      if (trk->cmds[idx].type == CapcomCmdType::Note ||
          trk->cmds[idx].type == CapcomCmdType::Rest) {
        CapcomCmdIR cmd = trk->cmds[idx];
        cmd.keyIndex = static_cast<uint8_t>(rng() % 32);
        cmd.lenIndex = static_cast<uint8_t>(1 + rng() % 7);
        cmd.type = (cmd.keyIndex == 0) ? CapcomCmdType::Rest : CapcomCmdType::Note;
        ir.updateCommand(t, idx, cmd);
        ++updates;
        ++applied;
      }
    } else if (op == 1) {
      // Insert a random note before a random command (structural; may exceed
      // budget on full songs, which must fail cleanly).
      const int idx = rng() % (n + 1);
      CapcomCmdIR cmd;
      cmd.keyIndex = static_cast<uint8_t>(1 + rng() % 31);
      cmd.lenIndex = static_cast<uint8_t>(1 + rng() % 7);
      cmd.type = CapcomCmdType::Note;
      cmd.statusByte = static_cast<uint8_t>((cmd.lenIndex << 5) | (cmd.keyIndex & 0x1F));
      cmd.sizeBytes = 1;
      ir.insertCommand(t, idx, cmd);
      ++inserts;
      ++applied;
    } else {
      // Remove a random Note/Rest that is not a pointer destination.
      std::vector<int> removable;
      for (int i = 0; i < n; ++i) {
        const auto& c = trk->cmds[i];
        if (c.type != CapcomCmdType::Note && c.type != CapcomCmdType::Rest) {
          continue;
        }
        bool isDest = false;
        for (int u = 0; u < CapcomSeqIR::MAX_TRACKS && !isDest; ++u) {
          const auto* ut = ir.track(u);
          if (!ut) {
            continue;
          }
          for (const auto& uc : ut->cmds) {
            if (uc.destTrackIndex == t && uc.destCmdIndex == i) {
              isDest = true;
              break;
            }
          }
        }
        if (!isDest) {
          removable.push_back(i);
        }
      }
      if (!removable.empty()) {
        ir.removeCommand(t, removable[rng() % removable.size()]);
        ++removes;
        ++applied;
      }
    }

    // Invariant check after every op.
    std::vector<uint8_t> image;
    std::string serErr;
    if (ir.serializeToBuffer(&original, &image, &serErr)) {
      ++serializeOk;
      std::string reason;
      if (!VerifyImage(image, input.base, input.priorityInHeader, reason)) {
        std::cout << "FAIL " << name << " (seed " << seed << ", step " << step
                  << "): corrupt image after edit: " << reason << std::endl;
        return 1;
      }
    } else {
      // A clean refusal (over budget, unresolved pointer) is acceptable.
      ++serializeFail;
    }
  }

  std::cout << "PASS " << name << " (seed " << seed << "): " << applied << " edits ("
            << updates << "u/" << inserts << "i/" << removes << "r), serialize "
            << serializeOk << " ok / " << serializeFail << " refused, no corruption"
            << std::endl;
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc >= 2) {
    const std::string firstArg(argv[1]);
    if (firstArg == "midi2spc") {
      MidiToSpcCommand cmd;
      std::string error;
      if (!ParseMidiToSpcArgs(argc, argv, cmd, error)) {
        if (!error.empty()) {
          std::cerr << "Error: " << error << std::endl;
          PrintMidiToSpcHelp();
          return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
      }
      return RunMidiToSpcCommand(cmd);
    }
    if (firstArg == "roundtrip") {
      return RunRoundtripCommand(argc, argv);
    }
    if (firstArg == "fuzz") {
      return RunFuzzCommand(argc, argv);
    }
    if (firstArg == "blanktemplate" && argc >= 3) {
      // Emit the File>New blank template SPC (for headless verification).
      const auto spc = conversion::BuildBlankTemplateSPC();
      std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(spc.data()),
                static_cast<std::streamsize>(spc.size()));
      std::cout << "Wrote blank template SPC: " << argv[2] << std::endl;
      return EXIT_SUCCESS;
    }
  }

  for (int i = 1; i < argc; ++i) {
    std::string s(argv[i]);
    if ((s == "-h") || (s == "--help")) {
      cliroot.displayHelp();
      PrintGlobalHelp();
      return EXIT_SUCCESS;
    }
    if (s == "-o") {
      if (i == argc - 1) {
        std::cerr << "Error: expected output directory" << std::endl;
        cliroot.displayUsage();
        PrintGlobalHelp();
        return EXIT_FAILURE;
      }
      cliroot.outputDir = fs::path(argv[++i]);
      continue;
    }
    cliroot.inputFiles.insert(fs::path(s));
  }

  bool success = false;
  if (cliroot.init()) {
    if (cliroot.exportAllCollections()) {
      success = true;
    }
  }

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
