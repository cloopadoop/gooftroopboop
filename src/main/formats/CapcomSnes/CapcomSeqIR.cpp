#include "CapcomSeqIR.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <string_view>

#include "CapcomTrackTraversal.h"
#include "RawFile.h"
#include "formats/CapcomSnes/CapcomSnesSeq.h"
#include "LogManager.h"
#include "nlohmann/json.hpp"

namespace {
constexpr int kMaxParseEvents = 20000;
constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

const char *cmdTypeName(CapcomCmdType type) {
  switch (type) {
    case CapcomCmdType::Note:
      return "Note";
    case CapcomCmdType::Rest:
      return "Rest";
    case CapcomCmdType::ToggleTriplet:
      return "ToggleTriplet";
    case CapcomCmdType::ToggleSlur:
      return "ToggleSlur";
    case CapcomCmdType::DottedNoteOn:
      return "DottedNoteOn";
    case CapcomCmdType::ToggleOctaveUp:
      return "ToggleOctaveUp";
    case CapcomCmdType::NoteAttributes:
      return "NoteAttributes";
    case CapcomCmdType::Tempo:
      return "Tempo";
    case CapcomCmdType::Duration:
      return "Duration";
    case CapcomCmdType::Volume:
      return "Volume";
    case CapcomCmdType::Pan:
      return "Pan";
    case CapcomCmdType::MasterVolume:
      return "MasterVolume";
    case CapcomCmdType::ProgramChange:
      return "ProgramChange";
    case CapcomCmdType::Octave:
      return "Octave";
    case CapcomCmdType::GlobalTranspose:
      return "GlobalTranspose";
    case CapcomCmdType::Transpose:
      return "Transpose";
    case CapcomCmdType::Tuning:
      return "Tuning";
    case CapcomCmdType::PortamentoTime:
      return "PortamentoTime";
    case CapcomCmdType::RepeatUntil:
      return "RepeatUntil";
    case CapcomCmdType::RepeatBreak:
      return "RepeatBreak";
    case CapcomCmdType::Goto:
      return "Goto";
    case CapcomCmdType::End:
      return "End";
    case CapcomCmdType::LFO:
      return "LFO";
    case CapcomCmdType::EchoParam:
      return "EchoParam";
    case CapcomCmdType::EchoOnOff:
      return "EchoOnOff";
    case CapcomCmdType::ReleaseRate:
      return "ReleaseRate";
    case CapcomCmdType::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

bool parseCmdType(const std::string &name, CapcomCmdType *out) {
  if (!out) {
    return false;
  }
  const std::string_view typeName(name);
  if (typeName == "Note") {
    *out = CapcomCmdType::Note;
  } else if (typeName == "Rest") {
    *out = CapcomCmdType::Rest;
  } else if (typeName == "ToggleTriplet") {
    *out = CapcomCmdType::ToggleTriplet;
  } else if (typeName == "ToggleSlur") {
    *out = CapcomCmdType::ToggleSlur;
  } else if (typeName == "DottedNoteOn") {
    *out = CapcomCmdType::DottedNoteOn;
  } else if (typeName == "ToggleOctaveUp") {
    *out = CapcomCmdType::ToggleOctaveUp;
  } else if (typeName == "NoteAttributes") {
    *out = CapcomCmdType::NoteAttributes;
  } else if (typeName == "Tempo") {
    *out = CapcomCmdType::Tempo;
  } else if (typeName == "Duration") {
    *out = CapcomCmdType::Duration;
  } else if (typeName == "Volume") {
    *out = CapcomCmdType::Volume;
  } else if (typeName == "Pan") {
    *out = CapcomCmdType::Pan;
  } else if (typeName == "MasterVolume") {
    *out = CapcomCmdType::MasterVolume;
  } else if (typeName == "ProgramChange") {
    *out = CapcomCmdType::ProgramChange;
  } else if (typeName == "Octave") {
    *out = CapcomCmdType::Octave;
  } else if (typeName == "GlobalTranspose") {
    *out = CapcomCmdType::GlobalTranspose;
  } else if (typeName == "Transpose") {
    *out = CapcomCmdType::Transpose;
  } else if (typeName == "Tuning") {
    *out = CapcomCmdType::Tuning;
  } else if (typeName == "PortamentoTime") {
    *out = CapcomCmdType::PortamentoTime;
  } else if (typeName == "RepeatUntil") {
    *out = CapcomCmdType::RepeatUntil;
  } else if (typeName == "RepeatBreak") {
    *out = CapcomCmdType::RepeatBreak;
  } else if (typeName == "Goto") {
    *out = CapcomCmdType::Goto;
  } else if (typeName == "End") {
    *out = CapcomCmdType::End;
  } else if (typeName == "LFO") {
    *out = CapcomCmdType::LFO;
  } else if (typeName == "EchoParam") {
    *out = CapcomCmdType::EchoParam;
  } else if (typeName == "EchoOnOff") {
    *out = CapcomCmdType::EchoOnOff;
  } else if (typeName == "ReleaseRate") {
    *out = CapcomCmdType::ReleaseRate;
  } else if (typeName == "Unknown") {
    *out = CapcomCmdType::Unknown;
  } else {
    return false;
  }
  return true;
}

bool readUint8(const nlohmann::json &obj, const char *key, uint8_t *out) {
  if (!out) {
    return false;
  }
  auto it = obj.find(key);
  if (it == obj.end()) {
    return false;
  }
  if (!it->is_number_integer()) {
    return false;
  }
  const int value = it->get<int>();
  if (value < 0 || value > 255) {
    return false;
  }
  *out = static_cast<uint8_t>(value);
  return true;
}

bool readInt(const nlohmann::json &obj, const char *key, int *out) {
  if (!out) {
    return false;
  }
  auto it = obj.find(key);
  if (it == obj.end()) {
    return false;
  }
  if (!it->is_number_integer()) {
    return false;
  }
  *out = it->get<int>();
  return true;
}

bool readUint32(const nlohmann::json &obj, const char *key, uint32_t *out) {
  if (!out) {
    return false;
  }
  auto it = obj.find(key);
  if (it == obj.end()) {
    return false;
  }
  if (!it->is_number_integer()) {
    return false;
  }
  const int64_t value = it->get<int64_t>();
  if (value < 0 || value > 0xFFFFFFFFll) {
    return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

// Append the byte encoding of one command. For control-flow commands the
// destination word is either written from cmd.destWord (writeDestWords, used
// by the layout-preserving path) or left as a 00 00 placeholder with its
// position reported through ptrPos (compacting path patches it later).
bool encodeCmdBytes(const CapcomCmdIR &cmd, std::vector<uint8_t> &out, bool writeDestWords,
                    size_t *ptrPos, std::string *error) {
  auto failParams = [&](size_t expected) {
    if (error) {
      std::ostringstream oss;
      oss << "Invalid parameter count for cmd type " << static_cast<int>(cmd.type) << ": expected "
          << expected << ", got " << cmd.params.size();
      *error = oss.str();
    }
    return false;
  };
  auto pushDest = [&]() {
    if (ptrPos) {
      *ptrPos = out.size();
    }
    if (writeDestWords) {
      out.push_back(static_cast<uint8_t>((cmd.destWord >> 8) & 0xFF));
      out.push_back(static_cast<uint8_t>(cmd.destWord & 0xFF));
    } else {
      out.push_back(0x00);
      out.push_back(0x00);
    }
  };

  switch (cmd.type) {
    case CapcomCmdType::Note:
    case CapcomCmdType::Rest:
      // lenIndex must be 1..7: a Note/Rest byte with lenIndex 0 lands in the
      // command range (0x00-0x1F) and would be re-parsed as an opcode. Refuse
      // rather than corrupt the stream.
      if (cmd.lenIndex == 0 || cmd.lenIndex > 7) {
        if (error) {
          *error = "Invalid note length index " + std::to_string(cmd.lenIndex) +
                   " (must be 1-7)";
        }
        return false;
      }
      out.push_back(static_cast<uint8_t>((cmd.lenIndex << 5) | (cmd.keyIndex & 0x1F)));
      return true;
    case CapcomCmdType::ToggleTriplet:
    case CapcomCmdType::ToggleSlur:
    case CapcomCmdType::DottedNoteOn:
    case CapcomCmdType::ToggleOctaveUp:
    case CapcomCmdType::End:
      out.push_back(cmd.statusByte);
      return true;
    case CapcomCmdType::NoteAttributes:
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
    case CapcomCmdType::EchoOnOff:
    case CapcomCmdType::ReleaseRate:
      if (cmd.params.size() != 1) {
        return failParams(1);
      }
      out.push_back(cmd.statusByte);
      out.insert(out.end(), cmd.params.begin(), cmd.params.end());
      return true;
    case CapcomCmdType::Tempo:
    case CapcomCmdType::LFO:
    case CapcomCmdType::EchoParam:
      if (cmd.params.size() != 2) {
        return failParams(2);
      }
      out.push_back(cmd.statusByte);
      out.insert(out.end(), cmd.params.begin(), cmd.params.end());
      return true;
    case CapcomCmdType::RepeatUntil:
      out.push_back(cmd.statusByte);
      out.push_back(cmd.repeatCount);
      pushDest();
      return true;
    case CapcomCmdType::RepeatBreak:
      out.push_back(cmd.statusByte);
      out.push_back(cmd.breakAttributes);
      pushDest();
      return true;
    case CapcomCmdType::Goto:
      out.push_back(cmd.statusByte);
      pushDest();
      return true;
    default:
      out.push_back(cmd.statusByte);
      out.insert(out.end(), cmd.params.begin(), cmd.params.end());
      return true;
  }
}
}  // namespace

bool CapcomSeqIR::parseFromSeq(CapcomSnesSeq *seq, RawFile *raw) {
  if (!seq || !raw) {
    return false;
  }
  return parseFromImage(raw, seq->dwOffset, seq->priorityInHeader);
}

bool CapcomSeqIR::parseFromImage(RawFile *raw, uint32_t seqBaseAbs, bool priorityInHeader) {
  if (!raw) {
    return false;
  }

  m_seqBaseAbs = seqBaseAbs;
  m_priorityInHeader = priorityInHeader;
  m_structuralChange = false;
  initEventMap();

  for (int i = 0; i < MAX_TRACKS; ++i) {
    m_tracks[i].reset();
  }

  uint32_t headerOffset = m_seqBaseAbs + (m_priorityInHeader ? 1 : 0);
  for (int i = 0; i < MAX_TRACKS; ++i) {
    uint16_t trkPtr = raw->readShortBE(headerOffset + static_cast<uint32_t>(i) * 2);
    if (trkPtr != 0) {
      if (!parseTrackIR(i, trkPtr, raw)) {
        L_WARN("Failed to parse track {} at offset 0x{:x}", i, trkPtr);
      }
    }
  }

  resolveDestinations();

  // Record the sequence's original byte footprint so serialization can refuse
  // to grow into neighboring ARAM data.
  uint32_t headerSize = (m_priorityInHeader ? 1u : 0u) + MAX_TRACKS * 2u;
  uint32_t maxEndAbs = m_seqBaseAbs + headerSize;
  for (int i = 0; i < MAX_TRACKS; ++i) {
    if (!m_tracks[i]) {
      continue;
    }
    for (const auto &cmd : m_tracks[i]->cmds) {
      const uint32_t end = cmd.origAbsOffset + cmd.sizeBytes;
      if (cmd.origAbsOffset >= m_seqBaseAbs && end > maxEndAbs) {
        maxEndAbs = end;
      }
    }
  }
  m_origAllocation = std::max(maxEndAbs - m_seqBaseAbs, m_minAllocation);

  return true;
}

void CapcomSeqIR::initEventMap() {
  m_eventMap.clear();
  m_eventMap[0x00] = CapcomCmdType::ToggleTriplet;
  m_eventMap[0x01] = CapcomCmdType::ToggleSlur;
  m_eventMap[0x02] = CapcomCmdType::DottedNoteOn;
  m_eventMap[0x03] = CapcomCmdType::ToggleOctaveUp;
  m_eventMap[0x04] = CapcomCmdType::NoteAttributes;
  m_eventMap[0x05] = CapcomCmdType::Tempo;
  m_eventMap[0x06] = CapcomCmdType::Duration;
  m_eventMap[0x07] = CapcomCmdType::Volume;
  m_eventMap[0x08] = CapcomCmdType::ProgramChange;
  m_eventMap[0x09] = CapcomCmdType::Octave;
  m_eventMap[0x0A] = CapcomCmdType::GlobalTranspose;
  m_eventMap[0x0B] = CapcomCmdType::Transpose;
  m_eventMap[0x0C] = CapcomCmdType::Tuning;
  m_eventMap[0x0D] = CapcomCmdType::PortamentoTime;
  m_eventMap[0x0E] = CapcomCmdType::RepeatUntil;
  m_eventMap[0x0F] = CapcomCmdType::RepeatUntil;
  m_eventMap[0x10] = CapcomCmdType::RepeatUntil;
  m_eventMap[0x11] = CapcomCmdType::RepeatUntil;
  m_eventMap[0x12] = CapcomCmdType::RepeatBreak;
  m_eventMap[0x13] = CapcomCmdType::RepeatBreak;
  m_eventMap[0x14] = CapcomCmdType::RepeatBreak;
  m_eventMap[0x15] = CapcomCmdType::RepeatBreak;
  m_eventMap[0x16] = CapcomCmdType::Goto;
  m_eventMap[0x17] = CapcomCmdType::End;
  m_eventMap[0x18] = CapcomCmdType::Pan;
  m_eventMap[0x19] = CapcomCmdType::MasterVolume;
  m_eventMap[0x1A] = CapcomCmdType::LFO;
  m_eventMap[0x1B] = CapcomCmdType::EchoParam;
  m_eventMap[0x1C] = CapcomCmdType::EchoOnOff;
  m_eventMap[0x1D] = CapcomCmdType::ReleaseRate;
}

bool CapcomSeqIR::toJson(nlohmann::json *out, std::string *error) const {
  if (!out) {
    if (error) {
      *error = "Invalid JSON output pointer.";
    }
    return false;
  }

  nlohmann::json json;
  json["format"] = "capcom_snes";
  json["seq_base"] = m_seqBaseAbs;
  json["priority_in_header"] = m_priorityInHeader;

  nlohmann::json tracksJson = nlohmann::json::array();
  for (int i = 0; i < MAX_TRACKS; ++i) {
    if (!m_tracks[i]) {
      continue;
    }
    const auto &trk = *m_tracks[i];
    nlohmann::json trackObj;
    trackObj["header_index"] = trk.trackIndex;
    trackObj["orig_start"] = trk.origTrackStartAbs;
    trackObj["orig_ptr"] = trk.origTrackPtrValue;

    nlohmann::json cmdArray = nlohmann::json::array();
    for (const auto &cmd : trk.cmds) {
      nlohmann::json cmdObj;
      cmdObj["type"] = cmdTypeName(cmd.type);
      cmdObj["status"] = cmd.statusByte;
      if (!cmd.params.empty()) {
        nlohmann::json paramsJson = nlohmann::json::array();
        for (uint8_t param : cmd.params) {
          paramsJson.push_back(param);
        }
        cmdObj["params"] = std::move(paramsJson);
      }
      if (cmd.type == CapcomCmdType::Note || cmd.type == CapcomCmdType::Rest) {
        cmdObj["key_index"] = cmd.keyIndex;
        cmdObj["len_index"] = cmd.lenIndex;
      }
      if (cmd.type == CapcomCmdType::RepeatUntil ||
          cmd.type == CapcomCmdType::RepeatBreak ||
          cmd.type == CapcomCmdType::Goto) {
        cmdObj["dest_word"] = cmd.destWord;
        if (cmd.destTrackIndex >= 0) {
          cmdObj["dest_track"] = cmd.destTrackIndex;
        }
        if (cmd.destCmdIndex >= 0) {
          cmdObj["dest_cmd"] = cmd.destCmdIndex;
        }
      }
      if (cmd.origAbsOffset != kInvalidOffset) {
        cmdObj["orig_offset"] = cmd.origAbsOffset;
      }
      cmdArray.push_back(std::move(cmdObj));
    }
    trackObj["commands"] = std::move(cmdArray);
    tracksJson.push_back(std::move(trackObj));
  }

  json["tracks"] = std::move(tracksJson);
  *out = std::move(json);
  return true;
}

bool CapcomSeqIR::loadFromJson(const nlohmann::json &json, std::string *error) {
  if (!json.is_object()) {
    if (error) {
      *error = "JSON root is not an object.";
    }
    return false;
  }

  uint32_t seqBase = 0;
  bool priority = false;
  if (!readUint32(json, "seq_base", &seqBase)) {
    if (error) {
      *error = "Missing seq_base in JSON.";
    }
    return false;
  }
  auto priorityIt = json.find("priority_in_header");
  if (priorityIt == json.end() || !priorityIt->is_boolean()) {
    if (error) {
      *error = "Missing priority_in_header in JSON.";
    }
    return false;
  }
  priority = priorityIt->get<bool>();

  auto tracksIt = json.find("tracks");
  if (tracksIt == json.end() || !tracksIt->is_array()) {
    if (error) {
      *error = "Missing tracks array in JSON.";
    }
    return false;
  }

  m_seqBaseAbs = seqBase;
  m_priorityInHeader = priority;
  // JSON-loaded IR may not correspond to the raw file's current layout.
  m_structuralChange = true;
  initEventMap();
  for (int i = 0; i < MAX_TRACKS; ++i) {
    m_tracks[i].reset();
  }

  for (const auto &trackJson : *tracksIt) {
    if (!trackJson.is_object()) {
      continue;
    }
    int headerIndex = -1;
    if (!readInt(trackJson, "header_index", &headerIndex)) {
      if (error) {
        *error = "Track missing header_index.";
      }
      return false;
    }
    if (headerIndex < 0 || headerIndex >= MAX_TRACKS) {
      if (error) {
        *error = "Track header_index out of range.";
      }
      return false;
    }

    CapcomTrackIR trk;
    trk.trackIndex = headerIndex;
    trk.origTrackStartAbs = trackJson.value("orig_start", 0u);
    trk.origTrackPtrValue = static_cast<uint16_t>(trackJson.value("orig_ptr", trk.origTrackStartAbs));

    auto cmdsIt = trackJson.find("commands");
    if (cmdsIt != trackJson.end() && cmdsIt->is_array()) {
      for (const auto &cmdJson : *cmdsIt) {
        if (!cmdJson.is_object()) {
          continue;
        }

        CapcomCmdIR cmd;
        cmd.origAbsOffset = kInvalidOffset;
        readUint32(cmdJson, "orig_offset", &cmd.origAbsOffset);

        std::string typeName = cmdJson.value("type", std::string("Unknown"));
        if (!parseCmdType(typeName, &cmd.type)) {
          cmd.type = CapcomCmdType::Unknown;
        }

        if (cmd.type == CapcomCmdType::Note || cmd.type == CapcomCmdType::Rest) {
          uint8_t keyIndex = 0;
          uint8_t lenIndex = 0;
          if (!readUint8(cmdJson, "key_index", &keyIndex) ||
              !readUint8(cmdJson, "len_index", &lenIndex)) {
            if (error) {
              *error = "Note command missing key_index or len_index.";
            }
            return false;
          }
          cmd.keyIndex = keyIndex;
          cmd.lenIndex = lenIndex;
          cmd.statusByte = static_cast<uint8_t>((lenIndex << 5) | (keyIndex & 0x1F));
          cmd.sizeBytes = 1;
        } else {
          uint8_t statusByte = 0;
          if (!readUint8(cmdJson, "status", &statusByte)) {
            switch (cmd.type) {
              case CapcomCmdType::ToggleTriplet:
                statusByte = 0x00;
                break;
              case CapcomCmdType::ToggleSlur:
                statusByte = 0x01;
                break;
              case CapcomCmdType::DottedNoteOn:
                statusByte = 0x02;
                break;
              case CapcomCmdType::ToggleOctaveUp:
                statusByte = 0x03;
                break;
              case CapcomCmdType::NoteAttributes:
                statusByte = 0x04;
                break;
              case CapcomCmdType::Tempo:
                statusByte = 0x05;
                break;
              case CapcomCmdType::Duration:
                statusByte = 0x06;
                break;
              case CapcomCmdType::Volume:
                statusByte = 0x07;
                break;
              case CapcomCmdType::ProgramChange:
                statusByte = 0x08;
                break;
              case CapcomCmdType::Octave:
                statusByte = 0x09;
                break;
              case CapcomCmdType::GlobalTranspose:
                statusByte = 0x0A;
                break;
              case CapcomCmdType::Transpose:
                statusByte = 0x0B;
                break;
              case CapcomCmdType::Tuning:
                statusByte = 0x0C;
                break;
              case CapcomCmdType::PortamentoTime:
                statusByte = 0x0D;
                break;
              case CapcomCmdType::Pan:
                statusByte = 0x18;
                break;
              case CapcomCmdType::MasterVolume:
                statusByte = 0x19;
                break;
              case CapcomCmdType::LFO:
                statusByte = 0x1A;
                break;
              case CapcomCmdType::EchoParam:
                statusByte = 0x1B;
                break;
              case CapcomCmdType::EchoOnOff:
                statusByte = 0x1C;
                break;
              case CapcomCmdType::ReleaseRate:
                statusByte = 0x1D;
                break;
              case CapcomCmdType::End:
                statusByte = 0x17;
                break;
              default:
                if (error) {
                  *error = "Command missing status byte.";
                }
                return false;
            }
          }
          cmd.statusByte = statusByte;

          auto paramsIt = cmdJson.find("params");
          if (paramsIt != cmdJson.end() && paramsIt->is_array()) {
            for (const auto &param : *paramsIt) {
              if (!param.is_number_integer()) {
                if (error) {
                  *error = "Command params must be integers.";
                }
                return false;
              }
              const int value = param.get<int>();
              if (value < 0 || value > 255) {
                if (error) {
                  *error = "Command param out of range.";
                }
                return false;
              }
              cmd.params.push_back(static_cast<uint8_t>(value));
            }
          }
          cmd.sizeBytes = static_cast<uint8_t>(1 + cmd.params.size());
        }

        if (cmd.type == CapcomCmdType::ProgramChange && !cmd.params.empty()) {
          cmd.program = cmd.params[0];
        }
        if (cmd.type == CapcomCmdType::RepeatUntil ||
            cmd.type == CapcomCmdType::RepeatBreak ||
            cmd.type == CapcomCmdType::Goto) {
          uint32_t destWord = 0;
          if (readUint32(cmdJson, "dest_word", &destWord)) {
            cmd.destWord = static_cast<uint16_t>(destWord & 0xFFFFu);
          }
          readInt(cmdJson, "dest_track", &cmd.destTrackIndex);
          readInt(cmdJson, "dest_cmd", &cmd.destCmdIndex);
          if (cmd.type == CapcomCmdType::RepeatUntil || cmd.type == CapcomCmdType::RepeatBreak) {
            if (cmd.statusByte >= 0x0E && cmd.statusByte <= 0x11) {
              cmd.repeatSlot = static_cast<uint8_t>(cmd.statusByte - 0x0E);
            } else if (cmd.statusByte >= 0x12 && cmd.statusByte <= 0x15) {
              cmd.repeatSlot = static_cast<uint8_t>(cmd.statusByte - 0x12);
            }
          }
          if (!cmd.params.empty()) {
            cmd.repeatCount = cmd.params[0];
          }
          if (cmd.params.size() >= 3) {
            cmd.destWord = static_cast<uint16_t>((cmd.params[1] << 8) | cmd.params[2]);
          } else if (cmd.params.size() >= 2 && cmd.type == CapcomCmdType::Goto) {
            cmd.destWord = static_cast<uint16_t>((cmd.params[0] << 8) | cmd.params[1]);
          }
        }

        trk.cmds.push_back(std::move(cmd));
      }
    }

    m_tracks[headerIndex] = std::move(trk);
  }

  resolveDestinations();
  return true;
}

bool CapcomSeqIR::parseTrackIR(int trackIndex, uint32_t trackStart, RawFile *raw) {
  CapcomTrackIR trk;
  trk.trackIndex = trackIndex;
  trk.origTrackStartAbs = trackStart;
  trk.origTrackPtrValue = static_cast<uint16_t>(trackStart);

  CapcomTrackTraversalResult traversal;
  if (!CapcomTrackTraversal::Traverse(raw, trackStart, &traversal)) {
    return false;
  }

  std::unordered_map<uint32_t, CapcomCmdIR> cmdsByOffset;
  cmdsByOffset.reserve(traversal.steps.size());
  for (const auto &step : traversal.steps) {
    const auto &cmd = step.cmd;
    cmdsByOffset.emplace(cmd.origAbsOffset, cmd);
  }

  std::vector<uint32_t> offsets;
  offsets.reserve(cmdsByOffset.size());
  for (const auto &entry : cmdsByOffset) {
    offsets.push_back(entry.first);
  }

  constexpr uint32_t kAddrMask = 0xFFFFu;
  std::sort(offsets.begin(), offsets.end(),
            [trackStart](uint32_t a, uint32_t b) {
              const uint32_t distA = (a - trackStart) & kAddrMask;
              const uint32_t distB = (b - trackStart) & kAddrMask;
              if (distA != distB) {
                return distA < distB;
              }
              return a < b;
            });

  trk.cmds.reserve(offsets.size());
  for (uint32_t offset : offsets) {
    trk.cmds.push_back(cmdsByOffset[offset]);
  }

  m_tracks[trackIndex] = std::move(trk);
  return true;
}

void CapcomSeqIR::resolveDestinations() {
  for (int ti = 0; ti < MAX_TRACKS; ++ti) {
    if (!m_tracks[ti]) continue;
    auto &trk = *m_tracks[ti];

    std::unordered_map<uint32_t, int> offsetToIndex;
    for (int i = 0; i < static_cast<int>(trk.cmds.size()); ++i) {
      offsetToIndex[trk.cmds[i].origAbsOffset] = i;
    }

    for (auto &cmd : trk.cmds) {
      if (cmd.type == CapcomCmdType::RepeatUntil ||
          cmd.type == CapcomCmdType::RepeatBreak ||
          cmd.type == CapcomCmdType::Goto) {
        auto it = offsetToIndex.find(cmd.destWord);
        if (it != offsetToIndex.end()) {
          cmd.destTrackIndex = ti;
          cmd.destCmdIndex = it->second;
        }
      }
    }
  }
}

bool CapcomSeqIR::serializeToBuffer(RawFile *raw, std::vector<uint8_t> *out, std::string *error) {
  if (!raw || !out) {
    if (error) {
      *error = "Invalid arguments.";
    }
    return false;
  }

  std::vector<uint8_t> &newData = *out;
  newData.clear();

  // Layout-preserving path: when no structural edit happened, every command
  // still lives at its parse-time offset with its parse-time size, so we copy
  // the original region and re-encode each command in place. Unreferenced gap
  // bytes (dead data between reachable commands) survive untouched, which is
  // what makes unedited round-trips byte-identical.
  bool canPreserveLayout = !m_structuralChange && m_origAllocation > 0;
  for (int ti = 0; canPreserveLayout && ti < MAX_TRACKS; ++ti) {
    if (!m_tracks[ti]) {
      continue;
    }
    for (const auto &cmd : m_tracks[ti]->cmds) {
      if (cmd.origAbsOffset == kInvalidOffset || cmd.origAbsOffset < m_seqBaseAbs ||
          cmd.origAbsOffset + cmd.sizeBytes > m_seqBaseAbs + m_origAllocation) {
        canPreserveLayout = false;
        break;
      }
    }
  }

  if (canPreserveLayout) {
    newData.resize(m_origAllocation);
    for (uint32_t i = 0; i < m_origAllocation; ++i) {
      newData[i] = raw->readByte(m_seqBaseAbs + i);
    }

    for (int ti = 0; ti < MAX_TRACKS; ++ti) {
      if (!m_tracks[ti]) {
        continue;
      }
      for (const auto &cmd : m_tracks[ti]->cmds) {
        std::vector<uint8_t> enc;
        if (!encodeCmdBytes(cmd, enc, /*writeDestWords=*/true, nullptr, error)) {
          return false;
        }
        if (enc.size() != cmd.sizeBytes) {
          if (error) {
            std::ostringstream oss;
            oss << "Command at 0x" << std::hex << cmd.origAbsOffset << " re-encodes to " << std::dec
                << enc.size() << " bytes but occupies " << static_cast<int>(cmd.sizeBytes)
                << "; internal size drift";
            *error = oss.str();
          }
          return false;
        }
        std::copy(enc.begin(), enc.end(), newData.begin() + (cmd.origAbsOffset - m_seqBaseAbs));
      }
    }
    return true;
  }

  uint32_t headerSize = (m_priorityInHeader ? 1u : 0u) + MAX_TRACKS * 2u;
  newData.resize(headerSize, 0);

  if (m_priorityInHeader) {
    newData[0] = raw->readByte(m_seqBaseAbs);
  }

  std::array<uint32_t, MAX_TRACKS> newTrackStarts{};
  std::array<std::vector<uint32_t>, MAX_TRACKS> newCmdOffsets;
  std::vector<CapcomPointerPatch> patches;

  auto setError = [&error](const std::string &message) {
    if (error) {
      *error = message;
    }
  };

  auto validateU16Offset = [&setError](const char *what, uint32_t absOffset) {
    if (absOffset > 0xFFFFu) {
      std::ostringstream oss;
      oss << what << " offset out of range for 16-bit pointer: 0x" << std::hex << absOffset;
      setError(oss.str());
      return false;
    }
    return true;
  };

  // Capcom shares melodies by jumping (Goto or RepeatBreak) into another
  // channel's data; the traversal clones those foreign commands into each
  // visiting track. A command's home is the track whose original byte region
  // contains it - emit it only there, and resolve jump targets into the
  // emitted home bytes.
  std::array<uint32_t, MAX_TRACKS> homeStart{};
  for (int ti = 0; ti < MAX_TRACKS; ++ti) {
    homeStart[ti] = m_tracks[ti] ? m_tracks[ti]->origTrackStartAbs : 0xFFFFFFFFu;
  }
  auto homeTrack = [&](uint32_t off) -> int {
    int best = -1;
    uint32_t bestStart = 0;
    for (int ti = 0; ti < MAX_TRACKS; ++ti) {
      if (homeStart[ti] == 0xFFFFFFFFu) continue;
      if (homeStart[ti] <= off && (best < 0 || homeStart[ti] > bestStart)) {
        best = ti;
        bestStart = homeStart[ti];
      }
    }
    return best;
  };
  std::unordered_map<uint32_t, uint32_t> emittedByOrig;
  for (int ti = 0; ti < MAX_TRACKS; ++ti) {
    if (!m_tracks[ti]) {
      newTrackStarts[ti] = 0;
      continue;
    }

    auto &trk = *m_tracks[ti];
    newTrackStarts[ti] = m_seqBaseAbs + static_cast<uint32_t>(newData.size());
    if (!validateU16Offset("Track start", newTrackStarts[ti])) {
      return false;
    }
    newCmdOffsets[ti].assign(trk.cmds.size(), 0);

    for (size_t ci = 0; ci < trk.cmds.size(); ++ci) {
      const auto &cmd = trk.cmds[ci];
      if (cmd.origAbsOffset != kInvalidOffset && homeTrack(cmd.origAbsOffset) != ti) {
        continue;  // foreign clone: emitted by its home track, mapped later
      }
      newCmdOffsets[ti][ci] = m_seqBaseAbs + static_cast<uint32_t>(newData.size());
      if (cmd.origAbsOffset != kInvalidOffset) {
        emittedByOrig.emplace(cmd.origAbsOffset, newCmdOffsets[ti][ci]);
      }
      if (!validateU16Offset("Command", newCmdOffsets[ti][ci])) {
        return false;
      }

      size_t ptrPos = 0;
      std::string encodeError;
      if (!encodeCmdBytes(cmd, newData, /*writeDestWords=*/false, &ptrPos, &encodeError)) {
        setError(encodeError);
        return false;
      }

      if (cmd.type == CapcomCmdType::RepeatUntil || cmd.type == CapcomCmdType::RepeatBreak ||
          cmd.type == CapcomCmdType::Goto) {
        CapcomPointerPatch p;
        p.kind = cmd.type == CapcomCmdType::RepeatUntil ? CapcomPointerPatch::Kind::RepeatUntil
                 : cmd.type == CapcomCmdType::RepeatBreak ? CapcomPointerPatch::Kind::RepeatBreak
                                                          : CapcomPointerPatch::Kind::Goto;
        p.newPtrAbsOffset = m_seqBaseAbs + static_cast<uint32_t>(ptrPos);
        p.targetTrackIndex = cmd.destTrackIndex;
        p.targetCmdIndex = cmd.destCmdIndex;
        p.origWord = cmd.destWord;
        patches.push_back(p);
      }
    }
  }

  // second pass: map skipped clone commands to their emitted home addresses
  for (int ti = 0; ti < MAX_TRACKS; ++ti) {
    if (!m_tracks[ti]) continue;
    auto &trk = *m_tracks[ti];
    for (size_t ci = 0; ci < trk.cmds.size(); ++ci) {
      if (newCmdOffsets[ti][ci] != 0) continue;
      const auto &cmd = trk.cmds[ci];
      if (cmd.origAbsOffset != kInvalidOffset) {
        auto it = emittedByOrig.find(cmd.origAbsOffset);
        if (it != emittedByOrig.end()) newCmdOffsets[ti][ci] = it->second;
      }
    }
  }

  uint32_t headerPtrBase = m_priorityInHeader ? 1 : 0;
  for (int ti = 0; ti < MAX_TRACKS; ++ti) {
    uint16_t ptr = static_cast<uint16_t>(newTrackStarts[ti] & 0xFFFF);
    size_t pos = headerPtrBase + ti * 2;
    newData[pos] = static_cast<uint8_t>((ptr >> 8) & 0xFF);
    newData[pos + 1] = static_cast<uint8_t>(ptr & 0xFF);
  }

  for (auto &p : patches) {
    size_t pos = p.newPtrAbsOffset - m_seqBaseAbs;
    if (pos + 1 >= newData.size()) {
      std::ostringstream oss;
      oss << "Pointer patch position out of range: pos=" << pos << ", size=" << newData.size();
      setError(oss.str());
      return false;
    }

    int targetTrackIndex = p.targetTrackIndex;
    int targetCmdIndex = p.targetCmdIndex;
    if (targetTrackIndex < 0 || targetCmdIndex < 0) {
      for (int ti = 0; ti < MAX_TRACKS; ++ti) {
        if (!m_tracks[ti]) {
          continue;
        }
        int foundCmdIndex = findCmdIndexByOffset(ti, p.origWord);
        if (foundCmdIndex >= 0) {
          targetTrackIndex = ti;
          targetCmdIndex = foundCmdIndex;
          break;
        }
      }
    }

    if (targetTrackIndex < 0 || targetTrackIndex >= MAX_TRACKS) {
      std::ostringstream oss;
      oss << "Unresolved destination for pointer patch (kind=" << static_cast<int>(p.kind)
          << "), origWord=0x" << std::hex << p.origWord;
      setError(oss.str());
      return false;
    }

    const auto &cmdOffsets = newCmdOffsets[targetTrackIndex];
    if (targetCmdIndex < 0 || targetCmdIndex >= static_cast<int>(cmdOffsets.size())) {
      std::ostringstream oss;
      oss << "Pointer patch target cmd index out of range (kind=" << static_cast<int>(p.kind)
          << "), track=" << targetTrackIndex << ", cmd=" << targetCmdIndex;
      setError(oss.str());
      return false;
    }

    uint32_t destAbs = cmdOffsets[targetCmdIndex];
    if (destAbs == 0) {
      // target was in a skipped clone region with no emitted home; last
      // resort: look the original offset up across everything emitted
      const auto &tcmd = m_tracks[targetTrackIndex]->cmds[static_cast<size_t>(targetCmdIndex)];
      auto it = tcmd.origAbsOffset != kInvalidOffset ? emittedByOrig.find(tcmd.origAbsOffset)
                                                     : emittedByOrig.end();
      if (it == emittedByOrig.end()) {
        std::ostringstream oss;
        oss << "Jump target has no emitted home (track=" << targetTrackIndex
            << ", cmd=" << targetCmdIndex << ")";
        setError(oss.str());
        return false;
      }
      destAbs = it->second;
    }
    if (!validateU16Offset("Pointer destination", destAbs)) {
      return false;
    }

    uint16_t newWord = static_cast<uint16_t>(destAbs);
    newData[pos] = static_cast<uint8_t>((newWord >> 8) & 0xFF);
    newData[pos + 1] = static_cast<uint8_t>(newWord & 0xFF);
  }

  return true;
}

bool CapcomSeqIR::debugRebuildSizes(RawFile *raw, std::vector<uint32_t> *outSizes, std::string *error) {
  const bool saved = m_structuralChange;
  m_structuralChange = true;  // force the rebuild path
  std::vector<uint8_t> buf;
  const uint32_t savedAlloc = m_origAllocation;
  m_origAllocation = 0x8000;  // don't trip the budget check while measuring
  const bool ok = serializeToBuffer(raw, &buf, error);
  m_origAllocation = savedAlloc;
  m_structuralChange = saved;
  if (!ok) return false;
  if (outSizes) {
    outSizes->clear();
    outSizes->push_back(static_cast<uint32_t>(buf.size()));
    for (int ti = 0; ti < MAX_TRACKS; ++ti) {
      uint32_t sum = 0;
      if (m_tracks[ti]) for (const auto &cmd : m_tracks[ti]->cmds) sum += cmd.sizeBytes;
      outSizes->push_back(sum);
    }
  }
  return true;
}

bool CapcomSeqIR::serializeToRaw(RawFile *raw, std::string *error) {
  if (!raw || !raw->isWritable()) {
    if (error) {
      *error = "Raw file is not writable.";
    }
    return false;
  }

  std::vector<uint8_t> newData;
  if (!serializeToBuffer(raw, &newData, error)) {
    return false;
  }

  L_INFO("CapcomSeqIR serializing {} bytes starting at seqBase=0x{:x}, rawSize={}",
         newData.size(), m_seqBaseAbs, raw->size());

  if (m_seqBaseAbs + newData.size() > raw->size()) {
    std::ostringstream oss;
    oss << "Serialized data (" << newData.size() << " bytes) exceeds available space at offset 0x"
        << std::hex << m_seqBaseAbs;
    if (error) {
      *error = oss.str();
    }
    L_ERROR("CapcomSeqIR data too large: {} bytes at 0x{:x}, raw size {}",
            newData.size(), m_seqBaseAbs, raw->size());
    return false;
  }

  // The bytes past the original footprint belong to other ARAM data (samples,
  // instrument tables, the driver itself). Writing over them "succeeds" but
  // corrupts audio, so refuse instead of trusting the whole-file bound above.
  if (m_origAllocation > 0 && newData.size() > m_origAllocation) {
    std::ostringstream oss;
    oss << "Sequence grew to " << newData.size() << " bytes but only " << m_origAllocation
        << " bytes are allocated at 0x" << std::hex << m_seqBaseAbs
        << ". Remove or shorten events to fit the budget.";
    if (error) {
      *error = oss.str();
    }
    L_ERROR("CapcomSeqIR over budget: {} bytes > allocation {} at 0x{:x}",
            newData.size(), m_origAllocation, m_seqBaseAbs);
    return false;
  }

  if (!raw->writeBytes(m_seqBaseAbs, newData)) {
    std::ostringstream oss;
    oss << "Failed to write " << newData.size() << " bytes at offset 0x" << std::hex << m_seqBaseAbs;
    if (error) {
      *error = oss.str();
    }
    L_ERROR("CapcomSeqIR writeBytes failed at offset 0x{:x}", m_seqBaseAbs);
    return false;
  }

  L_INFO("CapcomSeqIR serialized {} bytes successfully", newData.size());
  return true;
}

bool CapcomSeqIR::insertProgramChange(int trackIndex, int beforeCmdIndex, uint8_t program) {
  if (trackIndex < 0 || trackIndex >= MAX_TRACKS || !m_tracks[trackIndex]) {
    return false;
  }

  auto &trk = *m_tracks[trackIndex];
  if (beforeCmdIndex < 0 || beforeCmdIndex > static_cast<int>(trk.cmds.size())) {
    return false;
  }

  CapcomCmdIR cmd;
  cmd.type = CapcomCmdType::ProgramChange;
  cmd.statusByte = 0x08;
  cmd.params.push_back(program);
  cmd.program = program;
  cmd.sizeBytes = 2;
  cmd.origAbsOffset = 0xFFFFFFFF;

  if (beforeCmdIndex < static_cast<int>(trk.cmds.size())) {
    cmd.tick = trk.cmds[beforeCmdIndex].tick;
  }

  trk.cmds.insert(trk.cmds.begin() + beforeCmdIndex, cmd);
  m_structuralChange = true;

  for (auto &c : trk.cmds) {
    if (c.destTrackIndex == trackIndex && c.destCmdIndex >= beforeCmdIndex) {
      c.destCmdIndex++;
    }
  }

  return true;
}

bool CapcomSeqIR::insertCommand(int trackIndex, int beforeCmdIndex, const CapcomCmdIR &cmdIn) {
  if (trackIndex < 0 || trackIndex >= MAX_TRACKS || !m_tracks[trackIndex]) {
    return false;
  }

  auto &trk = *m_tracks[trackIndex];
  if (beforeCmdIndex < 0 || beforeCmdIndex > static_cast<int>(trk.cmds.size())) {
    return false;
  }

  CapcomCmdIR cmd = cmdIn;
  cmd.origAbsOffset = 0xFFFFFFFF;
  if (beforeCmdIndex < static_cast<int>(trk.cmds.size())) {
    cmd.tick = trk.cmds[beforeCmdIndex].tick;
  }

  trk.cmds.insert(trk.cmds.begin() + beforeCmdIndex, cmd);
  m_structuralChange = true;

  for (auto &c : trk.cmds) {
    if (c.destTrackIndex == trackIndex && c.destCmdIndex >= beforeCmdIndex) {
      c.destCmdIndex++;
    }
  }

  return true;
}

bool CapcomSeqIR::updateCommand(int trackIndex, int cmdIndex, const CapcomCmdIR &cmdIn) {
  if (trackIndex < 0 || trackIndex >= MAX_TRACKS || !m_tracks[trackIndex]) {
    return false;
  }

  auto &trk = *m_tracks[trackIndex];
  if (cmdIndex < 0 || cmdIndex >= static_cast<int>(trk.cmds.size())) {
    return false;
  }

  CapcomCmdIR cmd = cmdIn;
  const auto &prev = trk.cmds[cmdIndex];
  cmd.origAbsOffset = prev.origAbsOffset;
  cmd.tick = prev.tick;
  cmd.destTrackIndex = prev.destTrackIndex;
  cmd.destCmdIndex = prev.destCmdIndex;
  cmd.destWord = prev.destWord;
  cmd.repeatSlot = prev.repeatSlot;
  cmd.repeatCount = prev.repeatCount;
  cmd.breakAttributes = prev.breakAttributes;

  // A same-size replacement keeps the layout intact; a size change is
  // structural and forces the compacting serializer.
  std::vector<uint8_t> enc;
  if (encodeCmdBytes(cmd, enc, /*writeDestWords=*/true, nullptr, nullptr)) {
    if (enc.size() != prev.sizeBytes) {
      m_structuralChange = true;
    }
    cmd.sizeBytes = static_cast<uint8_t>(enc.size());
  } else {
    m_structuralChange = true;
  }

  trk.cmds[cmdIndex] = cmd;
  return true;
}

bool CapcomSeqIR::removeCommand(int trackIndex, int cmdIndex) {
  if (trackIndex < 0 || trackIndex >= MAX_TRACKS || !m_tracks[trackIndex]) {
    return false;
  }

  auto &trk = *m_tracks[trackIndex];
  if (cmdIndex < 0 || cmdIndex >= static_cast<int>(trk.cmds.size())) {
    return false;
  }

  trk.cmds.erase(trk.cmds.begin() + cmdIndex);
  m_structuralChange = true;

  for (auto &c : trk.cmds) {
    if (c.destTrackIndex != trackIndex) {
      continue;
    }
    if (c.destCmdIndex > cmdIndex) {
      c.destCmdIndex--;
    }
  }

  return true;
}

CapcomTrackIR *CapcomSeqIR::track(int index) {
  if (index < 0 || index >= MAX_TRACKS || !m_tracks[index]) {
    return nullptr;
  }
  return &(*m_tracks[index]);
}

const CapcomTrackIR *CapcomSeqIR::track(int index) const {
  if (index < 0 || index >= MAX_TRACKS || !m_tracks[index]) {
    return nullptr;
  }
  return &(*m_tracks[index]);
}

int CapcomSeqIR::findTrackIndexByStartOffset(uint32_t trackStart) const {
  for (int i = 0; i < MAX_TRACKS; ++i) {
    if (!m_tracks[i]) {
      continue;
    }
    if (m_tracks[i]->origTrackStartAbs == trackStart) {
      return i;
    }
  }
  return -1;
}

int CapcomSeqIR::findCmdIndexByOffset(int trackIndex, uint32_t offset) const {
  const auto *trk = track(trackIndex);
  if (!trk) return -1;

  for (int i = 0; i < static_cast<int>(trk->cmds.size()); ++i) {
    if (trk->cmds[i].origAbsOffset == offset) {
      return i;
    }
  }
  return -1;
}
