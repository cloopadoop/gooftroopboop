/*
 * VGMTrans (c) 2002-2026
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "MidiToSpcConversion.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <numeric>
#include "MidiReader.h"
#include "CapcomSnes/CapcomSnesWriter.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kSpcHeaderSize = 0x100;
constexpr size_t kSpcMinFileSize = 0x10180;
constexpr size_t kSpcFileSize = 0x10200;
constexpr size_t kSpcRamSize = 0x10000;
constexpr size_t kSpcDspRegSize = 0x80;
constexpr size_t kSpcExtraRamOffset = 0x101c0;

constexpr size_t kId666FlagOffset = 0x23;
constexpr size_t kId666TextStart = 0x2e;
constexpr size_t kId666TextEnd = 0xd2;

constexpr std::array<char, 25> kSpcSignature{
  'S', 'N', 'E', 'S', '-', 'S', 'P', 'C', '7', '0', '0', ' ',
  'S', 'o', 'u', 'n', 'd', ' ', 'F', 'i', 'l', 'e', ' ', 'D', 'a'
};

bool ReadFileBytes(const fs::path& path, std::vector<uint8_t>& outBytes, std::string* outError) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    if (outError != nullptr) {
      *outError = "Failed to open file: " + path.string();
    }
    return false;
  }

  const std::streamsize size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (size < 0) {
    if (outError != nullptr) {
      *outError = "Failed to read file size: " + path.string();
    }
    return false;
  }

  outBytes.assign(static_cast<size_t>(size), 0);
  if (!in.read(reinterpret_cast<char*>(outBytes.data()), size)) {
    if (outError != nullptr) {
      *outError = "Failed to read file bytes: " + path.string();
    }
    return false;
  }
  return true;
}

bool WriteFileBytes(const fs::path& path, const std::vector<uint8_t>& bytes, std::string* outError) {
  std::error_code ec;
  const fs::path parentPath = path.parent_path();
  if (!parentPath.empty() && !fs::exists(parentPath, ec)) {
    fs::create_directories(parentPath, ec);
    if (ec) {
      if (outError != nullptr) {
        *outError = "Failed to create directory: " + parentPath.string();
      }
      return false;
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (outError != nullptr) {
      *outError = "Failed to open output path: " + path.string();
    }
    return false;
  }

  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out.good()) {
    if (outError != nullptr) {
      *outError = "Failed to write output file: " + path.string();
    }
    return false;
  }

  return true;
}

bool ValidateSPCTemplate(const std::vector<uint8_t>& spcBytes,
                         const fs::path& templatePath,
                         std::string* outError) {
  if (spcBytes.size() < kSpcMinFileSize) {
    if (outError != nullptr) {
      *outError = "Template SPC is too small: " + templatePath.string();
    }
    return false;
  }

  if (!std::equal(kSpcSignature.begin(),
                  kSpcSignature.end(),
                  reinterpret_cast<const char*>(spcBytes.data()))) {
    if (outError != nullptr) {
      *outError = "Template SPC signature mismatch: " + templatePath.string();
    }
    return false;
  }

  if (spcBytes[0x21] != 0x1a || spcBytes[0x22] != 0x1a) {
    if (outError != nullptr) {
      *outError = "Template SPC missing 0x1A marker bytes at offsets 0x21/0x22";
    }
    return false;
  }

  return true;
}

void ApplyId666Mode(std::vector<uint8_t>& spcBytes, conversion::SPCId666Mode mode) {
  switch (mode) {
    case conversion::SPCId666Mode::None:
      spcBytes[kId666FlagOffset] = 0x00;
      for (size_t offset = kId666TextStart; offset <= kId666TextEnd; offset++) {
        spcBytes[offset] = 0x00;
      }
      break;
    case conversion::SPCId666Mode::Text:
      spcBytes[kId666FlagOffset] = 0x1a;
      break;
    case conversion::SPCId666Mode::Binary:
      spcBytes[kId666FlagOffset] = 0x1b;
      break;
  }
}

bool InjectSequence(std::vector<uint8_t>& spcBytes,
                    const std::vector<uint8_t>& sequenceBytes,
                    uint16_t baseAddress,
                    std::string* outError) {
  const size_t sequenceOffset = kSpcHeaderSize + baseAddress;
  if (sequenceOffset + sequenceBytes.size() > spcBytes.size()) {
    if (outError != nullptr) {
      *outError = "Sequence write range exceeds SPC file size";
    }
    return false;
  }

  std::copy(sequenceBytes.begin(),
            sequenceBytes.end(),
            spcBytes.begin() + static_cast<std::ptrdiff_t>(sequenceOffset));
  return true;
}

}  // namespace

namespace conversion {

std::vector<uint8_t> BuildDefaultCapcomProgramMap() {
  std::vector<uint8_t> map(128);
  std::iota(map.begin(), map.end(), 0);

  map[5] = 8;
  map[30] = 5;
  map[61] = 10;
  map[81] = 6;
  return map;
}

std::vector<uint8_t> BuildBlankTemplateSPC() {
  auto spc = std::vector<uint8_t>(kSpcFileSize, 0);

  static constexpr std::array<char, 33> kHeaderSignature{
    'S', 'N', 'E', 'S', '-', 'S', 'P', 'C', '7', '0', '0', ' ',
    'S', 'o', 'u', 'n', 'd', ' ', 'F', 'i', 'l', 'e', ' ', 'D',
    'a', 't', 'a', ' ', 'v', '0', '.', '3', '0'
  };
  std::copy(kHeaderSignature.begin(), kHeaderSignature.end(), spc.begin());

  spc[0x21] = 0x1a;
  spc[0x22] = 0x1a;
  spc[kId666FlagOffset] = 0x00;

  auto writeRam = [&spc](uint16_t addr, const std::initializer_list<uint8_t>& bytes) {
    const size_t offset = kSpcHeaderSize + addr;
    if (offset + bytes.size() > spc.size()) {
      return;
    }
    std::copy(bytes.begin(), bytes.end(), spc.begin() + static_cast<std::ptrdiff_t>(offset));
  };

  auto writeRamByte = [&spc](uint16_t addr, uint8_t value) {
    const size_t offset = kSpcHeaderSize + addr;
    if (offset < spc.size()) {
      spc[offset] = value;
    }
  };

  constexpr uint16_t bgmHeaderAddr = 0x4000;
  constexpr uint8_t bgmHeaderHi = static_cast<uint8_t>((bgmHeaderAddr >> 8) & 0xff);
  constexpr uint8_t bgmHeaderLo = static_cast<uint8_t>(bgmHeaderAddr & 0xff);

  writeRam(0x0100,
           {0x6f, 0x3f, 0xef, 0x06, 0x8f, bgmHeaderHi, 0xa1, 0x8f,
            bgmHeaderLo, 0xa0, 0x3f, 0x82, 0x05, 0x8d, 0x00, 0xdd});

  constexpr uint16_t dspRegListAddr = 0x2500;
  constexpr uint16_t dspValListAddr = 0x2510;
  constexpr uint16_t dspRegListMinus1 = static_cast<uint16_t>(dspRegListAddr - 1);
  constexpr uint16_t dspValListMinus1 = static_cast<uint16_t>(dspValListAddr - 1);
  writeRam(0x0200,
           {0x8d, 0x01, 0xf6,
            static_cast<uint8_t>(dspRegListMinus1 & 0xff),
            static_cast<uint8_t>((dspRegListMinus1 >> 8) & 0xff),
            0xc5, 0xf2, 0x00, 0xf6,
            static_cast<uint8_t>(dspValListMinus1 & 0xff),
            static_cast<uint8_t>((dspValListMinus1 >> 8) & 0xff),
            0xc5, 0xf3, 0x00, 0xfe, 0xf2});

  constexpr uint16_t instrTableAddr = 0x3000;
  writeRam(0x0300,
           {0x8d, 0x06, 0xcf, 0xda, 0xa0, 0x60, 0x98,
            static_cast<uint8_t>(instrTableAddr & 0xff), 0xa0, 0x98,
            static_cast<uint8_t>((instrTableAddr >> 8) & 0xff), 0xa1});

  writeRamByte(dspRegListAddr, 0x5d);
  writeRamByte(dspValListAddr, 0x20);

  constexpr uint16_t sampleDirAddr = 0x2000;
  constexpr uint16_t sampleStartAddr = 0x2200;
  writeRam(sampleDirAddr,
           {static_cast<uint8_t>(sampleStartAddr & 0xff),
            static_cast<uint8_t>((sampleStartAddr >> 8) & 0xff),
            static_cast<uint8_t>(sampleStartAddr & 0xff),
            static_cast<uint8_t>((sampleStartAddr >> 8) & 0xff)});
  writeRam(sampleStartAddr, {0x01, 0, 0, 0, 0, 0, 0, 0, 0});

  writeRam(instrTableAddr, {0x00, 0x8f, 0xe0, 0x00, 0x10, 0x00});

  // Blank song with all 8 channels present so the user can compose on any
  // track. Each track gets its own region with room to add notes; track 0
  // carries the global tempo. Layout: header at bgm+1, tracks spaced 0x80
  // apart from 0x4100 (0x80 = 128 bytes of headroom per channel).
  constexpr uint16_t seqHeaderAddr = static_cast<uint16_t>(bgmHeaderAddr + 1);
  constexpr uint16_t track0Addr = 0x4100;
  constexpr uint16_t trackStride = 0x80;
  {
    std::vector<uint8_t> hdr;
    for (int i = 0; i < 8; ++i) {
      const uint16_t addr = static_cast<uint16_t>(track0Addr + i * trackStride);
      hdr.push_back(static_cast<uint8_t>((addr >> 8) & 0xff));
      hdr.push_back(static_cast<uint8_t>(addr & 0xff));
    }
    const size_t off = kSpcHeaderSize + seqHeaderAddr;
    if (off + hdr.size() <= spc.size()) {
      std::copy(hdr.begin(), hdr.end(), spc.begin() + static_cast<std::ptrdiff_t>(off));
    }
  }
  // Each channel gets four empty bars (whole rests, 0xE0) so notes can be
  // inserted by splitting a rest. Track 0 also carries the global tempo.
  for (int i = 0; i < 8; ++i) {
    const uint16_t addr = static_cast<uint16_t>(track0Addr + i * trackStride);
    if (i == 0) {
      writeRam(addr, {0x05, 0x01, 0x00, 0x19, 0xff, 0x07, 0xa0, 0x08, 0x00, 0x09, 0x04,
                      0xe0, 0xe0, 0xe0, 0xe0, 0x17});
    } else {
      writeRam(addr, {0x07, 0xa0, 0x08, 0x00, 0x09, 0x04,
                      0xe0, 0xe0, 0xe0, 0xe0, 0x17});
    }
  }

  const size_t dspOffset = kSpcHeaderSize + kSpcRamSize;
  if (dspOffset + kSpcDspRegSize <= spc.size()) {
    std::fill_n(spc.begin() + static_cast<std::ptrdiff_t>(dspOffset), kSpcDspRegSize, 0);
  }
  if (kSpcExtraRamOffset + 0x40 <= spc.size()) {
    std::fill_n(spc.begin() + static_cast<std::ptrdiff_t>(kSpcExtraRamOffset), 0x40, 0);
  }
  return spc;
}

bool ConvertMidiToSpc(const fs::path& inputMidiPath,
                      const fs::path& outputSpcPath,
                      const MidiToSpcOptions& options,
                      MidiToSpcResult* outResult,
                      std::string* outError) {
  if (outResult != nullptr) {
    *outResult = MidiToSpcResult{};
  }
  if (outError != nullptr) {
    outError->clear();
  }

  MidiReader midi;
  if (!midi.loadFromFile(inputMidiPath.string())) {
    if (outError != nullptr) {
      *outError = midi.getErrorMessage();
    }
    return false;
  }

  CapcomSnesWriter::WriterConfig writerConfig;
  writerConfig.version = options.version;
  writerConfig.baseAddress = options.baseAddress;
  writerConfig.initialDurationRate = options.durationRate;
  writerConfig.defaultProgram = options.defaultProgram;
  writerConfig.stripAutomation = options.stripAutomation;
  writerConfig.gapPackAllowProgramSwaps = options.gapPackAllowProgramSwaps;
  writerConfig.enforceProgramLimit = options.enforceProgramLimit;
  writerConfig.maxProgram = options.maxProgram;

  if (!options.programMap.empty()) {
    writerConfig.programMap = options.programMap;
  } else if (options.useDefaultProgramMap) {
    writerConfig.programMap = BuildDefaultCapcomProgramMap();
  }

  writerConfig.programTransposeMap = options.programTransposeMap;
  writerConfig.programTuningMap = options.programTuningMap;

  CapcomSnesWriter writer;
  if (!writer.convertFromMidi(midi, writerConfig)) {
    if (outError != nullptr) {
      *outError = writer.getErrorMessage();
    }
    return false;
  }

  const std::vector<uint8_t>& sequenceBytes = writer.getSequenceData();
  if (sequenceBytes.size() < 17) {
    if (outError != nullptr) {
      *outError = "Converted sequence is unexpectedly small";
    }
    return false;
  }

  if (!options.sequenceOutputPath.empty()) {
    if (!WriteFileBytes(options.sequenceOutputPath, sequenceBytes, outError)) {
      return false;
    }
  }

  std::vector<uint8_t> spcBytes;
  fs::path effectiveTemplatePath;
  bool usedTemplateFile = false;
  if (options.templateSpcPath.empty()) {
    spcBytes = BuildBlankTemplateSPC();
  } else {
    if (!ReadFileBytes(options.templateSpcPath, spcBytes, outError)) {
      return false;
    }
    if (!ValidateSPCTemplate(spcBytes, options.templateSpcPath, outError)) {
      return false;
    }
    effectiveTemplatePath = options.templateSpcPath;
    usedTemplateFile = true;
  }

  if (!InjectSequence(spcBytes, sequenceBytes, options.baseAddress, outError)) {
    return false;
  }

  ApplyId666Mode(spcBytes, options.id666Mode);

  if (!WriteFileBytes(outputSpcPath, spcBytes, outError)) {
    return false;
  }

  if (outResult != nullptr) {
    outResult->sequenceSize = sequenceBytes.size();
    outResult->usedTemplateFile = usedTemplateFile;
    outResult->effectiveTemplatePath = effectiveTemplatePath;
    outResult->warnings = writer.getWarnings();
  }
  return true;
}

}  // namespace conversion
