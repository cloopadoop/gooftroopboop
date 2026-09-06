#include "CapcomTrackTraversal.h"

#include <algorithm>
#include <unordered_set>

#include "LogManager.h"
#include "RawFile.h"

namespace {
constexpr int kMaxParseEvents = 20000;

bool IsDotted(uint8_t noteAttributes) {
  return (noteAttributes & CAPCOM_SNES_MASK_NOTE_DOTTED) != 0;
}

bool IsTriplet(uint8_t noteAttributes) {
  return (noteAttributes & CAPCOM_SNES_MASK_NOTE_TRIPLET) != 0;
}

bool IsSlurred(uint8_t noteAttributes) {
  return (noteAttributes & CAPCOM_SNES_MASK_NOTE_SLURRED) != 0;
}

uint32_t LengthFromIndex(uint8_t lenIndex, bool dotted, bool triplet) {
  uint32_t len = 192u >> (7 - lenIndex);
  if (dotted) {
    if ((len % 2 == 0) && len < 0x80) {
      len = len + (len / 2);
    }
  } else if (triplet) {
    len = len * 2 / 3;
  }
  return len;
}

uint32_t DurationFromLength(uint32_t len, uint8_t durationRate, bool slurred) {
  uint32_t dur = len * durationRate;
  if (slurred) {
    dur = len << 8;
  } else if (dur == 0) {
    dur = len << 8;
  }
  dur = (dur + 0x80) >> 8;
  return std::max<uint32_t>(1, dur);
}

CapcomCmdType DecodeCmdType(uint8_t statusByte) {
  if (statusByte >= 0x20) {
    const uint8_t keyIndex = statusByte & 0x1F;
    return (keyIndex == 0) ? CapcomCmdType::Rest : CapcomCmdType::Note;
  }

  switch (statusByte) {
    case 0x00:
      return CapcomCmdType::ToggleTriplet;
    case 0x01:
      return CapcomCmdType::ToggleSlur;
    case 0x02:
      return CapcomCmdType::DottedNoteOn;
    case 0x03:
      return CapcomCmdType::ToggleOctaveUp;
    case 0x04:
      return CapcomCmdType::NoteAttributes;
    case 0x05:
      return CapcomCmdType::Tempo;
    case 0x06:
      return CapcomCmdType::Duration;
    case 0x07:
      return CapcomCmdType::Volume;
    case 0x08:
      return CapcomCmdType::ProgramChange;
    case 0x09:
      return CapcomCmdType::Octave;
    case 0x0A:
      return CapcomCmdType::GlobalTranspose;
    case 0x0B:
      return CapcomCmdType::Transpose;
    case 0x0C:
      return CapcomCmdType::Tuning;
    case 0x0D:
      return CapcomCmdType::PortamentoTime;
    case 0x0E:
    case 0x0F:
    case 0x10:
    case 0x11:
      return CapcomCmdType::RepeatUntil;
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
      return CapcomCmdType::RepeatBreak;
    case 0x16:
      return CapcomCmdType::Goto;
    case 0x17:
      return CapcomCmdType::End;
    case 0x18:
      return CapcomCmdType::Pan;
    case 0x19:
      return CapcomCmdType::MasterVolume;
    case 0x1A:
      return CapcomCmdType::LFO;
    case 0x1B:
      return CapcomCmdType::EchoParam;
    case 0x1C:
      return CapcomCmdType::EchoOnOff;
    case 0x1D:
      return CapcomCmdType::ReleaseRate;
    default:
      return CapcomCmdType::Unknown;
  }
}

bool DecodeCmd(RawFile *raw, uint32_t offset, CapcomCmdIR *out, std::string *error) {
  if (!raw || !out || !raw->isValidOffset(offset)) {
    return false;
  }

  // A malformed or edited stream can desync the parser (e.g. a byte in the
  // command range interpreted as a multi-byte opcode near the buffer end).
  // Reading an operand past the file bounds is undefined, so refuse rather
  // than fault. `size` is the total command length including the opcode.
  auto operandsInBounds = [raw, offset](uint32_t size) {
    return size == 0 || raw->isValidOffset(offset + size - 1);
  };

  out->params.clear();
  out->origAbsOffset = offset;
  out->statusByte = raw->readByte(offset);
  out->type = DecodeCmdType(out->statusByte);
  out->sizeBytes = 1;
  out->keyIndex = 0;
  out->lenIndex = 0;
  out->destWord = 0;
  out->repeatSlot = 0;
  out->repeatCount = 0;
  out->breakAttributes = 0;

  if (out->statusByte >= 0x20) {
    out->keyIndex = out->statusByte & 0x1F;
    out->lenIndex = out->statusByte >> 5;
    out->sizeBytes = 1;
    return true;
  }

  // Every case below reads at least the opcode plus operands; bail cleanly if
  // the declared command would run off the end of the buffer.
  auto truncated = [&](uint32_t size) {
    if (operandsInBounds(size)) {
      return false;
    }
    if (error) {
      *error = "Truncated command at end of data.";
    }
    out->sizeBytes = 1;
    return true;
  };

  switch (out->type) {
    case CapcomCmdType::ToggleTriplet:
    case CapcomCmdType::ToggleSlur:
    case CapcomCmdType::DottedNoteOn:
    case CapcomCmdType::ToggleOctaveUp:
    case CapcomCmdType::End:
      out->sizeBytes = 1;
      return true;
    case CapcomCmdType::NoteAttributes:
      if (truncated(2)) {
        return true;
      }
      out->params.push_back(raw->readByte(offset + 1));
      out->sizeBytes = 2;
      return true;
    case CapcomCmdType::Tempo:
      if (truncated(3)) {
        return true;
      }
      out->params.push_back(raw->readByte(offset + 1));
      out->params.push_back(raw->readByte(offset + 2));
      out->sizeBytes = 3;
      return true;
    case CapcomCmdType::Duration:
    case CapcomCmdType::Volume:
    case CapcomCmdType::Pan:
    case CapcomCmdType::MasterVolume:
    case CapcomCmdType::ProgramChange:
    case CapcomCmdType::Octave:
    case CapcomCmdType::GlobalTranspose:
    case CapcomCmdType::Transpose:
    case CapcomCmdType::Tuning:
    case CapcomCmdType::PortamentoTime:
    case CapcomCmdType::ReleaseRate:
      if (truncated(2)) {
        return true;
      }
      out->params.push_back(raw->readByte(offset + 1));
      out->sizeBytes = 2;
      if (out->type == CapcomCmdType::ProgramChange) {
        out->program = out->params[0];
      }
      return true;
    case CapcomCmdType::RepeatUntil:
      if (truncated(4)) {
        return true;
      }
      out->repeatSlot = out->statusByte - 0x0E;
      out->repeatCount = raw->readByte(offset + 1);
      out->destWord = raw->readShortBE(offset + 2);
      out->params.push_back(out->repeatCount);
      out->params.push_back(static_cast<uint8_t>(out->destWord >> 8));
      out->params.push_back(static_cast<uint8_t>(out->destWord & 0xFF));
      out->sizeBytes = 4;
      return true;
    case CapcomCmdType::RepeatBreak:
      if (truncated(4)) {
        return true;
      }
      out->repeatSlot = out->statusByte - 0x12;
      out->breakAttributes = raw->readByte(offset + 1);
      out->destWord = raw->readShortBE(offset + 2);
      out->params.push_back(out->breakAttributes);
      out->params.push_back(static_cast<uint8_t>(out->destWord >> 8));
      out->params.push_back(static_cast<uint8_t>(out->destWord & 0xFF));
      out->sizeBytes = 4;
      return true;
    case CapcomCmdType::Goto:
      if (truncated(3)) {
        return true;
      }
      out->destWord = raw->readShortBE(offset + 1);
      out->params.push_back(static_cast<uint8_t>(out->destWord >> 8));
      out->params.push_back(static_cast<uint8_t>(out->destWord & 0xFF));
      out->sizeBytes = 3;
      return true;
    case CapcomCmdType::LFO:
    case CapcomCmdType::EchoParam:
      if (truncated(3)) {
        return true;
      }
      out->params.push_back(raw->readByte(offset + 1));
      out->params.push_back(raw->readByte(offset + 2));
      out->sizeBytes = 3;
      return true;
    case CapcomCmdType::EchoOnOff:
      if (truncated(2)) {
        return true;
      }
      out->params.push_back(raw->readByte(offset + 1));
      out->sizeBytes = 2;
      return true;
    default:
      if (error) {
        *error = "Unknown command.";
      }
      // V1 engines (Goof Troop family) treat $1E/$1F as two-byte commands
      // (opcode + one operand). Decoding them as one byte desyncs the parse
      // against the driver. Stock GT songs never emit them, but imported or
      // converted data could.
      if ((out->statusByte == 0x1E || out->statusByte == 0x1F) && operandsInBounds(2)) {
        L_WARN("Rare command 0x{:02x} at 0x{:x} (V1 unknown, 1 operand) — please report this song",
               out->statusByte, offset);
        out->params.push_back(raw->readByte(offset + 1));
        out->sizeBytes = 2;
      } else {
        out->sizeBytes = 1;
      }
      return true;
  }
}
}  // namespace

bool CapcomTrackTraversal::Traverse(RawFile *raw,
                                    uint32_t trackStart,
                                    CapcomTrackTraversalResult *out,
                                    std::string *error) {
  if (!raw || !out || !raw->isValidOffset(trackStart)) {
    if (error) {
      *error = "Invalid track start.";
    }
    return false;
  }

  out->steps.clear();

  uint32_t curOffset = trackStart;
  uint32_t curTick = 0;
  // Capcom SNES tracks commonly begin without an explicit octave command,
  // so decode with engine-consistent startup octave 4.
  uint8_t noteAttributes = 0x04;
  uint8_t durationRate = 0;
  int8_t transpose = 0;
  int8_t globalTranspose = 0;
  uint8_t program = 0;
  uint32_t programChangeOffset = 0;
  uint8_t repeatCount[CAPCOM_SNES_REPEAT_SLOT_MAX] = {0, 0, 0, 0};
  uint32_t repeatStartOffset[CAPCOM_SNES_REPEAT_SLOT_MAX] = {0, 0, 0, 0};
  bool inRepeatLoop = false;
  uint32_t currentLoopSourceOffset = 0;
  std::unordered_set<uint32_t> visitedGotoTargets;

  int parseCount = 0;
  while (raw->isValidOffset(curOffset) && parseCount++ < kMaxParseEvents) {
    CapcomCmdIR cmd;
    if (!DecodeCmd(raw, curOffset, &cmd, error)) {
      return false;
    }

    cmd.tick = curTick;
    cmd.noteAttributes = noteAttributes;
    cmd.durationRate = durationRate;
    cmd.transpose = transpose;
    cmd.globalTranspose = globalTranspose;
    cmd.program = program;

    CapcomTrackTraversalStep step;
    step.cmd = cmd;
    step.programChangeOffset = programChangeOffset;
    step.isLoopRepeat = inRepeatLoop;
    step.loopSourceOffset = currentLoopSourceOffset;
    out->steps.push_back(step);

    const uint32_t nextOffset = curOffset + cmd.sizeBytes;

    switch (cmd.type) {
      case CapcomCmdType::Note:
      case CapcomCmdType::Rest: {
        const bool dotted = IsDotted(noteAttributes);
        const bool triplet = IsTriplet(noteAttributes);
        const bool slurred = IsSlurred(noteAttributes);

        const uint32_t len = LengthFromIndex(cmd.lenIndex, dotted, triplet);
        out->steps.back().deltaTicks = len;
        if (cmd.type == CapcomCmdType::Rest) {
          out->steps.back().durationTicks = len;
        } else {
          out->steps.back().durationTicks = DurationFromLength(len, durationRate, slurred);
        }

        curTick += len;

        if (dotted) {
          noteAttributes &= ~CAPCOM_SNES_MASK_NOTE_DOTTED;
        }
        curOffset = nextOffset;
        break;
      }
      case CapcomCmdType::ToggleTriplet:
        noteAttributes ^= CAPCOM_SNES_MASK_NOTE_TRIPLET;
        curOffset = nextOffset;
        break;
      case CapcomCmdType::ToggleSlur:
        noteAttributes ^= CAPCOM_SNES_MASK_NOTE_SLURRED;
        curOffset = nextOffset;
        break;
      case CapcomCmdType::DottedNoteOn:
        noteAttributes |= CAPCOM_SNES_MASK_NOTE_DOTTED;
        curOffset = nextOffset;
        break;
      case CapcomCmdType::ToggleOctaveUp:
        noteAttributes ^= CAPCOM_SNES_MASK_NOTE_OCTAVE_UP;
        curOffset = nextOffset;
        break;
      case CapcomCmdType::NoteAttributes: {
        const uint8_t attributes = cmd.params.empty() ? 0 : cmd.params[0];
        noteAttributes &= ~(CAPCOM_SNES_MASK_NOTE_OCTAVE_UP | CAPCOM_SNES_MASK_NOTE_TRIPLET |
                            CAPCOM_SNES_MASK_NOTE_SLURRED);
        noteAttributes |= attributes;
        curOffset = nextOffset;
        break;
      }
      case CapcomCmdType::Duration:
        durationRate = cmd.params.empty() ? 0 : cmd.params[0];
        curOffset = nextOffset;
        break;
      case CapcomCmdType::ProgramChange:
        programChangeOffset = cmd.origAbsOffset + 1;
        program = cmd.params.empty() ? 0 : cmd.params[0];
        curOffset = nextOffset;
        break;
      case CapcomCmdType::Octave: {
        const uint8_t newOctave = cmd.params.empty() ? 0 : cmd.params[0];
        noteAttributes = (noteAttributes & ~CAPCOM_SNES_MASK_NOTE_OCTAVE) |
                         (newOctave & CAPCOM_SNES_MASK_NOTE_OCTAVE);
        curOffset = nextOffset;
        break;
      }
      case CapcomCmdType::GlobalTranspose:
        globalTranspose = static_cast<int8_t>(cmd.params.empty() ? 0 : cmd.params[0]);
        curOffset = nextOffset;
        break;
      case CapcomCmdType::Transpose:
        transpose = static_cast<int8_t>(cmd.params.empty() ? 0 : cmd.params[0]);
        curOffset = nextOffset;
        break;
      case CapcomCmdType::RepeatUntil: {
        const uint8_t slot = cmd.repeatSlot;
        const uint8_t times = cmd.repeatCount;
        const uint32_t dest = cmd.destWord;

        if (times == 0 && repeatCount[slot] == 0) {
          return true;
        }

        if (repeatCount[slot] == 0) {
          repeatCount[slot] = times;
          repeatStartOffset[slot] = dest;
          curOffset = dest;
        } else {
          repeatCount[slot]--;
          if (repeatCount[slot] != 0) {
            inRepeatLoop = true;
            currentLoopSourceOffset = repeatStartOffset[slot];
            curOffset = dest;
          } else {
            inRepeatLoop = false;
            currentLoopSourceOffset = 0;
            curOffset = nextOffset;
          }
        }
        break;
      }
      case CapcomCmdType::RepeatBreak: {
        const uint8_t slot = cmd.repeatSlot;
        if (repeatCount[slot] == 1) {
          repeatCount[slot] = 0;
          inRepeatLoop = false;
          currentLoopSourceOffset = 0;

          noteAttributes &= ~(CAPCOM_SNES_MASK_NOTE_OCTAVE_UP | CAPCOM_SNES_MASK_NOTE_TRIPLET |
                              CAPCOM_SNES_MASK_NOTE_SLURRED);
          noteAttributes |= cmd.breakAttributes;
          curOffset = cmd.destWord;
        } else {
          curOffset = nextOffset;
        }
        break;
      }
      case CapcomCmdType::Goto: {
        const uint32_t dest = cmd.destWord;
        if (!visitedGotoTargets.insert(dest).second) {
          return true;
        }
        curOffset = dest;
        break;
      }
      case CapcomCmdType::End:
        return true;
      default:
        curOffset = nextOffset;
        break;
    }
  }

  if (parseCount >= kMaxParseEvents) {
    L_WARN("CapcomTrackTraversal hit parse guardrail at 0x{:x}", curOffset);
  }

  return true;
}
