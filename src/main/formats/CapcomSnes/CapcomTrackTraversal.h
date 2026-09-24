#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "CapcomSeqIR.h"
#include "formats/CapcomSnes/CapcomSnesSeq.h"

class RawFile;

struct CapcomTrackTraversalStep {
  CapcomCmdIR cmd;

  uint32_t programChangeOffset{0};
  uint32_t deltaTicks{0};
  uint32_t durationTicks{0};

  bool isLoopRepeat{false};
  uint32_t loopSourceOffset{0};
};

struct CapcomTrackTraversalResult {
  std::vector<CapcomTrackTraversalStep> steps;
};

class CapcomTrackTraversal {
public:
  static bool Traverse(RawFile *raw,
                       uint32_t trackStart,
                       CapcomTrackTraversalResult *out,
                       std::string *error = nullptr,
                       bool verifyControlFlow = false);
};
