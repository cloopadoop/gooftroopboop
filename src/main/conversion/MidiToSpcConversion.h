/*
 * VGMTrans (c) 2002-2026
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "CapcomSnes/CapcomSnesFormat.h"

namespace conversion {

enum class SPCId666Mode: uint8_t {
  None = 0,
  Text = 1,
  Binary = 2,
};

struct MidiToSpcOptions {
  CapcomSnesVersion version = CAPCOMSNES_V1_BGM_IN_LIST;
  uint16_t baseAddress = 0x0D20;
  uint8_t durationRate = 180;
  uint8_t defaultProgram = 1;
  bool stripAutomation = true;
  bool useDefaultProgramMap = true;
  std::vector<uint8_t> programMap;
  // Per-mapped-program pitch compensation (see CapcomSnesWriter::WriterConfig).
  std::vector<int8_t> programTransposeMap;
  std::vector<int8_t> programTuningMap;
  bool gapPackAllowProgramSwaps = true;
  bool enforceProgramLimit = true;
  uint8_t maxProgram = 21;
  std::filesystem::path templateSpcPath;
  std::filesystem::path sequenceOutputPath;
  SPCId666Mode id666Mode = SPCId666Mode::None;
};

struct MidiToSpcResult {
  size_t sequenceSize = 0;
  bool usedTemplateFile = false;
  std::filesystem::path effectiveTemplatePath;
  std::vector<std::string> warnings;
};

std::vector<uint8_t> BuildDefaultCapcomProgramMap();
std::vector<uint8_t> BuildBlankTemplateSPC();

bool ConvertMidiToSpc(const std::filesystem::path& inputMidiPath,
                      const std::filesystem::path& outputSpcPath,
                      const MidiToSpcOptions& options,
                      MidiToSpcResult* outResult,
                      std::string* outError);

}  // namespace conversion
