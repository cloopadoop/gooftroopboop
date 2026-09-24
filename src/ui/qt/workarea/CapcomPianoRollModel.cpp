#include "CapcomPianoRollModel.h"

#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <array>

#include "formats/CapcomSnes/CapcomTrackTraversal.h"
#include "GTBColl.h"
#include "components/instr/GTBInstrSet.h"
#include "SeqTrack.h"
#include "LogManager.h"

namespace {
bool SyncSeqTrackOffsetsFromHeader(CapcomSnesSeq* seq, RawFile* raw) {
  if (!seq || !raw) {
    return false;
  }

  const uint32_t headerOffset = seq->dwOffset + (seq->priorityInHeader ? 1u : 0u);
  std::vector<uint16_t> trackPtrs;
  trackPtrs.reserve(CapcomSeqIR::MAX_TRACKS);
  for (int i = CapcomSeqIR::MAX_TRACKS - 1; i >= 0; --i) {
    const uint16_t ptr = raw->readShortBE(headerOffset + static_cast<uint32_t>(i) * 2u);
    if (ptr != 0) {
      trackPtrs.push_back(ptr);
    }
  }

  if (trackPtrs.size() != seq->aTracks.size()) {
    L_WARN("Track pointer count mismatch: header={}, seq={}", trackPtrs.size(), seq->aTracks.size());
  }

  const size_t count = std::min(trackPtrs.size(), seq->aTracks.size());
  for (size_t i = 0; i < count; ++i) {
    auto* track = seq->aTracks[i];
    if (!track) {
      continue;
    }
    track->dwOffset = trackPtrs[i];
    track->dwStartOffset = trackPtrs[i];
  }

  return count > 0;
}

const std::map<uint8_t, std::string> &goofTroopInstrumentNames() {
  static const std::map<uint8_t, std::string> kNames = {
      {0x00, "Bass Drum"},
      {0x01, "Slap Drum"},
      {0x02, "Vibrato Organ"},
      {0x03, "Flute"},
      {0x04, "Flute 2"},
      {0x05, "Xylophone"},
      {0x06, "Robotic Piano"},
      {0x07, "Toy Piano"},
      {0x08, "Electric Piano"},
      {0x09, "Echo Piano"},
      {0x0A, "Echo Piano 2"},
      {0x0B, "Electric Piano 2"},
      {0x0C, "Muted Piano"},
      {0x0D, "Percussion Kit"},
      {0x0E, "Percussion Kit 2"},
      {0x0F, "Water Splash"},
      {0x10, "Synth Lead"},
      {0x11, "Synth Lead 2"},
      {0x12, "Bass"},
      {0x13, "Clarinet"},
      {0x14, "Bassoon"},
      {0x15, "Whistle"},
  };
  return kNames;
}

bool isDotted(uint8_t noteAttributes) {
  return (noteAttributes & CAPCOM_SNES_MASK_NOTE_DOTTED) != 0;
}

bool isTriplet(uint8_t noteAttributes) {
  return (noteAttributes & CAPCOM_SNES_MASK_NOTE_TRIPLET) != 0;
}

bool isSlurred(uint8_t noteAttributes) {
  return (noteAttributes & CAPCOM_SNES_MASK_NOTE_SLURRED) != 0;
}

bool isOctaveUp(uint8_t noteAttributes) {
  return (noteAttributes & CAPCOM_SNES_MASK_NOTE_OCTAVE_UP) != 0;
}

uint8_t octave(uint8_t noteAttributes) {
  return noteAttributes & CAPCOM_SNES_MASK_NOTE_OCTAVE;
}

struct SettingCommandSpec {
  CapcomCmdType type{CapcomCmdType::Unknown};
  uint8_t statusByte{0};
  std::vector<uint8_t> params;
};

bool BuildSettingCommand(CapcomSettingType type,
                         uint16_t value,
                         uint8_t value2,
                         SettingCommandSpec *out,
                         std::string *error) {
  if (!out) {
    return false;
  }
  out->params.clear();
  const uint8_t value8 = static_cast<uint8_t>(std::min<uint16_t>(value, static_cast<uint16_t>(0xFFu)));
  switch (type) {
    case CapcomSettingType::Tempo: {
      out->type = CapcomCmdType::Tempo;
      out->statusByte = 0x05;
      out->params.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
      out->params.push_back(static_cast<uint8_t>(value & 0xFF));
      break;
    }
    case CapcomSettingType::Duration:
      out->type = CapcomCmdType::Duration;
      out->statusByte = 0x06;
      out->params.push_back(value8);
      break;
    case CapcomSettingType::Octave:
      out->type = CapcomCmdType::Octave;
      out->statusByte = 0x09;
      out->params.push_back(static_cast<uint8_t>(std::min<uint16_t>(value, 7)));
      break;
    case CapcomSettingType::GlobalTranspose:
      out->type = CapcomCmdType::GlobalTranspose;
      out->statusByte = 0x0A;
      out->params.push_back(value8);
      break;
    case CapcomSettingType::Transpose:
      out->type = CapcomCmdType::Transpose;
      out->statusByte = 0x0B;
      out->params.push_back(value8);
      break;
    case CapcomSettingType::Volume:
      out->type = CapcomCmdType::Volume;
      out->statusByte = 0x07;
      out->params.push_back(value8);
      break;
    case CapcomSettingType::Pan:
      out->type = CapcomCmdType::Pan;
      out->statusByte = 0x18;
      out->params.push_back(value8);
      break;
    case CapcomSettingType::LFO:
      out->type = CapcomCmdType::LFO;
      out->statusByte = 0x1A;
      out->params.push_back(value8);
      out->params.push_back(value2);
      break;
    case CapcomSettingType::Echo:
      if (value2 > 0) {
        out->type = CapcomCmdType::EchoParam;
        out->statusByte = 0x1B;
        out->params.push_back(value8);
        out->params.push_back(value2);
      } else {
        out->type = CapcomCmdType::EchoOnOff;
        out->statusByte = 0x1C;
        out->params.push_back(value8);
      }
      break;
    case CapcomSettingType::ReleaseRate:
      out->type = CapcomCmdType::ReleaseRate;
      out->statusByte = 0x1D;
      out->params.push_back(value8);
      break;
    default:
      if (error) {
        *error = "Unsupported setting type.";
      }
      return false;
  }
  return true;
}

bool BuildSettingWriteBytes(const CapcomSettingEvent &setting,
                            uint16_t value,
                            uint8_t value2,
                            std::vector<uint8_t> *out,
                            std::string *error) {
  if (!out) {
    return false;
  }
  out->clear();
  out->push_back(setting.statusByte);
  const uint8_t value8 = static_cast<uint8_t>(std::min<uint16_t>(value, static_cast<uint16_t>(0xFFu)));
  switch (setting.statusByte) {
    case 0x05:
      out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
      out->push_back(static_cast<uint8_t>(value & 0xFF));
      break;
    case 0x1A:
    case 0x1B:
      out->push_back(value8);
      out->push_back(value2);
      break;
    default:
      out->push_back(value8);
      break;
  }
  if (setting.sizeBytes != out->size()) {
    if (error) {
      *error = "Setting size mismatch.";
    }
    return false;
  }
  return true;
}
}  // namespace

CapcomPianoRollModel::CapcomPianoRollModel(CapcomSnesSeq *seq, QObject *parent)
    : QObject(parent),
      m_seq(seq),
      m_raw(seq ? seq->rawFile() : nullptr),
      m_writableRaw((m_raw && m_raw->isWritable()) ? m_raw : nullptr) {
  // This editor is Goof Troop–only: hardcode the instrument set.
  m_programNames.clear();
  for (const auto &[prog, name] : goofTroopInstrumentNames()) {
    m_programNames.emplace_back(prog, name);
  }
}

std::vector<std::pair<RawFile *, uint32_t>> CapcomPianoRollModel::s_recentEdits{};

const std::vector<std::pair<RawFile *, uint32_t>> &CapcomPianoRollModel::recentEdits() {
  return s_recentEdits;
}

const CapcomTrackData *CapcomPianoRollModel::trackData(int trackIndex) const {
  if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size())) {
    return nullptr;
  }
  return &m_tracks[trackIndex];
}

const CapcomTrackIR *CapcomPianoRollModel::irTrack(int trackIndex, std::string *error) {
  const auto* data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return nullptr;
  }
  if (!m_raw) {
    if (error) {
      *error = "No raw file.";
    }
    return nullptr;
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return nullptr;
    }
  }

  const int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  if (irTrackIndex < 0) {
    if (error) {
      *error = "Cannot map track to IR.";
    }
    return nullptr;
  }
  return m_ir->track(irTrackIndex);
}

bool CapcomPianoRollModel::byteUsage(uint32_t *usedOut, uint32_t *budgetOut) {
  if (!m_seq || !m_raw) {
    return false;
  }
  CapcomSeqIR ir;
  if (!ir.parseFromSeq(m_seq, m_raw)) {
    return false;
  }
  std::vector<uint8_t> image;
  if (!ir.serializeToBuffer(m_raw, &image)) {
    return false;
  }
  if (usedOut) {
    *usedOut = static_cast<uint32_t>(image.size());
  }
  if (budgetOut) {
    *budgetOut = std::max(ir.originalAllocation(), m_allocationFloor);
  }
  return true;
}

bool CapcomPianoRollModel::reload() {
  if (!m_seq || !m_raw) {
    return false;
  }

  m_ir.reset();
  m_tracks.clear();
  m_tracks.reserve(m_seq->aTracks.size());

  for (size_t i = 0; i < m_seq->aTracks.size(); ++i) {
    auto *track = m_seq->aTracks[i];
    CapcomTrackData data;
    data.trackOffset = track->dwOffset;
    parseTrack(static_cast<int>(i), track->dwOffset, data);
    m_tracks.emplace_back(std::move(data));
  }

  return true;
}

uint32_t CapcomPianoRollModel::lengthFromIndex(uint8_t lenIndex, bool dotted, bool triplet) {
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

uint32_t CapcomPianoRollModel::durationFromLength(uint32_t len, uint8_t durationRate, bool slurred) {
  uint32_t dur = len * durationRate;
  if (slurred) {
    dur = len << 8;
  } else if (dur == 0) {
    dur = len << 8;
  }
  dur = (dur + 0x80) >> 8;
  return std::max<uint32_t>(1, dur);
}

std::vector<uint32_t> CapcomPianoRollModel::supportedNoteLengths(bool pendingDotted) {
  std::set<uint32_t> lengths;
  for (uint8_t index = 1; index <= 7; ++index) {
    if (!pendingDotted) {
      lengths.insert(lengthFromIndex(index, false, false));
      lengths.insert(lengthFromIndex(index, false, true));
    }
    lengths.insert(lengthFromIndex(index, true, false));
  }
  return {lengths.begin(), lengths.end()};
}

bool CapcomPianoRollModel::buildTimedEvent(uint32_t ticks, uint8_t key, bool dotted, bool triplet,
                                         std::vector<CapcomCmdIR> &commands) {
  // Preserve the incoming mode where possible. Dotted is one-shot and cannot
  // be cleared by NoteAttributes; triplet is persistent and must be restored.
  for (int mode = 0; mode < 3; ++mode) {
    const bool useDotted = dotted || mode == 2;
    const bool useTriplet = mode == 1 ? !triplet : triplet;
    for (uint8_t index = 1; index <= 7; ++index) {
      if (lengthFromIndex(index, useDotted, useTriplet) != ticks) {
        continue;
      }
      auto control = [&](CapcomCmdType type, uint8_t status) {
        CapcomCmdIR cmd;
        cmd.type = type;
        cmd.statusByte = status;
        cmd.sizeBytes = 1;
        commands.push_back(cmd);
      };
      if (useTriplet != triplet) {
        control(CapcomCmdType::ToggleTriplet, 0x00);
      }
      if (useDotted && !dotted) {
        control(CapcomCmdType::DottedNoteOn, 0x02);
      }
      CapcomCmdIR cmd;
      cmd.type = key == 0 ? CapcomCmdType::Rest : CapcomCmdType::Note;
      cmd.keyIndex = key;
      cmd.lenIndex = index;
      cmd.statusByte = static_cast<uint8_t>((index << 5) | key);
      cmd.sizeBytes = 1;
      commands.push_back(cmd);
      if (useTriplet != triplet) {
        control(CapcomCmdType::ToggleTriplet, 0x00);
      }
      return true;
    }
  }
  return false;
}

bool CapcomPianoRollModel::buildArticulatedNote(uint32_t ticks, uint8_t key, bool dotted, bool triplet,
                                              bool slurred, uint8_t durationRate, uint8_t program,
                                              const CapcomNoteEvent *sourceNote,
                                              std::vector<CapcomCmdIR> &commands) {
  if (!sourceNote || sourceNote->segments.size() <= 1) {
    return buildTimedEvent(ticks, key, dotted, triplet, commands);
  }
  bool currentSlur = slurred;
  uint8_t currentRate = durationRate, currentProgram = program;
  uint32_t total = 0;
  auto setSlur = [&](bool wanted) {
    if (currentSlur == wanted) return;
    CapcomCmdIR control;
    control.type = CapcomCmdType::ToggleSlur;
    control.statusByte = 0x01;
    control.sizeBytes = 1;
    commands.push_back(control);
    currentSlur = wanted;
  };
  auto setValue = [&](CapcomCmdType type, uint8_t status, uint8_t wanted, uint8_t &current) {
    if (current == wanted) return;
    CapcomCmdIR control;
    control.type = type;
    control.statusByte = status;
    control.sizeBytes = 2;
    control.params = {wanted};
    control.program = wanted;
    commands.push_back(control);
    current = wanted;
  };
  // Preserve constituent timing and articulation, not merely total duration.
  for (const auto &segment : sourceNote->segments) {
    setSlur(isSlurred(segment.noteAttributes));
    setValue(CapcomCmdType::Duration, 0x06, segment.durationRate, currentRate);
    setValue(CapcomCmdType::ProgramChange, 0x08, segment.program, currentProgram);
    const uint32_t length = lengthFromIndex(segment.lenIndex, isDotted(segment.noteAttributes),
                                            isTriplet(segment.noteAttributes));
    if (!buildTimedEvent(length, key, total == 0 && dotted, triplet, commands)) return false;
    total += length;
  }
  setSlur(slurred);
  setValue(CapcomCmdType::Duration, 0x06, durationRate, currentRate);
  setValue(CapcomCmdType::ProgramChange, 0x08, program, currentProgram);
  return total == ticks;
}

bool CapcomPianoRollModel::buildRests(uint32_t ticks, bool dotted, bool triplet,
                                    std::vector<CapcomCmdIR> &commands) {
  if (ticks == 0) {
    return !dotted;
  }
  // Bound work by the entire ARAM capacity. Tiny requested gaps must never
  // round up: all integers >= 2 are composable from straight/triplet rests.
  if (ticks > 192u * 0x10000u) {
    return false;
  }
  while (ticks > 384) {
    if (!buildTimedEvent(192, 0, dotted, triplet, commands)) {
      return false;
    }
    dotted = false;
    ticks -= 192;
  }
  const auto lengths = supportedNoteLengths();
  std::vector<int> previous(ticks + 1, -1);
  previous[0] = 0;
  for (uint32_t total = 1; total <= ticks; ++total) {
    for (auto length : lengths) {
      if (length <= total && previous[total - length] >= 0) {
        previous[total] = static_cast<int>(length);
      }
    }
  }
  // Only the first rest consumes an incoming dotted modifier.
  for (auto it = lengths.rbegin(); it != lengths.rend(); ++it) {
    const auto first = *it;
    if (first > ticks || previous[ticks - first] < 0) {
      continue;
    }
    if (!buildTimedEvent(first, 0, dotted, triplet, commands)) {
      continue;
    }
    ticks -= first;
    while (ticks > 0) {
      const auto length = static_cast<uint32_t>(previous[ticks]);
      if (!buildTimedEvent(length, 0, false, triplet, commands)) {
        return false;
      }
      ticks -= length;
    }
    return true;
  }
  return false;
}

bool CapcomPianoRollModel::replaceTimedEvent(int trackIndex, uint32_t offset,
                                           const std::vector<CapcomCmdIR> &commands, std::string *error,
                                           int replaceCount) {
  const auto *data = trackData(trackIndex);
  if (commands.empty() || !data || rejectBorrowed(trackIndex, offset, error) || !irTrack(trackIndex, error)) {
    return false;
  }
  const int ti = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  const int ci = m_ir->findCmdIndexByOffset(ti, offset);
  const auto *stream = m_ir->track(ti);
  if (!stream || ci < 0 || replaceCount < 1 ||
      static_cast<size_t>(ci) + static_cast<size_t>(replaceCount) > stream->cmds.size()) {
    if (error) *error = "Cannot locate the complete timing range.";
    m_ir.reset();
    return false;
  }
  if (replaceCount > 1) {
    std::unordered_set<uint32_t> interior;
    for (int i = 0; i < replaceCount; ++i) {
      const auto &event = stream->cmds[ci + i];
      if (event.type != CapcomCmdType::Rest || event.origAbsOffset != offset + i) {
        if (error) *error = "Rest range crosses a control or timing command.";
        m_ir.reset();
        return false;
      }
      if (i > 0) interior.insert(event.origAbsOffset);
    }
    // Interior entry points have independent timing semantics. Do not erase
    // their delays, or redirect a jump to a different tick by deleting them.
    for (int other = 0; other < CapcomSeqIR::MAX_TRACKS; ++other) {
      const auto *candidate = m_ir->track(other);
      if (!candidate) continue;
      bool protectedRange = interior.count(candidate->origTrackStartAbs) != 0;
      for (const auto &event : candidate->cmds) {
        if (other != ti && interior.count(event.origAbsOffset)) protectedRange = true;
        if ((event.type == CapcomCmdType::Goto || event.type == CapcomCmdType::RepeatUntil ||
             event.type == CapcomCmdType::RepeatBreak) && interior.count(event.destWord)) {
          protectedRange = true;
        }
      }
      if (protectedRange) {
        if (error) *error = "Rest range contains a shared or loop entry point; no edit was made.";
        m_ir.reset();
        return false;
      }
    }
    for (int i = replaceCount - 1; i > 0; --i) {
      if (!m_ir->removeCommand(ti, ci + i)) {
        if (error) *error = "Cannot replace the complete rest range.";
        m_ir.reset();
        return false;
      }
    }
  }
  // Replacing the original command with the FIRST prefix preserves jump
  // landings. Inserting a prefix before it would let loop entries skip it.
  if (ci < 0 || !m_ir->updateCommand(ti, ci, commands.front())) {
    if (error) {
      *error = "Cannot replace timing event.";
    }
    m_ir.reset();
    return false;
  }
  for (size_t i = 1; i < commands.size(); ++i) {
    if (!m_ir->insertCommand(ti, ci + static_cast<int>(i), commands[i])) {
      if (error) {
        *error = "Cannot insert timing commands.";
      }
      m_ir.reset();
      return false;
    }
  }
  const auto before = captureRawSnapshot();
  if (!m_ir->serializeToRaw(m_raw, error)) {
    m_ir.reset();
    return false;
  }
  SyncSeqTrackOffsetsFromHeader(m_seq, m_raw);
  m_redo.clear();
  pushRawDiffUndo(before);
  reload();
  return true;
}

uint8_t CapcomPianoRollModel::chooseLenIndex(uint32_t targetLen, bool dotted, bool triplet) {
  // lenIndex 0 would encode a note/rest status byte in the command range
  // (0x00-0x1F) and corrupt the stream, so the shortest representable note is
  // lenIndex 1 (a 64th). Only 1..7 are valid.
  uint8_t bestIdx = 1;
  uint32_t bestDiff = std::numeric_limits<uint32_t>::max();
  for (uint8_t idx = 1; idx < 8; ++idx) {
    const uint32_t len = lengthFromIndex(idx, dotted, triplet);
    const uint32_t diff = (len > targetLen) ? (len - targetLen) : (targetLen - len);
    if (diff < bestDiff) {
      bestIdx = idx;
      bestDiff = diff;
    }
  }
  return bestIdx;
}

int CapcomPianoRollModel::computeMidiKey(uint8_t keyIndex,
                                         uint8_t octaveVal,
                                         bool octaveUp,
                                         int8_t transpose,
                                         int8_t globalTranspose) {
  if (keyIndex == 0) {
    return -1;
  }
  int key = static_cast<int>(keyIndex) - 1;
  key += static_cast<int>(octaveVal) * 12;
  if (octaveUp) {
    key += 24;
  }
  key += transpose;
  key += globalTranspose;
  return key;
}

uint8_t CapcomPianoRollModel::clampKeyIndex(int candidate) {
  return static_cast<uint8_t>(std::clamp(candidate, 1, 31));
}

bool CapcomPianoRollModel::parseTrack(int trackIndex, uint32_t trackOffset, CapcomTrackData &outTrack) {
  if (!m_raw || !m_raw->isValidOffset(trackOffset)) {
    return false;
  }

  CapcomTrackTraversalResult traversal;
  if (!CapcomTrackTraversal::Traverse(m_raw, trackOffset, &traversal)) {
    return false;
  }

  std::unordered_map<uint32_t, uint32_t> firstTickByOffset;
  firstTickByOffset.reserve(traversal.steps.size());
  for (const auto &step : traversal.steps) {
    const auto &cmd = step.cmd;
    if (firstTickByOffset.find(cmd.origAbsOffset) == firstTickByOffset.end()) {
      firstTickByOffset[cmd.origAbsOffset] = cmd.tick;
    }
  }

  // The traversal follows the song-loop GOTO for one extra pass. Find that
  // GOTO: the first one jumping back to bytes already played. (A GOTO into
  // unplayed bytes is a channel borrowing another channel's melody.)
  {
    std::unordered_set<uint32_t> played;
    for (const auto &step : traversal.steps) {
      const auto &cmd = step.cmd;
      if (cmd.type == CapcomCmdType::Goto && played.count(cmd.destWord) > 0) {
        outTrack.hasSongLoop = true;
        outTrack.songLoopTick = cmd.tick;
        outTrack.songLoopGotoOffset = cmd.origAbsOffset;
        if (const auto it = firstTickByOffset.find(cmd.destWord); it != firstTickByOffset.end()) {
          outTrack.songLoopDestTick = it->second;
        }
        break;
      }
      played.insert(cmd.origAbsOffset);
    }
  }

  std::unordered_set<uint32_t> seenRepeatUntil;

  CapcomNoteEvent *lastNote = nullptr;
  int lastKey = -1;

  for (const auto &step : traversal.steps) {
    const auto &cmd = step.cmd;
    const bool replay = outTrack.hasSongLoop && cmd.tick >= outTrack.songLoopTick;
    switch (cmd.type) {
      case CapcomCmdType::Note:
      case CapcomCmdType::Rest: {
        const bool dotted = isDotted(cmd.noteAttributes);
        const bool triplet = isTriplet(cmd.noteAttributes);
        const bool slurred = isSlurred(cmd.noteAttributes);
        const bool isRest = cmd.type == CapcomCmdType::Rest;

        int midiKey = -1;
        if (!isRest) {
          midiKey = computeMidiKey(cmd.keyIndex, octave(cmd.noteAttributes), isOctaveUp(cmd.noteAttributes),
                                   cmd.transpose, cmd.globalTranspose);
        }

        if (!isRest && slurred && lastNote && midiKey == lastKey && lastNote->isSongLoopReplay == replay) {
          lastNote->durationTicks += step.durationTicks;
          lastNote->deltaTicks += step.deltaTicks;
          lastNote->segments.push_back(cmd);
          break;
        }

        CapcomNoteEvent ev;
        ev.segments.push_back(cmd);
        ev.trackIndex = trackIndex;
        ev.rawOffset = cmd.origAbsOffset;
        ev.programChangeOffset = step.programChangeOffset;
        ev.statusByte = cmd.statusByte;
        ev.keyIndex = cmd.keyIndex;
        ev.lenIndex = cmd.lenIndex;
        ev.program = cmd.program;
        ev.startTick = cmd.tick;
        ev.deltaTicks = step.deltaTicks;
        ev.durationTicks = step.durationTicks;
        ev.octave = octave(cmd.noteAttributes);
        ev.octaveUp = isOctaveUp(cmd.noteAttributes);
        ev.dotted = dotted;
        ev.triplet = triplet;
        ev.slurred = slurred;
        ev.durationRate = cmd.durationRate;
        ev.transpose = cmd.transpose;
        ev.globalTranspose = cmd.globalTranspose;
        ev.midiKey = midiKey;
        ev.isRest = isRest;
        ev.isLoopRepeat = step.isLoopRepeat || replay;
        ev.loopSourceOffset = step.loopSourceOffset;
        ev.isSongLoopReplay = replay;
        ev.instrumentName = instrumentNameForProgram(cmd.program);
        outTrack.notes.emplace_back(ev);

        if (isRest) {
          lastNote = nullptr;
          lastKey = -1;
        } else {
          lastNote = &outTrack.notes.back();
          lastKey = midiKey;
        }
        break;
      }
      case CapcomCmdType::Tempo: {
        if (cmd.params.size() < 2) {
          break;
        }
        const uint16_t tempoVal = static_cast<uint16_t>((static_cast<uint16_t>(cmd.params[0]) << 8) |
                                                        static_cast<uint16_t>(cmd.params[1]));
        CapcomSettingEvent ev;
        ev.trackIndex = trackIndex;
        ev.rawOffset = cmd.origAbsOffset;
        ev.tick = cmd.tick;
        ev.statusByte = cmd.statusByte;
        ev.sizeBytes = cmd.sizeBytes;
        ev.type = CapcomSettingType::Tempo;
        ev.value1 = cmd.params[0];
        ev.value2 = cmd.params[1];
        ev.description = "Tempo: " + std::to_string(tempoVal);
        outTrack.settings.push_back(ev);
        break;
      }
      case CapcomCmdType::Duration:
      case CapcomCmdType::Volume:
      case CapcomCmdType::Pan:
      case CapcomCmdType::MasterVolume:
      case CapcomCmdType::Octave:
      case CapcomCmdType::Transpose:
      case CapcomCmdType::GlobalTranspose:
      case CapcomCmdType::EchoOnOff:
      case CapcomCmdType::ReleaseRate: {
        if (cmd.params.empty()) {
          break;
        }

        CapcomSettingEvent ev;
        ev.trackIndex = trackIndex;
        ev.rawOffset = cmd.origAbsOffset;
        ev.tick = cmd.tick;
        ev.statusByte = cmd.statusByte;
        ev.sizeBytes = cmd.sizeBytes;
        ev.value1 = cmd.params[0];

        switch (cmd.type) {
          case CapcomCmdType::Octave:
            ev.type = CapcomSettingType::Octave;
            ev.description = "Octave: " + std::to_string(ev.value1);
            break;
          case CapcomCmdType::Transpose:
            ev.type = CapcomSettingType::Transpose;
            ev.description = "Transpose: " + std::to_string(static_cast<int8_t>(ev.value1));
            break;
          case CapcomCmdType::GlobalTranspose:
            ev.type = CapcomSettingType::GlobalTranspose;
            ev.description = "Global transpose: " + std::to_string(static_cast<int8_t>(ev.value1));
            break;
          case CapcomCmdType::Duration:
            ev.type = CapcomSettingType::Duration;
            ev.description = "Duration: " + std::to_string(ev.value1);
            break;
          case CapcomCmdType::Volume:
            ev.type = CapcomSettingType::Volume;
            ev.description = "Volume: " + std::to_string(ev.value1);
            break;
          case CapcomCmdType::Pan:
            ev.type = CapcomSettingType::Pan;
            ev.description = "Pan: " + std::to_string(ev.value1);
            break;
          case CapcomCmdType::MasterVolume:
            ev.type = CapcomSettingType::Volume;
            ev.description = "Master Vol: " + std::to_string(ev.value1);
            break;
          case CapcomCmdType::EchoOnOff:
            ev.type = CapcomSettingType::Echo;
            ev.description = std::string("Echo ") + (ev.value1 ? "ON" : "OFF");
            break;
          case CapcomCmdType::ReleaseRate:
            ev.type = CapcomSettingType::ReleaseRate;
            ev.description = "Release: " + std::to_string(ev.value1);
            break;
          default:
            break;
        }

        outTrack.settings.push_back(ev);
        break;
      }
      case CapcomCmdType::LFO:
      case CapcomCmdType::EchoParam: {
        if (cmd.params.size() < 2) {
          break;
        }

        CapcomSettingEvent ev;
        ev.trackIndex = trackIndex;
        ev.rawOffset = cmd.origAbsOffset;
        ev.tick = cmd.tick;
        ev.statusByte = cmd.statusByte;
        ev.sizeBytes = cmd.sizeBytes;

        if (cmd.type == CapcomCmdType::LFO) {
          ev.type = CapcomSettingType::LFO;
          ev.value1 = cmd.params[0];
          ev.value2 = cmd.params[1];
          ev.description = "LFO: " + std::to_string(ev.value1) + "/" + std::to_string(ev.value2);
        } else {
          ev.type = CapcomSettingType::Echo;
          ev.value1 = cmd.params[0];
          ev.value2 = cmd.params[1];
          ev.description = "Echo: " + std::to_string(ev.value1) + "/" + std::to_string(ev.value2);
        }

        outTrack.settings.push_back(ev);
        break;
      }
      case CapcomCmdType::RepeatUntil: {
        if (!seenRepeatUntil.insert(cmd.origAbsOffset).second) {
          break;
        }
        CapcomLoopEvent ev;
        ev.trackIndex = trackIndex;
        ev.rawOffset = cmd.origAbsOffset;
        ev.tick = cmd.tick;
        ev.slot = cmd.repeatSlot;
        ev.repeatCount = cmd.repeatCount;
        ev.destOffset = cmd.destWord;
        if (const auto it = firstTickByOffset.find(cmd.destWord); it != firstTickByOffset.end()) {
          ev.destTickValid = true;
          ev.destTick = it->second;
        }
        outTrack.loops.push_back(ev);
        break;
      }
      default:
        break;
    }
  }

  return true;
}

int CapcomPianoRollModel::homeTrackOfOffset(uint32_t offset) const {
  // Track data is laid out back to back, so an offset belongs to the track
  // with the greatest start at or before it.
  int home = -1;
  uint32_t bestStart = 0;
  for (size_t t = 0; t < m_tracks.size(); ++t) {
    const uint32_t start = m_tracks[t].trackOffset;
    if (start <= offset && (home < 0 || start > bestStart)) {
      home = static_cast<int>(t);
      bestStart = start;
    }
  }
  return home;
}

bool CapcomPianoRollModel::rejectBorrowed(int trackIndex, uint32_t offset, std::string *error) const {
  const int home = homeTrackOfOffset(offset);
  if (home < 0 || trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size() ||
      m_tracks[static_cast<size_t>(home)].trackOffset == m_tracks[static_cast<size_t>(trackIndex)].trackOffset) {
    return false;
  }
  if (error) {
    *error = "These notes are stored in Track " + std::to_string(home + 1) + " and this channel plays them "
             "from there, so they can only be edited on Track " + std::to_string(home + 1) + ".";
  }
  return true;
}

std::string CapcomPianoRollModel::instrumentNameForProgram(uint8_t program) const {
  if (const auto it = goofTroopInstrumentNames().find(program); it != goofTroopInstrumentNames().end()) {
    return it->second;
  }
  return "Program " + std::to_string(program);
}

bool CapcomPianoRollModel::applyEdit(int trackIndex,
                                     size_t noteIndex,
                                     int midiKey,
                                     uint32_t targetLenTicks,
                                     bool makeRest,
                                     std::string *error) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (noteIndex >= data->notes.size()) {
    if (error) {
      *error = "Invalid note selection.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  const auto &note = data->notes[noteIndex];
  const uint32_t desiredLen = targetLenTicks;
  const uint8_t newLenIndex = chooseLenIndex(desiredLen, note.dotted, note.triplet);
  const uint32_t newLen = lengthFromIndex(newLenIndex, note.dotted, note.triplet);

  int rawKey = midiKey - note.transpose - note.globalTranspose;
  const int octaveOffset = static_cast<int>(note.octave) * 12 + (note.octaveUp ? 24 : 0);
  int keyIndexCandidate = rawKey - octaveOffset + 1;

  uint8_t newKeyIndex = makeRest ? 0 : clampKeyIndex(keyIndexCandidate);
  if (note.deltaTicks != lengthFromIndex(note.lenIndex, note.dotted, note.triplet)) {
    if (desiredLen != note.deltaTicks) {
      if (error) *error = "Use non-ripple resize to change a tied span's length.";
      return false;
    }
    std::map<uint32_t, uint8_t> replacements;
    for (const auto& segment : note.segments) {
      const int key = midiKey - segment.transpose - segment.globalTranspose -
          static_cast<int>(octave(segment.noteAttributes)) * 12 -
          (isOctaveUp(segment.noteAttributes) ? 24 : 0) + 1;
      if (!makeRest && (key < 1 || key > 31)) {
        if (error) *error = "Pitch cannot be represented for every segment of the tied span.";
        return false;
      }
      const uint8_t value = static_cast<uint8_t>((segment.statusByte & 0xE0) | (makeRest ? 0 : key));
      const auto previous = replacements.find(segment.origAbsOffset);
      if (previous != replacements.end() && previous->second != value) {
        if (error) *error = "Shared tied segments require conflicting pitch values.";
        return false;
      }
      replacements[segment.origAbsOffset] = value;
    }
    EditTransaction transaction(*this);
    for (const auto& [offset, value] : replacements) {
      if (!m_raw->writeByte(offset, value)) {
        if (error) *error = "Failed to update tied span.";
        return false;
      }
    }
    reload();
    return transaction.commit(error);
  }
  if (desiredLen != newLen) {
    std::vector<CapcomCmdIR> commands;
    if (!buildTimedEvent(desiredLen, newKeyIndex, note.dotted, note.triplet, commands)) {
      if (error) {
        *error = "Requested length is not representable in this timing context; no edit was made.";
      }
      return false;
    }
    return replaceTimedEvent(trackIndex, note.rawOffset, commands, error);
  }
  const uint8_t newStatus = static_cast<uint8_t>((newLenIndex << 5) | (newKeyIndex & 0x1f));

  L_INFO("CapcomEdit track={}, note={}, offset=0x{:x}, old=0x{:02x}, new=0x{:02x}, midiKey={}, lenIdx={}, rest={}",
         trackIndex, noteIndex, note.rawOffset, note.statusByte, newStatus, midiKey, newLenIndex, makeRest);
  if (!m_raw->writeByte(note.rawOffset, newStatus)) {
    if (error) {
      *error = "Failed to write to raw file buffer.";
    }
    return false;
  }
  // Verify write
  const auto verify = m_raw->readByte(note.rawOffset);
  if (verify != newStatus) {
    L_WARN("CapcomEdit verification mismatch at 0x{:x}: wrote 0x{:02x}, read back 0x{:02x}",
           note.rawOffset, newStatus, verify);
  }

  m_redo.clear();
  pushUndoBatch({EditEntry{m_raw, note.rawOffset, note.statusByte, newStatus}});
  recordEdit(m_raw, note.rawOffset);
  reload();
  return true;
}

bool CapcomPianoRollModel::AppendNoteAtTick(int trackIndex,
                                            uint32_t tick,
                                            int midiKey,
                                            uint32_t lenTicks,
                                            std::string *error,
                                            const CapcomNoteEvent *sourceNote) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) *error = "Invalid track selection.";
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) *error = "Backing file is not writable.";
    return false;
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) *error = "Failed to parse sequence IR.";
      return false;
    }
  }
  // Map by start offset (model display order differs from header order).
  // The new-song template gives every track its own END byte, so offsets
  // are unique and this lookup is unambiguous.
  const int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  auto *trk = irTrackIndex >= 0 ? m_ir->track(irTrackIndex) : nullptr;
  if (!trk) {
    if (error) *error = "Cannot map track to IR.";
    return false;
  }

  // New material goes just before the track's terminal: its END (a finite
  // track grows) or its song-loop GOTO (the looped body grows, so the note
  // plays on every repeat). The looping GOTO comes from parseTrack's loop
  // detection, not "the last command": some tracks end `goto ; end` (a dead
  // END after the loop) and a GOTO into another channel's melody is not a loop.
  uint32_t terminalOffset = 0;
  if (data->hasSongLoop) {
    terminalOffset = data->songLoopGotoOffset;
    const int home = homeTrackOfOffset(terminalOffset);
    if (home >= 0 && m_tracks[static_cast<size_t>(home)].trackOffset != data->trackOffset) {
      if (error) {
        *error = "This channel loops inside notes stored in Track " + std::to_string(home + 1) +
                 ", so its loop can only be extended on Track " + std::to_string(home + 1) + ".";
      }
      return false;
    }
  } else if (!trk->cmds.empty() && trk->cmds.back().type == CapcomCmdType::End) {
    terminalOffset = trk->cmds.back().origAbsOffset;
  } else {
    if (error) {
      *error = "Cannot append after this track.";
    }
    return false;
  }
  const int insertIdx = m_ir->findCmdIndexByOffset(irTrackIndex, terminalOffset);
  CapcomTrackTraversalResult traversal;
  if (insertIdx < 0 || !CapcomTrackTraversal::Traverse(m_raw, data->trackOffset, &traversal, error)) {
    if (error && error->empty()) {
      *error = "Cannot find the end of this track.";
    }
    return false;
  }
  // Use the FIRST time the terminal is reached. A looping GOTO is walked again
  // after the loop-back with a larger tick; the first pass ends at the body end.
  const CapcomCmdIR *terminal = nullptr;
  for (const auto &step : traversal.steps) {
    if (step.cmd.origAbsOffset == terminalOffset) {
      terminal = &step.cmd;
      break;
    }
  }
  if (!terminal || tick > std::numeric_limits<uint32_t>::max() - lenTicks) {
    if (error) {
      *error = "Cannot resolve a finite append timing context.";
    }
    return false;
  }
  const bool extendsLoop = data->hasSongLoop;
  // The body end is where the terminal sits on the first pass.
  const uint32_t endTick = terminal->tick;
  if (tick < endTick) {
    if (error) *error = "Position overlaps existing events.";
    return false;
  }

  std::vector<CapcomCmdIR> newCmds;
  auto pushSetting = [&](CapcomCmdType t, uint8_t status, uint8_t operand) {
    CapcomCmdIR c;
    c.type = t;
    c.statusByte = status;
    c.params = {operand};
    c.sizeBytes = 2;
    c.origAbsOffset = 0xFFFFFFFF;
    newCmds.push_back(c);
  };

  const int clampedKey = std::clamp(midiKey, 0, 127);
  const int transpose = terminal->transpose + terminal->globalTranspose;
  const int rawKey = clampedKey - transpose;
  const bool octaveUp = rawKey >= 96;
  const int octaveBase = octaveUp ? 24 : 0;
  const uint8_t oct = static_cast<uint8_t>(std::clamp((rawKey - octaveBase) / 12, 0, 7));
  const int keyCandidate = rawKey - octaveBase - oct * 12 + 1;
  if (keyCandidate < 1 || keyCandidate > 31) {
    if (error) {
      *error = "Pitch cannot be represented with the current transpose.";
    }
    return false;
  }
  const uint8_t keyIndex = static_cast<uint8_t>(keyCandidate);

  // A fresh track needs its basic voice state before the first note - but only
  // the parts it doesn't already have. A track can carry setup commands
  // (volume/duration/program from the new-song template or a prior edit) with
  // no notes yet; blindly re-emitting a ProgramChange here used to clobber the
  // track's real instrument back to Electric Piano.
  if (data->notes.empty()) {
    bool hasVol = false, hasDur = false, hasProg = false;
    for (int i = 0; i < insertIdx && i < static_cast<int>(trk->cmds.size()); ++i) {
      switch (trk->cmds[static_cast<size_t>(i)].type) {
        case CapcomCmdType::Volume: hasVol = true; break;
        case CapcomCmdType::Duration: hasDur = true; break;
        case CapcomCmdType::ProgramChange: hasProg = true; break;
        default: break;
      }
    }
    if (!hasVol) pushSetting(CapcomCmdType::Volume, 0x07, 0xC0);
    if (!hasDur) pushSetting(CapcomCmdType::Duration, 0x06, 0xC0);
    if (!hasProg) pushSetting(CapcomCmdType::ProgramChange, 0x08, 0x08);
    pushSetting(CapcomCmdType::Octave, 0x09, oct);
  } else {
    // only emit an octave change when the track isn't already there
    if (octave(terminal->noteAttributes) != oct) {
      pushSetting(CapcomCmdType::Octave, 0x09, oct);
    }
  }
  const bool previousOctaveUp = isOctaveUp(terminal->noteAttributes);
  if (previousOctaveUp != octaveUp) {
    CapcomCmdIR toggle;
    toggle.type = CapcomCmdType::ToggleOctaveUp;
    toggle.statusByte = 0x03;
    toggle.sizeBytes = 1;
    toggle.origAbsOffset = 0xFFFFFFFF;
    newCmds.push_back(toggle);
  }

  const uint32_t gap = tick - endTick;
  const bool dotted = isDotted(terminal->noteAttributes);
  const bool triplet = isTriplet(terminal->noteAttributes);
  if (gap > 0 && !buildRests(gap, dotted, triplet, newCmds)) {
    if (error) {
      *error = "Gap is not representable in this timing context (minimum rest is 2 ticks).";
    }
    return false;
  }
  uint8_t durationRate = terminal->durationRate;
  uint8_t program = terminal->program;
  for (const auto &command : newCmds) {
    if (command.params.empty()) continue;
    if (command.type == CapcomCmdType::Duration) durationRate = command.params[0];
    if (command.type == CapcomCmdType::ProgramChange) program = command.params[0];
  }
  if (!buildArticulatedNote(lenTicks, keyIndex, gap == 0 && dotted, triplet,
                            isSlurred(terminal->noteAttributes), durationRate, program, sourceNote, newCmds)) {
    if (error) {
      *error = "Requested length is not representable in this timing context; no edit was made.";
    }
    return false;
  }
  // Inside a loop the GOTO carries the state around to the loop start, so put
  // back what the loop body expects there: without this every later pass would
  // play in the new note's octave.
  if (extendsLoop) {
    const uint8_t loopOctave = octave(terminal->noteAttributes);
    if (oct != loopOctave) {
      pushSetting(CapcomCmdType::Octave, 0x09, loopOctave);
    }
    if (previousOctaveUp != octaveUp) {
      CapcomCmdIR toggleBack;
      toggleBack.type = CapcomCmdType::ToggleOctaveUp;
      toggleBack.statusByte = 0x03;
      toggleBack.sizeBytes = 1;
      toggleBack.origAbsOffset = 0xFFFFFFFF;
      newCmds.push_back(toggleBack);
    }
  }

  // Insert `cmds` before command `at` of IR track `irTrack`, then point any
  // jump that targeted that command (a repeat's exit landing on the END/GOTO)
  // at the first inserted command, so the new material is played on that path
  // too instead of being skipped.
  auto insertBeforeTerminal = [&](int irTrack, int at, const std::vector<CapcomCmdIR> &cmds) {
    for (size_t i = 0; i < cmds.size(); ++i) {
      if (!m_ir->insertCommand(irTrack, at + static_cast<int>(i), cmds[i])) {
        return false;
      }
    }
    const int movedTerminal = at + static_cast<int>(cmds.size());
    for (int t = 0; t < 8; ++t) {
      auto *other = m_ir->track(t);
      if (!other) {
        continue;
      }
      for (auto &c : other->cmds) {
        if (c.destTrackIndex == irTrack && c.destCmdIndex == movedTerminal) {
          c.destCmdIndex = at;
        }
      }
    }
    return true;
  };

  if (!insertBeforeTerminal(irTrackIndex, insertIdx, newCmds)) {
    if (error) *error = "Failed to insert note commands.";
    m_ir.reset();  // IR may hold a partial insert; drop it and re-parse next edit
    return false;
  }

  // Extending one channel's loop body alone would make its cycle longer than
  // the others', so the channels drift apart on every repeat. Pad every other
  // channel that loops at the same point with rests by the same amount.
  if (extendsLoop) {
    const uint32_t pad = tick + lenTicks - endTick;
    for (size_t t2 = 0; t2 < m_tracks.size(); ++t2) {
      const auto &d2 = m_tracks[t2];
      if (static_cast<int>(t2) == trackIndex || !d2.hasSongLoop || d2.songLoopTick != endTick) {
        continue;
      }
      const int ir2 = m_ir->findTrackIndexByStartOffset(d2.trackOffset);
      const int home2 = homeTrackOfOffset(d2.songLoopGotoOffset);
      if (ir2 < 0 || ir2 == irTrackIndex || home2 < 0 ||
          m_tracks[static_cast<size_t>(home2)].trackOffset != d2.trackOffset) {
        continue;  // same stream, or a loop living in another channel's bytes (padded there)
      }
      const int idx2 = m_ir->findCmdIndexByOffset(ir2, d2.songLoopGotoOffset);
      CapcomTrackTraversalResult tr2;
      const CapcomCmdIR *term2 = nullptr;
      if (idx2 >= 0 && CapcomTrackTraversal::Traverse(m_raw, d2.trackOffset, &tr2)) {
        for (const auto &step : tr2.steps) {
          if (step.cmd.origAbsOffset == d2.songLoopGotoOffset) {
            term2 = &step.cmd;
            break;
          }
        }
      }
      std::vector<CapcomCmdIR> rests;
      if (!term2 || !buildRests(pad, isDotted(term2->noteAttributes), isTriplet(term2->noteAttributes), rests) ||
          !insertBeforeTerminal(ir2, idx2, rests)) {
        if (error) {
          *error = "Could not extend Track " + std::to_string(t2 + 1) +
                   "'s loop by the same amount, so the channels would drift apart; no edit was made.";
        }
        m_ir.reset();
        return false;
      }
    }
  }

  const auto undoSnap = captureRawSnapshot();
  if (!m_ir->serializeToRaw(m_raw, error)) {
    m_ir.reset();
    return false;
  }
  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("AppendNoteAtTick: failed to sync track offsets from header");
  }
  m_redo.clear();
  pushRawDiffUndo(undoSnap);
  reload();
  return true;
}

bool CapcomPianoRollModel::InsertNoteAtTick(int track_index,
                                            uint32_t tick,
                                            int midi_key,
                                            uint32_t target_len_ticks,
                                            uint32_t *out_tick,
                                            uint32_t *out_duration_diff,
                                            std::string *error,
                                            const CapcomNoteEvent *sourceNote) {
  if (out_tick) {
    *out_tick = 0;
  }
  if (out_duration_diff) {
    *out_duration_diff = 0;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  const auto *data = trackData(track_index);
  if (!data) {
    if (error) {
      *error = "Invalid track selection.";
    }
    return false;
  }

  int note_index = -1;
  for (size_t i = 0; i < data->notes.size(); ++i) {
    const auto &note = data->notes[i];
    // A replay after the song loop shares bytes with a first-pass event;
    // editing it would put the note at the first-pass tick. Past the loop point
    // is "no event here", which routes callers to AppendNoteAtTick.
    if (note.isSongLoopReplay) {
      continue;
    }
    const uint32_t end_tick = note.startTick + note.deltaTicks;
    if (tick >= note.startTick && tick < end_tick) {
      note_index = static_cast<int>(i);
      break;
    }
  }

  if (note_index < 0) {
    if (error) {
      *error = "No event at this position.";
    }
    return false;
  }

  const auto &note = data->notes[static_cast<size_t>(note_index)];
  if (rejectBorrowed(track_index, note.rawOffset, error)) {
    return false;
  }
  const uint32_t base_len = lengthFromIndex(note.lenIndex, note.dotted, note.triplet);
  if (note.deltaTicks != base_len) {
    if (error) {
      *error = "Cannot insert inside tied notes.";
    }
    return false;
  }

  if (note.isRest) {
    const uint32_t leading = tick - note.startTick;
    uint32_t available = base_len;
    int replaceCount = 1;
    // Consume only as much consecutive silent space as needed. Source-byte
    // adjacency excludes intervening program, articulation and loop commands.
    while (target_len_ticks > available - leading &&
           static_cast<size_t>(note_index + replaceCount) < data->notes.size()) {
      const auto &next = data->notes[static_cast<size_t>(note_index + replaceCount)];
      if (!next.isRest || next.isSongLoopReplay || next.rawOffset != note.rawOffset + replaceCount ||
          next.startTick != note.startTick + available ||
          next.deltaTicks != lengthFromIndex(next.lenIndex, next.dotted, next.triplet)) break;
      available += next.deltaTicks;
      ++replaceCount;
    }
    // A finite track may grow at its end. There are no downstream events to
    // displace when this rest range is followed immediately by END.
    const uint32_t afterRange = note.rawOffset + replaceCount;
    if (target_len_ticks > available - leading &&
        static_cast<size_t>(note_index + replaceCount) == data->notes.size() &&
        m_raw->isValidOffset(afterRange) && m_raw->readByte(afterRange) == 0x17 &&
        target_len_ticks <= std::numeric_limits<uint32_t>::max() - leading) {
      available = leading + target_len_ticks;
    }
    if (target_len_ticks == 0 || target_len_ticks > available - leading) {
      if (error) {
        *error = "Requested length exceeds the available rest; no edit was made.";
      }
      return false;
    }
    const int rawKey = midi_key - note.transpose - note.globalTranspose;
    const int key = rawKey - static_cast<int>(note.octave) * 12 - (note.octaveUp ? 24 : 0) + 1;
    if (key < 1 || key > 31) {
      if (error) {
        *error = "Pitch cannot be represented with the current octave/transpose.";
      }
      return false;
    }
    std::vector<CapcomCmdIR> commands;
    const uint32_t trailing = available - leading - target_len_ticks;
    bool generated = leading == 0 || buildRests(leading, note.dotted, note.triplet, commands);
    if (generated) {
      generated = buildArticulatedNote(target_len_ticks, static_cast<uint8_t>(key),
                                      leading == 0 && note.dotted, note.triplet, note.slurred,
                                      note.durationRate, note.program, sourceNote, commands);
    }
    if (!generated || (trailing > 0 && !buildRests(trailing, false, note.triplet, commands))) {
      if (error) {
        *error = "Position or length is not representable in this timing context; no edit was made.";
      }
      return false;
    }
    if (!replaceTimedEvent(track_index, note.rawOffset, commands, error, replaceCount)) {
      return false;
    }
    if (out_tick) {
      *out_tick = tick;
    }
    return true;
  }

  if (!note.isRest) {
    if (sourceNote && sourceNote->segments.size() > 1) {
      if (error) *error = "A tied span needs rest space to preserve its articulation.";
      return false;
    }
    if (tick == note.startTick) {
      if (error) {
        *error = "Cannot insert at the start of a note.";
      }
      return false;
    }
    const uint32_t note_end = note.startTick + note.durationTicks;
    if (tick < note_end) {
      if (error) {
        *error = "Cannot insert while a note is still sounding.";
      }
      return false;
    }
    if (note.slurred || note.durationRate == 0) {
      if (error) {
        *error = "Cannot insert inside tied notes.";
      }
      return false;
    }
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return false;
    }
  }

  const int ir_track_index = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  if (ir_track_index < 0) {
    if (error) {
      *error = "Cannot map track to IR.";
    }
    return false;
  }

  int cmd_index = m_ir->findCmdIndexByOffset(ir_track_index, note.rawOffset);
  if (cmd_index < 0) {
    if (error) {
      *error = "Cannot find event in IR.";
    }
    return false;
  }

  auto *trk = m_ir->track(ir_track_index);
  if (!trk) {
    if (error) {
      *error = "Cannot access IR track.";
    }
    return false;
  }

  const uint32_t desired_len = target_len_ticks;
  const int target_midi = std::clamp(midi_key, 0, 127);
  const int raw_key = target_midi - note.transpose - note.globalTranspose;
  const int octave_offset = static_cast<int>(note.octave) * 12 + (note.octaveUp ? 24 : 0);
  const int key_index_candidate = raw_key - octave_offset + 1;
  const uint8_t new_key_index = clampKeyIndex(key_index_candidate);
  const uint32_t pre_ticks = tick - note.startTick;

  struct LenOption {
    uint8_t index;
    uint32_t len;
  };

  auto build_options = [&](bool dotted, bool triplet) {
    std::vector<LenOption> opts;
    opts.reserve(8);
    // lenIndex starts at 1: index 0 encodes a note/rest byte in the command
    // range (0x00-0x1F) and would corrupt the stream.
    for (uint8_t idx = 1; idx < 8; ++idx) {
      const uint32_t len = lengthFromIndex(idx, dotted, triplet);
      if (len < 1) {
        continue;
      }
      opts.push_back({idx, len});
    }
    return opts;
  };

  const auto options_first = build_options(note.dotted, note.triplet);
  const auto options_plain = build_options(false, note.triplet);

  if (options_first.empty() || options_plain.empty()) {
    if (error) {
      *error = "No valid note lengths for this position.";
    }
    return false;
  }

  struct DecompEntry {
    uint16_t count;
    uint32_t prev;
    uint8_t index;
  };

  const uint16_t kInf = std::numeric_limits<uint16_t>::max();
  std::vector<DecompEntry> dp(base_len + 1, {kInf, 0, 0});
  dp[0] = {0, 0, 0};

  for (uint32_t len = 0; len <= base_len; ++len) {
    if (dp[len].count == kInf) {
      continue;
    }
    for (const auto &opt : options_plain) {
      const uint32_t next = len + opt.len;
      if (next > base_len) {
        continue;
      }
      const uint16_t next_count = static_cast<uint16_t>(dp[len].count + 1);
      if (next_count < dp[next].count) {
        dp[next] = {next_count, len, opt.index};
      }
    }
  }

  auto can_decompose = [&](uint32_t len) {
    return dp[len].count != kInf;
  };

  auto build_decomp = [&](uint32_t len, std::vector<uint8_t> *out) {
    if (!out || !can_decompose(len)) {
      return false;
    }
    out->clear();
    while (len > 0) {
      const auto &entry = dp[len];
      out->push_back(entry.index);
      len = entry.prev;
    }
    std::reverse(out->begin(), out->end());
    return true;
  };

  auto abs_diff = [](uint32_t a, uint32_t b) {
    if (a > b) {
      return a - b;
    }
    return b - a;
  };

  uint8_t first_len_index = 0;
  uint8_t insert_len_index = 0;
  uint32_t pre_len = 0;
  uint32_t pre_remainder_len = 0;
  uint32_t post_len = 0;
  bool insert_first = false;
  bool found = false;
  uint32_t best_pre_diff = std::numeric_limits<uint32_t>::max();
  uint32_t best_len_diff = std::numeric_limits<uint32_t>::max();
  uint32_t best_rest_cmds = std::numeric_limits<uint32_t>::max();
  uint32_t best_post_len = std::numeric_limits<uint32_t>::max();

  if (note.isRest) {
    for (const auto &insert_opt : options_first) {
      if (insert_opt.len > base_len) {
        continue;
      }
      const uint32_t candidate_post = base_len - insert_opt.len;
      if (!can_decompose(candidate_post)) {
        continue;
      }
      const uint32_t pre_diff = pre_ticks;
      const uint32_t len_diff = abs_diff(insert_opt.len, desired_len);
      const uint32_t rest_cmds = static_cast<uint32_t>(dp[candidate_post].count);
      bool better = false;
      if (!found || pre_diff < best_pre_diff) {
        better = true;
      } else if (pre_diff == best_pre_diff && len_diff < best_len_diff) {
        better = true;
      } else if (pre_diff == best_pre_diff && len_diff == best_len_diff && rest_cmds < best_rest_cmds) {
        better = true;
      } else if (pre_diff == best_pre_diff && len_diff == best_len_diff &&
                 rest_cmds == best_rest_cmds && candidate_post < best_post_len) {
        better = true;
      }
      if (better) {
        found = true;
        insert_first = true;
        first_len_index = insert_opt.index;
        insert_len_index = 0;
        pre_len = 0;
        pre_remainder_len = 0;
        post_len = candidate_post;
        best_pre_diff = pre_diff;
        best_len_diff = len_diff;
        best_rest_cmds = rest_cmds;
        best_post_len = candidate_post;
      }
    }

    for (const auto &first_opt : options_first) {
      if (first_opt.len >= base_len) {
        continue;
      }
      for (uint32_t rem_len = 0; rem_len <= base_len; ++rem_len) {
        if (!can_decompose(rem_len)) {
          continue;
        }
        const uint32_t candidate_pre = first_opt.len + rem_len;
        if (candidate_pre >= base_len) {
          continue;
        }
        for (const auto &insert_opt : options_plain) {
          if (candidate_pre + insert_opt.len > base_len) {
            continue;
          }
          const uint32_t candidate_post = base_len - candidate_pre - insert_opt.len;
          if (!can_decompose(candidate_post)) {
            continue;
          }
          const uint32_t pre_diff = abs_diff(candidate_pre, pre_ticks);
          const uint32_t len_diff = abs_diff(insert_opt.len, desired_len);
          const uint32_t rest_cmds = 1u + static_cast<uint32_t>(dp[rem_len].count) +
                                     static_cast<uint32_t>(dp[candidate_post].count);
          bool better = false;
          if (!found || pre_diff < best_pre_diff) {
            better = true;
          } else if (pre_diff == best_pre_diff && len_diff < best_len_diff) {
            better = true;
          } else if (pre_diff == best_pre_diff && len_diff == best_len_diff &&
                     rest_cmds < best_rest_cmds) {
            better = true;
          } else if (pre_diff == best_pre_diff && len_diff == best_len_diff &&
                     rest_cmds == best_rest_cmds && candidate_post < best_post_len) {
            better = true;
          }
          if (better) {
            found = true;
            insert_first = false;
            first_len_index = first_opt.index;
            insert_len_index = insert_opt.index;
            pre_len = candidate_pre;
            pre_remainder_len = rem_len;
            post_len = candidate_post;
            best_pre_diff = pre_diff;
            best_len_diff = len_diff;
            best_rest_cmds = rest_cmds;
            best_post_len = candidate_post;
          }
        }
      }
    }
  } else {
    for (const auto &pre_opt : options_first) {
      if (pre_opt.len >= base_len) {
        continue;
      }
      for (const auto &insert_opt : options_plain) {
        if (pre_opt.len + insert_opt.len > base_len) {
          continue;
        }
        const uint32_t candidate_post = base_len - pre_opt.len - insert_opt.len;
        if (!can_decompose(candidate_post)) {
          continue;
        }
        const uint32_t pre_diff = abs_diff(pre_opt.len, pre_ticks);
        const uint32_t len_diff = abs_diff(insert_opt.len, desired_len);
        const uint32_t rest_cmds = static_cast<uint32_t>(dp[candidate_post].count);
        bool better = false;
        if (!found || pre_diff < best_pre_diff) {
          better = true;
        } else if (pre_diff == best_pre_diff && len_diff < best_len_diff) {
          better = true;
        } else if (pre_diff == best_pre_diff && len_diff == best_len_diff && rest_cmds < best_rest_cmds) {
          better = true;
        } else if (pre_diff == best_pre_diff && len_diff == best_len_diff &&
                   rest_cmds == best_rest_cmds && candidate_post < best_post_len) {
          better = true;
        }
        if (better) {
          found = true;
          first_len_index = pre_opt.index;
          insert_len_index = insert_opt.index;
          pre_len = pre_opt.len;
          post_len = candidate_post;
          best_pre_diff = pre_diff;
          best_len_diff = len_diff;
          best_rest_cmds = rest_cmds;
          best_post_len = candidate_post;
        }
      }
    }
  }

  if (!found || best_pre_diff != 0 || best_len_diff != 0) {
    if (error) {
      *error = "Position or length is not exactly representable in this note's silent tail.";
    }
    return false;
  }

  const uint32_t actual_tick = note.startTick + pre_len;
  if (out_tick) {
    *out_tick = actual_tick;
  }

  std::vector<uint8_t> pre_remainder_indices;
  std::vector<uint8_t> post_indices;
  if (note.isRest && !insert_first && pre_remainder_len > 0) {
    if (!build_decomp(pre_remainder_len, &pre_remainder_indices)) {
      if (error) {
        *error = "Failed to split pre-rest length.";
      }
      return false;
    }
  }
  if (post_len > 0) {
    if (!build_decomp(post_len, &post_indices)) {
      if (error) {
        *error = "Failed to split trailing rest.";
      }
      return false;
    }
  }

  uint8_t pre_rate = note.durationRate;
  uint32_t duration_diff = 0;
  if (!note.isRest) {
    uint32_t best_diff = std::numeric_limits<uint32_t>::max();
    uint32_t best_rate_delta = std::numeric_limits<uint32_t>::max();
    uint8_t best_rate = note.durationRate;
    for (uint32_t rate = 0; rate <= 255; ++rate) {
      const uint32_t duration = durationFromLength(pre_len, static_cast<uint8_t>(rate), note.slurred);
      const uint32_t diff = abs_diff(duration, note.durationTicks);
      const uint32_t rate_delta = abs_diff(rate, static_cast<uint32_t>(note.durationRate));
      if (diff < best_diff || (diff == best_diff && rate_delta < best_rate_delta)) {
        best_diff = diff;
        best_rate_delta = rate_delta;
        best_rate = static_cast<uint8_t>(rate);
      }
      if (best_diff == 0 && best_rate_delta == 0) {
        break;
      }
    }
    pre_rate = best_rate;
    duration_diff = best_diff;
    if (duration_diff != 0) {
      if (error) {
        *error = "Cannot preserve the original sounding duration at this position.";
      }
      return false;
    }
    if (out_duration_diff) {
      *out_duration_diff = duration_diff;
    }
  }

  auto make_rest_cmd = [](uint8_t len_index) {
    CapcomCmdIR rest_cmd;
    rest_cmd.type = CapcomCmdType::Rest;
    rest_cmd.keyIndex = 0;
    rest_cmd.lenIndex = len_index;
    rest_cmd.statusByte = static_cast<uint8_t>((len_index << 5) | 0x00);
    rest_cmd.sizeBytes = 1;
    return rest_cmd;
  };

  auto make_note_cmd = [&](uint8_t len_index) {
    CapcomCmdIR note_cmd;
    note_cmd.type = CapcomCmdType::Note;
    note_cmd.keyIndex = new_key_index;
    note_cmd.lenIndex = len_index;
    note_cmd.statusByte = static_cast<uint8_t>((len_index << 5) | (new_key_index & 0x1F));
    note_cmd.sizeBytes = 1;
    return note_cmd;
  };

  if (note.isRest) {
    auto &cmd = trk->cmds[cmd_index];
    if (insert_first) {
      cmd.type = CapcomCmdType::Note;
      cmd.keyIndex = new_key_index;
      cmd.lenIndex = first_len_index;
      cmd.statusByte = static_cast<uint8_t>((cmd.lenIndex << 5) | (cmd.keyIndex & 0x1F));
      cmd.sizeBytes = 1;
    } else {
      cmd.type = CapcomCmdType::Rest;
      cmd.keyIndex = 0;
      cmd.lenIndex = first_len_index;
      cmd.statusByte = static_cast<uint8_t>((cmd.lenIndex << 5) | (cmd.keyIndex & 0x1F));
      cmd.sizeBytes = 1;
    }

    int insert_at = cmd_index + 1;
    for (uint8_t len_index : pre_remainder_indices) {
      const CapcomCmdIR rest_cmd = make_rest_cmd(len_index);
      if (!m_ir->insertCommand(ir_track_index, insert_at, rest_cmd)) {
        if (error) {
          *error = "Failed to insert leading rest.";
        }
        return false;
      }
      insert_at++;
    }

    if (!insert_first) {
      const CapcomCmdIR new_note = make_note_cmd(insert_len_index);
      if (!m_ir->insertCommand(ir_track_index, insert_at, new_note)) {
        if (error) {
          *error = "Failed to insert note.";
        }
        return false;
      }
      insert_at++;
    }

    for (uint8_t len_index : post_indices) {
      const CapcomCmdIR rest_cmd = make_rest_cmd(len_index);
      if (!m_ir->insertCommand(ir_track_index, insert_at, rest_cmd)) {
        if (error) {
          *error = "Failed to insert trailing rest.";
        }
        return false;
      }
      insert_at++;
    }
  } else {
    const bool change_rate = (pre_rate != note.durationRate);
    if (change_rate) {
      CapcomCmdIR rate_cmd;
      rate_cmd.type = CapcomCmdType::Duration;
      rate_cmd.statusByte = 0x06;
      rate_cmd.params.push_back(pre_rate);
      rate_cmd.durationRate = pre_rate;
      rate_cmd.sizeBytes = 2;
      if (!m_ir->insertCommand(ir_track_index, cmd_index, rate_cmd)) {
        if (error) {
          *error = "Failed to set duration.";
        }
        return false;
      }
      cmd_index++;
    }

    auto &cmd = trk->cmds[cmd_index];
    cmd.type = CapcomCmdType::Note;
    cmd.lenIndex = first_len_index;
    cmd.statusByte = static_cast<uint8_t>((cmd.lenIndex << 5) | (cmd.keyIndex & 0x1F));
    cmd.sizeBytes = 1;

    int insert_at = cmd_index + 1;
    if (change_rate) {
      CapcomCmdIR restore_cmd;
      restore_cmd.type = CapcomCmdType::Duration;
      restore_cmd.statusByte = 0x06;
      restore_cmd.params.push_back(note.durationRate);
      restore_cmd.durationRate = note.durationRate;
      restore_cmd.sizeBytes = 2;
      if (!m_ir->insertCommand(ir_track_index, insert_at, restore_cmd)) {
        if (error) {
          *error = "Failed to restore duration.";
        }
        return false;
      }
      insert_at++;
    }

    const CapcomCmdIR new_note = make_note_cmd(insert_len_index);
    if (!m_ir->insertCommand(ir_track_index, insert_at, new_note)) {
      if (error) {
        *error = "Failed to insert note.";
      }
      return false;
    }
    insert_at++;

    for (uint8_t len_index : post_indices) {
      const CapcomCmdIR rest_cmd = make_rest_cmd(len_index);
      if (!m_ir->insertCommand(ir_track_index, insert_at, rest_cmd)) {
        if (error) {
          *error = "Failed to insert trailing rest.";
        }
        return false;
      }
      insert_at++;
    }
  }

  std::vector<uint8_t> before;
  before.reserve(m_raw->size());
  for (size_t i = 0; i < m_raw->size(); ++i) {
    before.push_back(m_raw->readByte(i));
  }

  if (!m_ir->serializeToRaw(m_raw, error)) {
    // the in-memory IR now holds the failed edit; drop it so the next
    // operation re-parses from the untouched raw instead of committing it
    m_ir.reset();
    return false;
  }

  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("InsertNoteAtTick: failed to sync track offsets from header");
  }

  std::vector<EditEntry> batch;
  batch.reserve(before.size() / 8);
  for (size_t i = 0; i < before.size(); ++i) {
    const uint8_t newVal = m_raw->readByte(i);
    const uint8_t oldVal = before[i];
    if (newVal == oldVal) {
      continue;
    }
    batch.push_back(EditEntry{m_raw, static_cast<uint32_t>(i), oldVal, newVal});
  }

  if (!batch.empty()) {
    recordEdit(m_raw, batch.front().offset);
    m_redo.clear();
    pushUndoBatch(std::move(batch));
  }
  reload();
  return true;
}

bool CapcomPianoRollModel::MoveNote(int trackIndex,
                                    size_t noteIndex,
                                    uint32_t tick,
                                    int midiKey,
                                    uint32_t *out_tick,
                                    std::string *error) {
  if (out_tick) {
    *out_tick = 0;
  }

  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  const auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track selection.";
    }
    return false;
  }

  if (noteIndex >= data->notes.size()) {
    if (error) {
      *error = "Invalid note selection.";
    }
    return false;
  }

  const auto note = data->notes[noteIndex];
  if (note.midiKey < 0 || note.isRest) {
    if (error) {
      *error = "Cannot move rests.";
    }
    return false;
  }
  if (note.isSongLoopReplay) {
    if (error) {
      *error = "That note is the loop replaying; edit it in the first pass of the song.";
    }
    return false;
  }

  const uint32_t clampedTick = tick;
  const int clampedMidi = std::clamp(midiKey, 0, 127);

  if (clampedTick == note.startTick) {
    if (out_tick) {
      *out_tick = note.startTick;
    }
    return applyEdit(trackIndex, noteIndex, clampedMidi, note.deltaTicks, false, error);
  }

  const uint32_t newStart = clampedTick;
  const uint32_t newEnd = newStart + note.durationTicks;
  for (size_t i = 0; i < data->notes.size(); ++i) {
    if (i == noteIndex) {
      continue;
    }
    const auto &other = data->notes[i];
    if (other.midiKey < 0 || other.isRest || other.isSongLoopReplay) {
      continue;
    }
    const uint32_t otherStart = other.startTick;
    const uint32_t otherEnd = otherStart + other.durationTicks;
    const bool overlap = newStart < otherEnd && otherStart < newEnd;
    if (overlap) {
      if (error) {
        *error = "Move would overlap an existing note.";
      }
      return false;
    }
  }

  const size_t undoBefore = m_undo.size();
  const size_t redoBefore = m_redo.size();

  std::string tmpError;
  if (!eraseNotes({{trackIndex, static_cast<int>(noteIndex)}}, &tmpError)) {
    if (error) {
      *error = tmpError;
    }
    return false;
  }

  uint32_t insertedTick = 0;
  uint32_t durationDiff = 0;
  bool inserted = InsertNoteAtTick(trackIndex,
                        clampedTick,
                        clampedMidi,
                        note.deltaTicks,
                        &insertedTick,
                        &durationDiff,
                        &tmpError, &note);
  if (!inserted && tmpError.find("No event at this position") != std::string::npos) {
    tmpError.clear();
    inserted = AppendNoteAtTick(trackIndex, clampedTick, clampedMidi, note.deltaTicks, &tmpError, &note);
    if (inserted) insertedTick = clampedTick;
  }
  if (!inserted) {
    std::string undoError;
    undo(&undoError);
    m_redo.clear();
    if (error) {
      *error = tmpError;
      if (!undoError.empty()) {
        *error += " (undo also failed: " + undoError + ")";
      }
    }
    return false;
  }

  if (out_tick) {
    *out_tick = insertedTick;
  }

  if (m_undo.size() >= undoBefore + 2) {
    auto insertBatch = std::move(m_undo.back());
    m_undo.pop_back();
    auto eraseBatch = std::move(m_undo.back());
    m_undo.pop_back();

    std::unordered_map<uint32_t, EditEntry> merged;
    merged.reserve(insertBatch.size() + eraseBatch.size());

    for (const auto &entry : insertBatch) {
      merged.emplace(entry.offset, entry);
    }

    for (const auto &entry : eraseBatch) {
      auto it = merged.find(entry.offset);
      if (it != merged.end()) {
        it->second.oldVal = entry.oldVal;
      } else {
        merged.emplace(entry.offset, entry);
      }
    }

    std::vector<EditEntry> mergedBatch;
    mergedBatch.reserve(merged.size());
    for (auto &kv : merged) {
      mergedBatch.push_back(kv.second);
    }
    std::sort(mergedBatch.begin(), mergedBatch.end(), [](const EditEntry &a, const EditEntry &b) {
      return a.offset < b.offset;
    });

    m_undo.push_back(std::move(mergedBatch));
    if (m_undo.size() > 64) {
      m_undo.erase(m_undo.begin());
    }
  }

  if (m_redo.size() != redoBefore) {
    m_redo.clear();
  }

  return true;
}

bool CapcomPianoRollModel::eraseNotes(const std::vector<std::pair<int, int>> &noteRefs, std::string *error) {
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  // Resize/move also erase first. Validate the ENTIRE selection before any
  // write: a displayed tied span owns multiple event bytes, and erasing only
  // its first one would leave audible continuations behind.
  // Erasing edits bytes in place, so on shared melody bytes it works and every
  // channel that plays them hears it (see test_shared_song_edit); only
  // structural IR edits need rejectBorrowed.
  for (const auto &[trackIndex, noteIndex] : noteRefs) {
    const auto *data = trackData(trackIndex);
    if (!data || noteIndex < 0 || noteIndex >= static_cast<int>(data->notes.size())) {
      if (error) {
        *error = "Invalid note selection.";
      }
      return false;
    }
  }

  std::unordered_set<uint32_t> seenOffsets;
  std::vector<EditEntry> batch;
  for (const auto &[trackIndex, noteIndex] : noteRefs) {
    const auto *data = trackData(trackIndex);
    if (!data || noteIndex < 0 || noteIndex >= static_cast<int>(data->notes.size())) {
      if (error) {
        *error = "Invalid note selection.";
      }
      return false;
    }
    const auto &note = data->notes[static_cast<size_t>(noteIndex)];
    for (const auto& segment : note.segments) {
      if (!seenOffsets.insert(segment.origAbsOffset).second) continue;
      const uint8_t newStatus = static_cast<uint8_t>(segment.statusByte & 0xE0);
      if (!m_raw->writeByte(segment.origAbsOffset, newStatus)) {
        if (error) *error = "Failed to write to raw file buffer.";
        return false;
      }
      batch.push_back(EditEntry{m_raw, segment.origAbsOffset, segment.statusByte, newStatus});
      recordEdit(m_raw, segment.origAbsOffset);
    }
  }

  if (!batch.empty()) {
    m_redo.clear();
    pushUndoBatch(std::move(batch));
  }
  reload();
  return true;
}

bool CapcomPianoRollModel::setInstrument(const std::vector<std::pair<int, int>> &noteRefs,
                                         uint8_t program,
                                         std::string *error) {
  L_INFO("setInstrument called with {} notes, program=0x{:02x}", noteRefs.size(), program);

  if (!m_raw || !m_raw->isWritable()) {
    L_WARN("setInstrument: raw file not writable");
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  if (!m_ir) {
    L_INFO("setInstrument: creating IR");
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      L_ERROR("setInstrument: failed to parse IR");
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return false;
    }
    L_INFO("setInstrument: IR parsed successfully");
  }

  std::unordered_set<int64_t> selectedSet;
  for (const auto &[ti, ni] : noteRefs) {
    selectedSet.insert((static_cast<int64_t>(ti) << 32) | static_cast<int64_t>(ni));
  }

  struct NoteInfo {
    int trackIndex;
    int irTrackIndex;
    int noteIndex;
    uint32_t rawOffset;
    uint8_t currentProgram;
    int irCmdIndex;
  };
  bool anyChange = false;
  std::vector<NoteInfo> sortedNotes;
  for (const auto &[trackIndex, noteIndex] : noteRefs) {
    const auto *data = trackData(trackIndex);
    if (!data || noteIndex < 0 || noteIndex >= static_cast<int>(data->notes.size())) {
      L_WARN("setInstrument: invalid note selection track={} note={}", trackIndex, noteIndex);
      if (error) {
        *error = "Invalid note selection.";
      }
      return false;
    }
    const auto &note = data->notes[static_cast<size_t>(noteIndex)];
    L_INFO("setInstrument: note track={} idx={} offset=0x{:x} currentProg=0x{:02x}", 
           trackIndex, noteIndex, note.rawOffset, note.program);
    if (rejectBorrowed(trackIndex, note.rawOffset, error)) {
      return false;
    }
    if (note.program != program) {
      anyChange = true;
    }
    int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
    if (irTrackIndex < 0) {
      L_ERROR("setInstrument: cannot map track start 0x{:x} to IR track", data->trackOffset);
      if (error) {
        *error = "Cannot map track to IR.";
      }
      return false;
    }
    int irIdx = m_ir->findCmdIndexByOffset(irTrackIndex, note.rawOffset);
    if (irIdx < 0) {
      L_ERROR("setInstrument: cannot find note in IR at offset 0x{:x}", note.rawOffset);
      if (error) {
        *error = "Cannot find note in IR.";
      }
      return false;
    }
    L_INFO("setInstrument: found note in IR at cmdIndex={}", irIdx);
    sortedNotes.push_back({trackIndex, irTrackIndex, noteIndex, note.rawOffset, note.program, irIdx});
  }

  if (sortedNotes.empty()) {
    return true;
  }

  if (!anyChange) {
    return true;
  }

  struct InsertOp {
    int trackIndex;
    int beforeCmdIndex;
    uint8_t programValue;
  };
  std::vector<InsertOp> insertions;

  std::unordered_map<int, std::vector<NoteInfo>> notesByTrack;
  for (const auto &note : sortedNotes) {
    notesByTrack[note.trackIndex].push_back(note);
  }

  for (auto &[trackIndex, notes] : notesByTrack) {
    std::sort(notes.begin(), notes.end(),
              [](const NoteInfo &a, const NoteInfo &b) {
                return a.noteIndex < b.noteIndex;
              });

    for (size_t i = 0; i < notes.size(); ++i) {
      const auto &note = notes[i];
      const bool segmentStart = (i == 0) || (notes[i - 1].noteIndex + 1 != note.noteIndex);
      const bool programChanged = (i > 0) && (notes[i - 1].currentProgram != note.currentProgram);
      if ((segmentStart || programChanged) && note.currentProgram != program) {
        insertions.push_back({note.irTrackIndex, note.irCmdIndex, program});
      }

      const auto *data = trackData(trackIndex);
      if (data) {
        int nextNoteIndex = note.noteIndex + 1;
        if (nextNoteIndex < static_cast<int>(data->notes.size())) {
          int64_t nextKey = (static_cast<int64_t>(trackIndex) << 32) | static_cast<int64_t>(nextNoteIndex);
          bool nextIsSelected = selectedSet.find(nextKey) != selectedSet.end();
          if (!nextIsSelected) {
            const auto &nextNote = data->notes[static_cast<size_t>(nextNoteIndex)];
            int nextIrIdx = m_ir->findCmdIndexByOffset(note.irTrackIndex, nextNote.rawOffset);
            if (nextIrIdx >= 0 && nextIrIdx != note.irCmdIndex && nextNote.program != program) {
              insertions.push_back({note.irTrackIndex, nextIrIdx, nextNote.program});
            }
          }
        }
      }
    }
  }

  std::sort(insertions.begin(), insertions.end(),
            [](const InsertOp &a, const InsertOp &b) {
              if (a.trackIndex != b.trackIndex) {
                return a.trackIndex < b.trackIndex;
              }
              if (a.beforeCmdIndex != b.beforeCmdIndex) {
                return a.beforeCmdIndex > b.beforeCmdIndex;
              }
              return a.programValue < b.programValue;
            });

  // One program change per insertion point. Two can collide when a repeat
  // body replays: the "restore" queued for the note after the selection can be
  // the same bytes as a selected note, so both land before the same command.
  // The selected note's new program must win; keeping both let the sort order
  // (by program value) decide, and the old instrument survived whenever the new
  // program number was higher.
  {
    std::vector<InsertOp> kept;
    for (const auto &op : insertions) {
      auto same = std::find_if(kept.begin(), kept.end(), [&](const InsertOp &k) {
        return k.trackIndex == op.trackIndex && k.beforeCmdIndex == op.beforeCmdIndex;
      });
      if (same == kept.end()) {
        kept.push_back(op);
      } else if (op.programValue == program) {
        *same = op;
      }
    }
    insertions.swap(kept);
  }

  std::vector<uint8_t> before;
  before.reserve(m_raw->size());
  for (size_t i = 0; i < m_raw->size(); ++i) {
    before.push_back(m_raw->readByte(i));
  }

  for (const auto &ins : insertions) {
    L_INFO("CapcomEdit IR insertProgramChange track={}, beforeCmd={}, program=0x{:02x}",
           ins.trackIndex, ins.beforeCmdIndex, ins.programValue);
    if (!m_ir->insertProgramChange(ins.trackIndex, ins.beforeCmdIndex, ins.programValue)) {
      if (error) {
        *error = "Failed to insert program change in IR.";
      }
      m_ir.reset();  // may hold earlier inserts; re-parse from the untouched raw next edit
      return false;
    }
  }

  // Self-clean: each edit leaves the previous edit's "restore" ProgramChange
  // sitting right in front of the one just inserted (PC old, PC new, note),
  // so repeated instrument changes grew the track by 2 dead bytes each time.
  {
    std::set<int> touched;
    for (const auto &ins : insertions) touched.insert(ins.trackIndex);
    const int dropped = dropDeadProgramChanges(touched);
    if (dropped > 0) {
      L_INFO("setInstrument: dropped {} dead ProgramChange command(s)", dropped);
    }
  }

  if (!m_ir->serializeToRaw(m_raw, error)) {
    // the in-memory IR now holds the failed edit; drop it so the next
    // operation re-parses from the untouched raw instead of committing it
    m_ir.reset();
    return false;
  }

  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("setInstrument: failed to sync track offsets from header");
  }

  std::vector<EditEntry> batch;
  batch.reserve(before.size() / 8);
  for (size_t i = 0; i < before.size(); ++i) {
    const uint8_t newVal = m_raw->readByte(i);
    const uint8_t oldVal = before[i];
    if (newVal == oldVal) {
      continue;
    }
    batch.push_back(EditEntry{m_raw, static_cast<uint32_t>(i), oldVal, newVal});
  }

  if (!batch.empty()) {
    recordEdit(m_raw, batch.front().offset);
    m_redo.clear();
    pushUndoBatch(std::move(batch));
  }

  reload();
  return true;
}

bool CapcomPianoRollModel::updateSetting(int trackIndex,
                                         size_t settingIndex,
                                         CapcomSettingType type,
                                         uint16_t value,
                                         uint8_t value2,
                                         std::string *error) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (settingIndex >= data->settings.size()) {
    if (error) {
      *error = "Invalid setting selection.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  const auto &setting = data->settings[settingIndex];
  if (setting.type != type) {
    if (error) {
      *error = "Changing setting type is not supported; remove and add a new event.";
    }
    return false;
  }

  std::vector<uint8_t> newBytes;
  if (!BuildSettingWriteBytes(setting, value, value2, &newBytes, error)) {
    return false;
  }

  std::vector<EditEntry> batch;
  for (size_t i = 0; i < newBytes.size(); ++i) {
    const uint32_t offset = setting.rawOffset + static_cast<uint32_t>(i);
    const uint8_t oldVal = m_raw->readByte(offset);
    const uint8_t newVal = newBytes[i];
    if (oldVal == newVal) {
      continue;
    }
    if (!m_raw->writeByte(offset, newVal)) {
      if (error) {
        *error = "Failed to write to raw file buffer.";
      }
      return false;
    }
    batch.push_back(EditEntry{m_raw, offset, oldVal, newVal});
    recordEdit(m_raw, offset);
  }

  if (batch.empty()) {
    return true;
  }

  m_redo.clear();
  pushUndoBatch(std::move(batch));
  reload();
  return true;
}

bool CapcomPianoRollModel::addSetting(int trackIndex,
                                      uint32_t tick,
                                      CapcomSettingType type,
                                      uint16_t value,
                                      uint8_t value2,
                                      std::string *error) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return false;
    }
  }

  int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  if (irTrackIndex < 0) {
    if (error) {
      *error = "Cannot map track to IR.";
    }
    return false;
  }

  uint32_t candidateTick = 0;
  uint32_t candidateOffset = 0;
  bool foundCandidate = false;
  for (const auto &note : data->notes) {
    if (note.startTick >= tick) {
      if (!foundCandidate || note.startTick < candidateTick ||
          (note.startTick == candidateTick && note.rawOffset < candidateOffset)) {
        foundCandidate = true;
        candidateTick = note.startTick;
        candidateOffset = note.rawOffset;
      }
    }
  }
  for (const auto &setting : data->settings) {
    if (setting.tick >= tick) {
      if (!foundCandidate || setting.tick < candidateTick ||
          (setting.tick == candidateTick && setting.rawOffset < candidateOffset)) {
        foundCandidate = true;
        candidateTick = setting.tick;
        candidateOffset = setting.rawOffset;
      }
    }
  }

  uint32_t trackEndTick = 0;
  for (const auto &note : data->notes) {
    trackEndTick = std::max<uint32_t>(trackEndTick, note.startTick + note.deltaTicks);
  }
  for (const auto &setting : data->settings) {
    trackEndTick = std::max<uint32_t>(trackEndTick, setting.tick);
  }

  int beforeCmdIndex = -1;
  uint32_t actualTick = trackEndTick;
  if (foundCandidate) {
    beforeCmdIndex = m_ir->findCmdIndexByOffset(irTrackIndex, candidateOffset);
    actualTick = candidateTick;
  }
  if (foundCandidate && beforeCmdIndex < 0) {
    if (error) {
      *error = "Cannot map target position to IR.";
    }
    return false;
  }

  auto *trk = m_ir->track(irTrackIndex);
  if (!trk) {
    if (error) {
      *error = "Cannot access IR track.";
    }
    return false;
  }
  if (beforeCmdIndex < 0) {
    beforeCmdIndex = static_cast<int>(trk->cmds.size());
  }

  SettingCommandSpec spec;
  if (!BuildSettingCommand(type, value, value2, &spec, error)) {
    return false;
  }

  CapcomCmdIR cmd;
  cmd.type = spec.type;
  cmd.statusByte = spec.statusByte;
  cmd.params = spec.params;
  cmd.sizeBytes = static_cast<uint8_t>(1 + cmd.params.size());
  cmd.origAbsOffset = 0xFFFFFFFF;
  cmd.tick = actualTick;

  if (!m_ir->insertCommand(irTrackIndex, beforeCmdIndex, cmd)) {
    if (error) {
      *error = "Failed to insert setting command.";
    }
    return false;
  }

  const auto undoSnap = captureRawSnapshot();
  if (!m_ir->serializeToRaw(m_raw, error)) {
    // the in-memory IR now holds the failed edit; drop it so the next
    // operation re-parses from the untouched raw instead of committing it
    m_ir.reset();
    return false;
  }

  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("addSetting: failed to sync track offsets from header");
  }

  m_redo.clear();
  pushRawDiffUndo(undoSnap);

  reload();
  return true;
}

bool CapcomPianoRollModel::removeSetting(int trackIndex, size_t settingIndex, std::string *error) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (settingIndex >= data->settings.size()) {
    if (error) {
      *error = "Invalid setting selection.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return false;
    }
  }

  const auto &setting = data->settings[settingIndex];
  int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  if (irTrackIndex < 0) {
    if (error) {
      *error = "Cannot map track to IR.";
    }
    return false;
  }
  int cmdIndex = m_ir->findCmdIndexByOffset(irTrackIndex, setting.rawOffset);
  if (cmdIndex < 0) {
    if (error) {
      *error = "Cannot find setting in IR.";
    }
    return false;
  }

  if (!m_ir->removeCommand(irTrackIndex, cmdIndex)) {
    if (error) {
      *error = "Failed to remove setting command.";
    }
    return false;
  }

  const auto undoSnap = captureRawSnapshot();
  if (!m_ir->serializeToRaw(m_raw, error)) {
    // the in-memory IR now holds the failed edit; drop it so the next
    // operation re-parses from the untouched raw instead of committing it
    m_ir.reset();
    return false;
  }

  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("removeSetting: failed to sync track offsets from header");
  }

  m_redo.clear();
  pushRawDiffUndo(undoSnap);
  reload();
  return true;
}

bool CapcomPianoRollModel::updateCommandParam(int trackIndex,
                                              int cmdIndex,
                                              int paramIndex,
                                              uint8_t value,
                                              std::string *error) {
  const auto* data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return false;
    }
  }

  const int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  if (irTrackIndex < 0) {
    if (error) {
      *error = "Cannot map track to IR.";
    }
    return false;
  }
  auto* trk = m_ir->track(irTrackIndex);
  if (!trk) {
    if (error) {
      *error = "Cannot access IR track.";
    }
    return false;
  }
  if (cmdIndex < 0 || cmdIndex >= static_cast<int>(trk->cmds.size())) {
    if (error) {
      *error = "Invalid command index.";
    }
    return false;
  }
  auto& cmd = trk->cmds[static_cast<size_t>(cmdIndex)];
  if (cmd.type == CapcomCmdType::Note || cmd.type == CapcomCmdType::Rest) {
    if (error) {
      *error = "Use the piano roll editor to edit notes.";
    }
    return false;
  }
  if (cmd.type == CapcomCmdType::RepeatUntil || cmd.type == CapcomCmdType::RepeatBreak ||
      cmd.type == CapcomCmdType::Goto) {
    if (error) {
      *error = "Editing pointer-based commands is not supported.";
    }
    return false;
  }

  if (paramIndex < 0 || paramIndex >= static_cast<int>(cmd.params.size())) {
    if (error) {
      *error = "Invalid parameter index.";
    }
    return false;
  }

  const uint32_t writeOffset = cmd.origAbsOffset + 1u + static_cast<uint32_t>(paramIndex);
  if (!m_raw->isValidOffset(writeOffset)) {
    if (error) {
      *error = "Command parameter offset out of range.";
    }
    return false;
  }

  const uint8_t oldVal = m_raw->readByte(writeOffset);
  const uint8_t newVal = value;
  if (oldVal == newVal) {
    return true;
  }

  if (!m_raw->writeByte(writeOffset, newVal)) {
    if (error) {
      *error = "Failed to write command parameter.";
    }
    return false;
  }

  recordEdit(m_raw, writeOffset);
  m_redo.clear();
  std::vector<EditEntry> batch;
  batch.push_back(EditEntry{m_raw, writeOffset, oldVal, newVal});
  pushUndoBatch(std::move(batch));
  reload();
  return true;
}

bool CapcomPianoRollModel::updateLoopRepeatCount(int trackIndex,
                                                 size_t loopIndex,
                                                 uint8_t repeatCount,
                                                 std::string *error) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (loopIndex >= data->loops.size()) {
    if (error) {
      *error = "Invalid loop selection.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  const auto &loop = data->loops[loopIndex];
  const uint32_t countOffset = loop.rawOffset + 1;
  if (!m_raw->isValidOffset(loop.rawOffset) || !m_raw->isValidOffset(countOffset)) {
    if (error) {
      *error = "Loop command offset out of range.";
    }
    return false;
  }

  const uint8_t oldVal = m_raw->readByte(countOffset);
  const uint8_t newVal = repeatCount;
  if (oldVal == newVal) {
    return true;
  }

  if (!m_raw->writeByte(countOffset, newVal)) {
    if (error) {
      *error = "Failed to write loop count.";
    }
    return false;
  }

  recordEdit(m_raw, countOffset);
  m_redo.clear();
  std::vector<EditEntry> batch;
  batch.push_back(EditEntry{m_raw, countOffset, oldVal, newVal});
  pushUndoBatch(std::move(batch));
  reload();
  return true;
}

bool CapcomPianoRollModel::createLoop(int trackIndex,
                                      uint32_t startTick,
                                      uint32_t endTick,
                                      uint8_t slot,
                                      uint8_t repeatCount,
                                      std::string *error) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }
  if (startTick >= endTick) {
    if (error) {
      *error = "Loop start tick must be before end tick.";
    }
    return false;
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return false;
    }
  }

  int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  if (irTrackIndex < 0) {
    if (error) {
      *error = "Cannot map track to IR.";
    }
    return false;
  }

  uint32_t startOffset = 0;
  bool foundStartOffset = false;
  if (startTick == 0) {
    foundStartOffset = true;
  } else {
    for (const auto &note : data->notes) {
      if (note.startTick == startTick) {
        if (!foundStartOffset || note.rawOffset < startOffset) {
          foundStartOffset = true;
          startOffset = note.rawOffset;
        }
      }
    }
    for (const auto &setting : data->settings) {
      if (setting.tick == startTick) {
        if (!foundStartOffset || setting.rawOffset < startOffset) {
          foundStartOffset = true;
          startOffset = setting.rawOffset;
        }
      }
    }
  }

  int startCmdIndex = 0;
  if (startTick != 0) {
    if (!foundStartOffset) {
      if (error) {
        *error = "Loop start must match an existing note/setting tick (or use 0).";
      }
      return false;
    }
    startCmdIndex = m_ir->findCmdIndexByOffset(irTrackIndex, startOffset);
    if (startCmdIndex < 0) {
      if (error) {
        *error = "Cannot map loop start to IR.";
      }
      return false;
    }
  }

  uint32_t candidateTick = 0;
  uint32_t candidateOffset = 0;
  bool foundCandidate = false;
  for (const auto &note : data->notes) {
    if (note.startTick >= endTick) {
      if (!foundCandidate || note.startTick < candidateTick ||
          (note.startTick == candidateTick && note.rawOffset < candidateOffset)) {
        foundCandidate = true;
        candidateTick = note.startTick;
        candidateOffset = note.rawOffset;
      }
    }
  }
  for (const auto &setting : data->settings) {
    if (setting.tick >= endTick) {
      if (!foundCandidate || setting.tick < candidateTick ||
          (setting.tick == candidateTick && setting.rawOffset < candidateOffset)) {
        foundCandidate = true;
        candidateTick = setting.tick;
        candidateOffset = setting.rawOffset;
      }
    }
  }
  for (const auto &loop : data->loops) {
    if (loop.tick >= endTick) {
      if (!foundCandidate || loop.tick < candidateTick ||
          (loop.tick == candidateTick && loop.rawOffset < candidateOffset)) {
        foundCandidate = true;
        candidateTick = loop.tick;
        candidateOffset = loop.rawOffset;
      }
    }
  }

  int beforeCmdIndex = -1;
  if (foundCandidate) {
    beforeCmdIndex = m_ir->findCmdIndexByOffset(irTrackIndex, candidateOffset);
  }
  if (foundCandidate && beforeCmdIndex < 0) {
    if (error) {
      *error = "Cannot map loop end to IR.";
    }
    return false;
  }

  auto *trk = m_ir->track(irTrackIndex);
  if (!trk) {
    if (error) {
      *error = "Cannot access IR track.";
    }
    return false;
  }
  if (beforeCmdIndex < 0) {
    beforeCmdIndex = static_cast<int>(trk->cmds.size());
  }
  // A loop at the end must precede END/GOTO; bytes after it are unreachable.
  if (beforeCmdIndex == static_cast<int>(trk->cmds.size()) && beforeCmdIndex > 0 &&
      (trk->cmds.back().type == CapcomCmdType::End || trk->cmds.back().type == CapcomCmdType::Goto)) {
    --beforeCmdIndex;
  }
  if (beforeCmdIndex <= startCmdIndex) {
    if (error) {
      *error = "Loop end must be after loop start.";
    }
    return false;
  }

  const uint8_t clampedSlot = static_cast<uint8_t>(std::min<uint8_t>(slot, 3));
  const uint16_t destWord = static_cast<uint16_t>(
      (startTick == 0 ? data->trackOffset : startOffset) & 0xFFFFu);
  CapcomCmdIR cmd;
  cmd.type = CapcomCmdType::RepeatUntil;
  cmd.statusByte = static_cast<uint8_t>(0x0E + clampedSlot);
  cmd.repeatSlot = clampedSlot;
  cmd.repeatCount = repeatCount;
  cmd.destWord = destWord;
  cmd.destTrackIndex = irTrackIndex;
  cmd.destCmdIndex = startCmdIndex;
  cmd.params = {repeatCount,
                static_cast<uint8_t>((destWord >> 8) & 0xFF),
                static_cast<uint8_t>(destWord & 0xFF)};
  cmd.sizeBytes = 4;
  cmd.origAbsOffset = 0xFFFFFFFF;

  if (!m_ir->insertCommand(irTrackIndex, beforeCmdIndex, cmd)) {
    if (error) {
      *error = "Failed to insert loop command.";
    }
    return false;
  }

  const auto undoSnap = captureRawSnapshot();
  if (!m_ir->serializeToRaw(m_raw, error)) {
    // the in-memory IR now holds the failed edit; drop it so the next
    // operation re-parses from the untouched raw instead of committing it
    m_ir.reset();
    return false;
  }

  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("createLoop: failed to sync track offsets from header");
  }

  m_redo.clear();
  pushRawDiffUndo(undoSnap);
  reload();
  return true;
}

bool CapcomPianoRollModel::removeLoop(int trackIndex, size_t loopIndex, std::string *error) {
  auto *data = trackData(trackIndex);
  if (!data) {
    if (error) {
      *error = "Invalid track.";
    }
    return false;
  }
  if (loopIndex >= data->loops.size()) {
    if (error) {
      *error = "Invalid loop selection.";
    }
    return false;
  }
  if (!m_raw || !m_raw->isWritable()) {
    if (error) {
      *error = "Backing file is not writable.";
    }
    return false;
  }

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) {
        *error = "Failed to parse sequence IR.";
      }
      return false;
    }
  }

  const auto &loop = data->loops[loopIndex];
  int irTrackIndex = m_ir->findTrackIndexByStartOffset(data->trackOffset);
  if (irTrackIndex < 0) {
    if (error) {
      *error = "Cannot map track to IR.";
    }
    return false;
  }
  int cmdIndex = m_ir->findCmdIndexByOffset(irTrackIndex, loop.rawOffset);
  if (cmdIndex < 0) {
    if (error) {
      *error = "Cannot find loop command in IR.";
    }
    return false;
  }

  if (!m_ir->removeCommand(irTrackIndex, cmdIndex)) {
    if (error) {
      *error = "Failed to remove loop command.";
    }
    return false;
  }

  const auto undoSnap = captureRawSnapshot();
  if (!m_ir->serializeToRaw(m_raw, error)) {
    // the in-memory IR now holds the failed edit; drop it so the next
    // operation re-parses from the untouched raw instead of committing it
    m_ir.reset();
    return false;
  }

  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("removeLoop: failed to sync track offsets from header");
  }

  m_redo.clear();
  pushRawDiffUndo(undoSnap);
  reload();
  return true;
}

bool CapcomPianoRollModel::endSongLoops(std::string *error) {
  if (!m_raw || !m_raw->isWritable()) {
    if (error) *error = "Backing file is not writable.";
    return false;
  }
  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) *error = "Failed to parse sequence IR.";
      return false;
    }
  }
  bool changed = false;
  std::set<uint32_t> converted;  // a GOTO shared by several channels is converted once
  for (const auto &d : m_tracks) {
    if (!d.hasSongLoop || converted.count(d.songLoopGotoOffset) > 0) {
      continue;
    }
    // The loop GOTO found by parseTrack (not "the last command": tracks can end
    // `goto ; end`, and a GOTO into another channel's melody is not a loop).
    // Convert it where it lives: its home track's stream.
    const int home = homeTrackOfOffset(d.songLoopGotoOffset);
    if (home < 0) {
      continue;
    }
    const int ti = m_ir->findTrackIndexByStartOffset(m_tracks[static_cast<size_t>(home)].trackOffset);
    const int loopIdx = ti < 0 ? -1 : m_ir->findCmdIndexByOffset(ti, d.songLoopGotoOffset);
    if (loopIdx < 0) {
      continue;  // verified below: a loop we could not reach makes the whole edit fail
    }
    converted.insert(d.songLoopGotoOffset);
    // Swap only the GOTO for an END. Anything after it stays: other channels
    // may play melodies stored there. Jumps that landed on the GOTO (a repeat's
    // exit) must land on the END; remove+insert at one index leaves every
    // other index unchanged, so remember them and repoint afterwards.
    std::vector<std::pair<int, int>> landers;
    for (int t = 0; t < 8; ++t) {
      const auto *other = m_ir->track(t);
      if (!other) {
        continue;
      }
      for (int i = 0; i < static_cast<int>(other->cmds.size()); ++i) {
        const auto &c = other->cmds[static_cast<size_t>(i)];
        if (c.destTrackIndex == ti && c.destCmdIndex == loopIdx && !(t == ti && i == loopIdx)) {
          landers.emplace_back(t, i);
        }
      }
    }
    CapcomCmdIR end;
    end.type = CapcomCmdType::End;
    end.statusByte = 0x17;
    end.sizeBytes = 1;
    end.origAbsOffset = 0xFFFFFFFF;
    if (!m_ir->removeCommand(ti, loopIdx) || !m_ir->insertCommand(ti, loopIdx, end)) {
      if (error) *error = "Failed to end the song loop.";
      m_ir.reset();
      return false;
    }
    for (const auto &[t, i] : landers) {
      m_ir->track(t)->cmds[static_cast<size_t>(i)].destCmdIndex = loopIdx;
    }
    changed = true;
  }
  if (!changed) return true;  // nothing looped

  const auto undoSnap = captureRawSnapshot();
  if (!m_ir->serializeToRaw(m_raw, error)) {
    m_ir.reset();
    return false;
  }
  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("endSongLoops: failed to sync track offsets from header");
  }

  // Verify with the same definition parseTrack uses: after re-parsing, no
  // channel may still jump back into bytes it already played. (GOTOs into
  // another channel's melody are not loops and correctly survive.) If any loop
  // remains - e.g. one we could not reach - roll back completely rather than
  // ship a half-ended song.
  const bool parsed = reload();
  bool stillLoops = !parsed;
  for (const auto &d : m_tracks) {
    if (d.hasSongLoop) {
      stillLoops = true;
    }
  }
  if (stillLoops) {
    // restore the pre-edit bytes and report; nothing is committed
    m_raw->writeBytes(0, undoSnap);
    SyncSeqTrackOffsetsFromHeader(m_seq, m_raw);
    reload();
    if (error) {
      *error = "This song's loop could not be ended on every channel, so nothing was changed.";
    }
    return false;
  }

  m_redo.clear();
  pushRawDiffUndo(undoSnap);
  return true;
}

int CapcomPianoRollModel::dropDeadProgramChanges(const std::set<int> &trackIndices) {
  if (!m_ir) return 0;

  // Same landing-point discipline as optimizeSequence: a command a jump
  // lands on is never removed, whether the jump comes from this track
  // (index-based) or another one (matched by home offset).
  std::set<std::pair<int, int>> jumpTargets;
  std::set<uint32_t> jumpTargetOffsets;
  for (int ti = 0; ti < 8; ++ti) {
    const auto *trk = m_ir->track(ti);
    if (!trk) continue;
    for (const auto &c : trk->cmds) {
      if (c.destCmdIndex < 0) continue;
      jumpTargets.insert({c.destTrackIndex, c.destCmdIndex});
      const auto *dtrk = m_ir->track(c.destTrackIndex);
      if (dtrk && c.destCmdIndex < static_cast<int>(dtrk->cmds.size())) {
        const uint32_t off = dtrk->cmds[static_cast<size_t>(c.destCmdIndex)].origAbsOffset;
        if (off != 0xFFFFFFFFu) jumpTargetOffsets.insert(off);
      }
    }
  }
  auto isProtected = [&](int ti, int idx, const CapcomCmdIR &c) {
    if (jumpTargets.count({ti, idx})) return true;
    return c.origAbsOffset != 0xFFFFFFFFu && jumpTargetOffsets.count(c.origAbsOffset) > 0;
  };
  // Zero-duration state setters that may sit between the two program
  // changes without letting the dead value reach a voice.
  auto isTransparent = [](CapcomCmdType t) {
    switch (t) {
      case CapcomCmdType::Octave:
      case CapcomCmdType::Volume:
      case CapcomCmdType::Duration:
      case CapcomCmdType::Pan:
      case CapcomCmdType::Transpose:
      case CapcomCmdType::Tuning:
        return true;
      default:
        return false;
    }
  };

  // Whether the control flow leaves this command linearly (what follows is
  // reached only via a landing, if at all).
  auto isFlowBreak = [](CapcomCmdType t) {
    return t == CapcomCmdType::Goto || t == CapcomCmdType::RepeatUntil ||
           t == CapcomCmdType::RepeatBreak || t == CapcomCmdType::End;
  };

  int dropped = 0;
  for (int ti : trackIndices) {
    auto *trk = m_ir->track(ti);
    if (!trk) continue;
    // Program the voice is known to hold at this point in the stream, or -1
    // when it could have arrived from anywhere (track start, a landing, a
    // command we don't model).
    int known = -1;
    for (int i = 0; i < static_cast<int>(trk->cmds.size());) {
      const auto &cur = trk->cmds[static_cast<size_t>(i)];
      const bool landing = isProtected(ti, i, cur);
      if (landing) known = -1;
      if (cur.type != CapcomCmdType::ProgramChange) {
        if (isFlowBreak(cur.type) || cur.type == CapcomCmdType::Unknown) known = -1;
        ++i;
        continue;
      }
      // parseFromSeq leaves cmd.program unset; the operand byte is the truth
      const int value = cur.params.empty() ? cur.program : cur.params[0];
      bool redundant = false;
      if (!landing) {
        if (known == value) {
          // Same-value re-set: corpus render tests put this in the same class
          // as a dead store (~-25 dB spectral distance, i.e. the driver's
          // sub-sample timing jitter) versus +74 dB for a real change.
          redundant = true;
        } else {
          // Dead store: overwritten before anything timed (note/rest) runs,
          // so the value never reaches a voice. (cmd.tick is only populated
          // for inserted commands, so time is judged by command type.)
          for (size_t j = static_cast<size_t>(i) + 1; j < trk->cmds.size(); ++j) {
            const auto &nxt = trk->cmds[j];
            if (nxt.type == CapcomCmdType::ProgramChange) { redundant = true; break; }
            if (!isTransparent(nxt.type)) break;
          }
        }
      }
      if (!redundant || !m_ir->removeCommand(ti, i)) {
        known = value;
        ++i;
        continue;
      }
      ++dropped;
      std::set<std::pair<int, int>> remapped;
      for (const auto &t : jumpTargets) {
        if (t.first == ti && t.second > i) remapped.insert({t.first, t.second - 1});
        else remapped.insert(t);
      }
      jumpTargets.swap(remapped);
      // do not advance: the next command slid into index i
    }
  }
  return dropped;
}

bool CapcomPianoRollModel::optimizeSequence(uint32_t *bytesBefore,
                                             uint32_t *bytesAfter,
                                             std::string *error) {
  if (bytesBefore) *bytesBefore = 0;
  if (bytesAfter) *bytesAfter = 0;
  if (!m_raw || !m_raw->isWritable()) {
    if (error) *error = "Backing file is not writable.";
    return false;
  }
  uint32_t before = 0, budget = 0;
  byteUsage(&before, &budget);
  if (bytesBefore) *bytesBefore = before;
  if (bytesAfter) *bytesAfter = before;

  if (!m_ir) {
    m_ir = std::make_unique<CapcomSeqIR>();
    m_ir->setMinAllocation(m_allocationFloor);
    if (!m_ir->parseFromSeq(m_seq, m_raw)) {
      if (error) *error = "Failed to parse sequence IR.";
      return false;
    }
  }

  // Any command a jump lands on must survive untouched, and re-entry points
  // invalidate dead-store reasoning across them.
  std::set<std::pair<int, int>> jumpTargets;
  std::set<uint32_t> jumpTargetOffsets;  // cross-track landings, by home offset
  for (int ti = 0; ti < 8; ++ti) {
    const auto *trk = m_ir->track(ti);
    if (!trk) continue;
    for (const auto &c : trk->cmds) {
      if (c.destCmdIndex < 0) continue;
      jumpTargets.insert({c.destTrackIndex, c.destCmdIndex});
      const auto *dtrk = m_ir->track(c.destTrackIndex);
      if (dtrk && c.destCmdIndex < static_cast<int>(dtrk->cmds.size())) {
        const uint32_t off = dtrk->cmds[static_cast<size_t>(c.destCmdIndex)].origAbsOffset;
        if (off != 0xFFFFFFFFu) jumpTargetOffsets.insert(off);
      }
    }
  }
  auto isProtected = [&](int ti, int idx, const CapcomCmdIR &c) {
    if (jumpTargets.count({ti, idx})) return true;
    return c.origAbsOffset != 0xFFFFFFFFu && jumpTargetOffsets.count(c.origAbsOffset) > 0;
  };

  auto isSimpleSetting = [](uint8_t st) {
    // plain state setters our own editor emits; conservative subset
    return st == 0x06 || st == 0x07 || st == 0x08 || st == 0x09 ||
           st == 0x18 || st == 0x19;
  };

  int removedCount = 0;
  for (int ti = 0; ti < 8; ++ti) {
    auto *trk = m_ir->track(ti);
    if (!trk) continue;

    // Foreign clones (commands whose home bytes live in another track's
    // region - Capcom's shared-melody trick) are skipped by the serializer,
    // so touching them here would report progress with zero real gain and
    // trigger a pointless rewrite. Bound all passes to the own-home prefix.
    size_t streamEnd = trk->cmds.size();
    for (size_t i = 0; i < trk->cmds.size(); ++i) {
      const auto &c = trk->cmds[i];
      if (c.origAbsOffset == 0xFFFFFFFFu) continue;
      int home = -1; uint32_t bestStart = 0;
      for (int tj = 0; tj < 8; ++tj) {
        const auto *other = m_ir->track(tj);
        if (!other) continue;
        if (other->origTrackStartAbs <= c.origAbsOffset &&
            (home < 0 || other->origTrackStartAbs > bestStart)) {
          home = tj; bestStart = other->origTrackStartAbs;
        }
      }
      if (home >= 0 && home != ti) { streamEnd = i; break; }
    }

    // Pass 1: adjacent byte-identical settings only. Corpus testing showed
    // the driver has audible side effects even when re-setting an unchanged
    // value across notes (a same-value volume re-set in "Goofy or Max"
    // changed the render), so no look-through reasoning is safe. Two
    // literally consecutive identical setters are the one provable no-op.
    for (int i = static_cast<int>(streamEnd) - 2; i >= 0; --i) {
      if (i + 1 >= static_cast<int>(streamEnd)) continue;
      const auto &cur = trk->cmds[static_cast<size_t>(i)];
      const auto &nxt = trk->cmds[static_cast<size_t>(i + 1)];
      if (!isSimpleSetting(cur.statusByte)) continue;
      if (isProtected(ti, i, cur) || isProtected(ti, i + 1, nxt)) continue;
      const bool identical = nxt.statusByte == cur.statusByte && nxt.params == cur.params;
      if (identical && m_ir->removeCommand(ti, i)) {
        L_WARN("OPT dedupe: track {} idx {} status 0x{:02x} orig 0x{:x}", ti, i, cur.statusByte, cur.origAbsOffset);
        --streamEnd;
        ++removedCount;
        // indices in jumpTargets after i shift down within this track
        std::set<std::pair<int, int>> remapped;
        for (const auto &t : jumpTargets) {
          if (t.first == ti && t.second > i) remapped.insert({t.first, t.second - 1});
          else remapped.insert(t);
        }
        jumpTargets.swap(remapped);
      }
    }

    // Pass 2: merge adjacent rests. Dotted/triplet toggles change effective
    // durations mode-dependently, so skip tracks that use them at all.
    bool modal = false;
    for (const auto &c : trk->cmds) {
      if (c.type == CapcomCmdType::DottedNoteOn || c.type == CapcomCmdType::ToggleTriplet) {
        modal = true;
        break;
      }
    }
    if (modal) continue;

    // Pass 3: a track with no notes and no commands after tick 0 has fully
    // unobservable timing - its rests only pad silence - so drop them all.
    // (Settings mid-track would fire at rest-dependent times; tempo affects
    // the whole song, so any command at tick > 0 keeps the rests.)
    {
      bool hasNote = false, hasLateCmd = false;
      for (size_t i = 0; i < streamEnd; ++i) {
        const auto &c = trk->cmds[i];
        if (c.type == CapcomCmdType::Note) { hasNote = true; break; }
        if (c.tick > 0 && c.type != CapcomCmdType::Rest && c.type != CapcomCmdType::End) {
          hasLateCmd = true;
        }
      }
      if (!hasNote && !hasLateCmd) {
        for (int i = static_cast<int>(streamEnd) - 1; i >= 0; --i) {
          if (trk->cmds[static_cast<size_t>(i)].type != CapcomCmdType::Rest) continue;
          if (isProtected(ti, i, trk->cmds[static_cast<size_t>(i)])) continue;
          if (m_ir->removeCommand(ti, i)) {
            L_WARN("OPT restcollapse: track {} idx {}", ti, i);
            --streamEnd;
            ++removedCount;
            std::set<std::pair<int, int>> remapped;
            for (const auto &t : jumpTargets) {
              if (t.first == ti && t.second > i) remapped.insert({t.first, t.second - 1});
              else remapped.insert(t);
            }
            jumpTargets.swap(remapped);
          }
        }
      }
    }

    bool merged = true;
    while (merged) {
      merged = false;
      for (int i = 0; i + 1 < static_cast<int>(streamEnd); ++i) {
        auto &a = trk->cmds[static_cast<size_t>(i)];
        const auto &b = trk->cmds[static_cast<size_t>(i + 1)];
        if (a.type != CapcomCmdType::Rest || b.type != CapcomCmdType::Rest) continue;
        if (isProtected(ti, i, a) || isProtected(ti, i + 1, b)) continue;  // landing points
        const uint32_t lenA = 192u >> (7 - a.lenIndex);
        const uint32_t lenB = 192u >> (7 - b.lenIndex);
        const uint32_t sum = lenA + lenB;
        // representable as a single rest only if it is a power-of-two length
        int k = -1;
        for (int li = 1; li <= 7; ++li) {
          if ((192u >> (7 - li)) == sum) { k = li; break; }
        }
        if (k < 0) continue;
        a.lenIndex = static_cast<uint8_t>(k);
        a.statusByte = static_cast<uint8_t>(k << 5);
        if (!m_ir->removeCommand(ti, i + 1)) continue;
        L_WARN("OPT restmerge: track {} idx {} orig 0x{:x}", ti, i + 1, b.origAbsOffset);
        --streamEnd;
        ++removedCount;
        std::set<std::pair<int, int>> remapped;
        for (const auto &t : jumpTargets) {
          if (t.first == ti && t.second > i + 1) remapped.insert({t.first, t.second - 1});
          else remapped.insert(t);
        }
        jumpTargets.swap(remapped);
        merged = true;
        break;
      }
    }
  }

  if (removedCount == 0) {
    return true;  // nothing to do - not an error
  }

  const auto undoSnap = captureRawSnapshot();
  std::string serErr;
  if (!m_ir->serializeToRaw(m_raw, &serErr)) {
    // Songs with overlapped/shared track data can grow on reserialization;
    // raw was not touched, so just drop the IR edits and report "no change".
    m_ir.reset();
    if (serErr.find("bytes are allocated") != std::string::npos) {
      return true;
    }
    if (error) *error = serErr;
    return false;
  }
  if (!SyncSeqTrackOffsetsFromHeader(m_seq, m_raw)) {
    L_WARN("optimizeSequence: failed to sync track offsets from header");
  }
  m_redo.clear();
  pushRawDiffUndo(undoSnap);
  reload();

  uint32_t after = 0;
  byteUsage(&after, &budget);
  if (bytesAfter) *bytesAfter = after;
  return true;
}

CapcomPianoRollModel::EditTransaction::EditTransaction(CapcomPianoRollModel &model)
    : m_model(model), m_raw(model.m_raw), m_before(model.captureRawSnapshot()),
      m_undo(model.m_undo), m_redo(model.m_redo), m_recentEdits(s_recentEdits),
      m_allocationFloor(model.m_allocationFloor) {
}

CapcomPianoRollModel::EditTransaction::~EditTransaction() {
  if (m_active) {
    std::string error;
    if (!rollback(&error)) {
      L_ERROR("Edit transaction rollback failed: {}", error);
    }
  }
}

bool CapcomPianoRollModel::EditTransaction::commit(std::string *error) {
  if (!m_active || !m_raw || m_model.m_raw != m_raw || m_raw->size() != m_before.size()) {
    if (error) {
      *error = "Cannot commit transaction: backing data changed.";
    }
    return false;
  }
  const bool changed = m_model.captureRawSnapshot() != m_before;
  // Primitive edits may have filled or truncated the bounded history. Restore
  // the complete checkpoint before adding exactly one entry for the net edit.
  m_model.m_undo = m_undo;
  m_model.m_redo = m_redo;
  s_recentEdits = m_recentEdits;
  if (changed) {
    m_model.m_redo.clear();
    m_model.pushRawDiffUndo(m_before);
    for (const auto &entry : m_model.m_undo.back()) {
      recordEdit(entry.raw, entry.offset);
    }
  }
  m_model.m_ir.reset();
  m_active = false;
  return true;
}

bool CapcomPianoRollModel::EditTransaction::rollback(std::string *error) {
  if (!m_active) {
    return true;
  }
  if (!m_raw || m_model.m_raw != m_raw || !m_raw->isWritable() || m_raw->size() != m_before.size()) {
    if (error) {
      *error = "Cannot roll back transaction: backing data changed or is read-only.";
    }
    return false;
  }
  for (size_t i = 0; i < m_before.size(); ++i) {
    if (m_raw->readByte(i) != m_before[i] && !m_raw->writeByte(i, m_before[i])) {
      if (error) {
        *error = "Failed to restore transaction bytes.";
      }
      return false;
    }
  }
  m_model.m_allocationFloor = m_allocationFloor;
  m_model.m_undo = m_undo;
  m_model.m_redo = m_redo;
  s_recentEdits = m_recentEdits;
  SyncSeqTrackOffsetsFromHeader(m_model.m_seq, m_raw);
  m_model.reload();
  m_active = false;
  return true;
}

bool CapcomPianoRollModel::undo(std::string *error) {
  if (m_undo.empty()) {
    return false;
  }
  auto batch = std::move(m_undo.back());
  m_undo.pop_back();
  std::vector<EditEntry> redoBatch;
  redoBatch.reserve(batch.size());
  for (const auto &entry : batch) {
    if (!entry.raw || !entry.raw->isWritable()) {
      if (error) {
        *error = "Backing file is not writable.";
      }
      return false;
    }
    redoBatch.push_back(EditEntry{entry.raw, entry.offset, entry.oldVal, entry.newVal});
    if (!entry.raw->writeByte(entry.offset, entry.oldVal)) {
      if (error) {
        *error = "Failed to write during undo.";
      }
      return false;
    }
  }
  m_redo.push_back(std::move(redoBatch));

  SyncSeqTrackOffsetsFromHeader(m_seq, m_raw);
  reload();
  return true;
}

bool CapcomPianoRollModel::redo(std::string *error) {
  if (m_redo.empty()) {
    return false;
  }
  auto batch = std::move(m_redo.back());
  m_redo.pop_back();
  std::vector<EditEntry> undoBatch;
  undoBatch.reserve(batch.size());
  for (const auto &entry : batch) {
    if (!entry.raw || !entry.raw->isWritable()) {
      if (error) {
        *error = "Backing file is not writable.";
      }
      return false;
    }
    undoBatch.push_back(EditEntry{entry.raw, entry.offset, entry.oldVal, entry.newVal});
    if (!entry.raw->writeByte(entry.offset, entry.newVal)) {
      if (error) {
        *error = "Failed to write during redo.";
      }
      return false;
    }
  }
  m_undo.push_back(std::move(undoBatch));

  SyncSeqTrackOffsetsFromHeader(m_seq, m_raw);
  reload();
  return true;
}

void CapcomPianoRollModel::pushUndoBatch(std::vector<EditEntry> batch) {
  if (batch.empty()) {
    return;
  }
  constexpr size_t kMax = 64;
  m_undo.push_back(std::move(batch));
  if (m_undo.size() > kMax) {
    m_undo.erase(m_undo.begin());
  }
}

std::vector<uint8_t> CapcomPianoRollModel::captureRawSnapshot() const {
  std::vector<uint8_t> snap;
  if (!m_raw) {
    return snap;
  }
  const size_t n = m_raw->size();
  snap.resize(n);
  for (size_t i = 0; i < n; ++i) {
    snap[i] = m_raw->readByte(i);
  }
  return snap;
}

void CapcomPianoRollModel::pushRawDiffUndo(const std::vector<uint8_t> &before) {
  if (!m_raw || before.empty()) {
    return;
  }
  const size_t n = std::min(before.size(), m_raw->size());
  std::vector<EditEntry> batch;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t cur = m_raw->readByte(i);
    if (cur != before[i]) {
      batch.push_back(EditEntry{m_raw, static_cast<uint32_t>(i), before[i], cur});
    }
  }
  pushUndoBatch(std::move(batch));
}

bool CapcomPianoRollModel::flush(std::string *error) {
  if (!m_writableRaw || !m_writableRaw->isWritable()) {
    if (error) {
      *error = "Raw file is read-only; cannot save edits.";
    }
    return false;
  }
  if (!m_writableRaw->flush()) {
    if (error) {
      *error = "Failed to flush data to disk.";
    }
    return false;
  }
  return true;
}

void CapcomPianoRollModel::recordEdit(RawFile *raw, uint32_t offset) {
  if (!raw) {
    return;
  }
  // keep small list of recent edited offsets for playback verification
  constexpr size_t kMax = 16;
  s_recentEdits.emplace_back(raw, offset);
  if (s_recentEdits.size() > kMax) {
    s_recentEdits.erase(s_recentEdits.begin());
  }
}
