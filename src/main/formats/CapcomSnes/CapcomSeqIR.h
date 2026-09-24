#pragma once

#include <vector>
#include <array>
#include <optional>
#include <unordered_map>
#include <cstdint>
#include <string>

#include "nlohmann/json.hpp"

class RawFile;
class CapcomSnesSeq;

enum class CapcomCmdType {
  Note,
  Rest,
  ToggleTriplet,
  ToggleSlur,
  DottedNoteOn,
  ToggleOctaveUp,
  NoteAttributes,
  Tempo,
  Duration,
  Volume,
  Pan,
  MasterVolume,
  ProgramChange,
  Octave,
  GlobalTranspose,
  Transpose,
  Tuning,
  PortamentoTime,
  RepeatUntil,
  RepeatBreak,
  Goto,
  End,
  LFO,
  EchoParam,
  EchoOnOff,
  ReleaseRate,
  NoOp,
  Unknown
};

struct CapcomCmdIR {
  CapcomCmdType type{CapcomCmdType::Unknown};

  uint32_t origAbsOffset{0};
  uint8_t statusByte{0};
  uint8_t sizeBytes{1};
  uint32_t tick{0};

  std::vector<uint8_t> params;

  uint8_t keyIndex{0};
  uint8_t lenIndex{0};
  uint8_t program{0};
  uint8_t noteAttributes{0};
  uint8_t durationRate{0};
  int8_t transpose{0};
  int8_t globalTranspose{0};

  uint16_t destWord{0};
  int destTrackIndex{-1};
  int destCmdIndex{-1};

  uint8_t repeatSlot{0};
  uint8_t repeatCount{0};
  uint8_t breakAttributes{0};
};

struct CapcomTrackIR {
  int trackIndex{-1};
  uint32_t origTrackStartAbs{0};
  uint16_t origTrackPtrValue{0};
  std::vector<CapcomCmdIR> cmds;
};

struct CapcomPointerPatch {
  enum class Kind { HeaderTrackPtr, RepeatUntil, RepeatBreak, Goto };

  Kind kind{Kind::HeaderTrackPtr};
  uint32_t newPtrAbsOffset{0};
  int targetTrackIndex{-1};
  int targetCmdIndex{-1};
  uint16_t origWord{0};
};

class CapcomSeqIR {
public:
  static constexpr int MAX_TRACKS = 8;

  CapcomSeqIR() = default;

  bool parseFromSeq(CapcomSnesSeq *seq, RawFile *raw);
  // Parse directly from a raw ARAM-addressed image (no CapcomSnesSeq needed).
  // `seqBaseAbs` is the song header offset in `raw` (Goof Troop: 0x0D20).
  bool parseFromImage(RawFile *raw, uint32_t seqBaseAbs, bool priorityInHeader);
  bool serializeToRaw(RawFile *raw, std::string *error = nullptr);
  // Build the serialized sequence image without writing anything. `raw` is
  // only read (priority byte); safe on read-only files. Used by the round-trip
  // parity harness and previews.
  bool serializeToBuffer(RawFile *raw, std::vector<uint8_t> *out, std::string *error = nullptr);
  bool toJson(nlohmann::json *out, std::string *error = nullptr) const;
  bool loadFromJson(const nlohmann::json &json, std::string *error = nullptr);

  bool insertProgramChange(int trackIndex, int beforeCmdIndex, uint8_t program);
  bool insertCommand(int trackIndex, int beforeCmdIndex, const CapcomCmdIR &cmd);
  // Diagnostics: force the rebuild serialization path and report per-track
  // emitted byte counts (layout-preserving path hides rebuild-only bugs).
  bool debugRebuildSizes(RawFile *raw, std::vector<uint32_t> *outSizes, std::string *error);
  bool updateCommand(int trackIndex, int cmdIndex, const CapcomCmdIR &cmd);
  bool removeCommand(int trackIndex, int cmdIndex);

  [[nodiscard]] CapcomTrackIR *track(int index);
  [[nodiscard]] const CapcomTrackIR *track(int index) const;
  [[nodiscard]] int findTrackIndexByStartOffset(uint32_t trackStart) const;
  [[nodiscard]] int findCmdIndexByOffset(int trackIndex, uint32_t offset) const;

  // Byte footprint the sequence occupied when parsed (header + all command
  // bytes, relative to the sequence base). Serialization must not grow past
  // this: the bytes after it belong to other ARAM data (samples, driver).
  // 0 = unknown (e.g. IR loaded from JSON), which falls back to file bounds.
  [[nodiscard]] uint32_t originalAllocation() const { return m_origAllocation; }
  // Raise the serialization budget above the parsed footprint (e.g. a new
  // song may grow into the region the game reserves for its largest track).
  void setMinAllocation(uint32_t bytes) { m_minAllocation = bytes; m_origAllocation = std::max(m_origAllocation, bytes); }

private:
  void initEventMap();
  bool parseTrackIR(int trackIndex, uint32_t trackStart, RawFile *raw);
  void resolveDestinations();

  uint32_t m_seqBaseAbs{0};
  uint32_t m_origAllocation{0};
  uint32_t m_minAllocation{0};
  bool m_priorityInHeader{false};
  // False while every command still sits at its parse-time offset with its
  // parse-time size. Structural edits (insert/remove/resize) force the
  // compacting serializer; otherwise commands are written back in place,
  // which keeps unedited songs byte-identical (round-trip parity).
  bool m_structuralChange{false};
  std::array<std::optional<CapcomTrackIR>, MAX_TRACKS> m_tracks;
  std::unordered_map<uint8_t, CapcomCmdType> m_eventMap;
};
