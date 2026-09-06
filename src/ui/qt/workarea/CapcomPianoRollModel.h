#pragma once

#include <QObject>
#include <vector>
#include <string>
#include <memory>

#include "RawFile.h"
#include "formats/CapcomSnes/CapcomSnesSeq.h"
#include "formats/CapcomSnes/CapcomSeqIR.h"

struct CapcomNoteEvent {
  int trackIndex{-1};
  uint32_t rawOffset{0};
  uint32_t programChangeOffset{0};
  uint8_t statusByte{0};
  uint8_t keyIndex{0};
  uint8_t lenIndex{0};
  uint8_t program{0};
  uint32_t startTick{0};
  uint32_t deltaTicks{0};
  uint32_t durationTicks{0};
  uint8_t octave{0};
  bool octaveUp{false};
  bool dotted{false};
  bool triplet{false};
  bool slurred{false};
  uint8_t durationRate{0};
  int8_t transpose{0};
  int8_t globalTranspose{0};
  int midiKey{-1};
  bool isRest{false};
  bool isLoopRepeat{false};
  uint32_t loopSourceOffset{0};
  std::string instrumentName;
};

enum class CapcomSettingType {
  Tempo,
  Volume,
  Pan,
  Duration,
  Octave,
  Transpose,
  GlobalTranspose,
  LFO,
  Echo,
  ReleaseRate,
  Portamento,
  Unknown
};

struct CapcomSettingEvent {
  int trackIndex{-1};
  uint32_t rawOffset{0};
  uint32_t tick{0};
  uint8_t statusByte{0};
  uint8_t sizeBytes{0};
  CapcomSettingType type{CapcomSettingType::Unknown};
  uint8_t value1{0};
  uint8_t value2{0};
  std::string description;
};

struct CapcomLoopEvent {
  int trackIndex{-1};
  uint32_t rawOffset{0};
  uint32_t tick{0};
  uint8_t slot{0};
  uint8_t repeatCount{0};
  uint16_t destOffset{0};
  uint32_t destTick{0};
  bool destTickValid{false};
};

struct CapcomTrackData {
  uint32_t trackOffset{0};
  std::vector<CapcomNoteEvent> notes;
  std::vector<CapcomSettingEvent> settings;
  std::vector<CapcomLoopEvent> loops;
};

class CapcomPianoRollModel : public QObject {
  Q_OBJECT
 public:
  explicit CapcomPianoRollModel(CapcomSnesSeq *seq, QObject *parent = nullptr);

  bool reload();
 [[nodiscard]] int trackCount() const { return static_cast<int>(m_tracks.size()); }
  [[nodiscard]] const CapcomTrackData *trackData(int trackIndex) const;
  [[nodiscard]] const std::vector<CapcomTrackData> &tracks() const { return m_tracks; }
  [[nodiscard]] RawFile *rawFile() const { return m_raw; }
  [[nodiscard]] bool canWrite() const { return m_writableRaw != nullptr; }
  static const std::vector<std::pair<RawFile *, uint32_t>> &recentEdits();
  [[nodiscard]] const std::vector<std::pair<uint8_t, std::string>> &programNames() const {
    return m_programNames;
  }

  // Edit a single note and immediately write the change to the backing RawFile.
  bool applyEdit(int trackIndex,
                 size_t noteIndex,
                 int midiKey,
                 uint32_t targetLenTicks,
                 bool makeRest,
                 std::string *error = nullptr);
  // Serialization budget floor for new/rehomed songs (0 = parsed footprint).
  void setAllocationFloor(uint32_t bytes) { m_allocationFloor = bytes; if (m_ir) m_ir->setMinAllocation(bytes); }

  bool InsertNoteAtTick(int track_index,
                        uint32_t tick,
                        int midi_key,
                        uint32_t target_len_ticks,
                        uint32_t *out_tick = nullptr,
                        uint32_t *out_duration_diff = nullptr,
                        std::string *error = nullptr);

  // Append a note past the end of a track's events, filling the gap with
  // rests. This is the only way to put notes on a still-empty track, where
  // InsertNoteAtTick has no rest event to split.
  bool AppendNoteAtTick(int trackIndex,
                        uint32_t tick,
                        int midiKey,
                        uint32_t lenTicks,
                        std::string *error = nullptr);

  bool MoveNote(int trackIndex,
                size_t noteIndex,
                uint32_t tick,
                int midiKey,
                uint32_t *out_tick = nullptr,
                std::string *error = nullptr);
  bool eraseNotes(const std::vector<std::pair<int, int>> &noteRefs, std::string *error = nullptr);
  bool setInstrument(const std::vector<std::pair<int, int>> &noteRefs, uint8_t program, std::string *error = nullptr);
  bool updateSetting(int trackIndex,
                     size_t settingIndex,
                     CapcomSettingType type,
                     uint16_t value,
                     uint8_t value2,
                     std::string *error = nullptr);
  bool addSetting(int trackIndex,
                  uint32_t tick,
                  CapcomSettingType type,
                  uint16_t value,
                  uint8_t value2,
                  std::string *error = nullptr);
  bool removeSetting(int trackIndex, size_t settingIndex, std::string *error = nullptr);
  bool updateCommandParam(int trackIndex,
                          int cmdIndex,
                          int paramIndex,
                          uint8_t value,
                          std::string *error = nullptr);
  bool updateLoopRepeatCount(int trackIndex,
                             size_t loopIndex,
                             uint8_t repeatCount,
                             std::string *error = nullptr);
  bool createLoop(int trackIndex,
                  uint32_t startTick,
                  uint32_t endTick,
                  uint8_t slot,
                  uint8_t repeatCount,
                  std::string *error = nullptr);
  bool removeLoop(int trackIndex, size_t loopIndex, std::string *error = nullptr);
  // Reclaim sequence bytes: merge adjacent rests into single encodable rest
  // events and drop dead/duplicate setting commands. Safe by construction:
  // jump targets are never removed and act as barriers. Undoable.
  bool optimizeSequence(uint32_t *bytesBefore, uint32_t *bytesAfter, std::string *error = nullptr);

  bool undo(std::string *error = nullptr);
  bool redo(std::string *error = nullptr);
  [[nodiscard]] bool canUndo() const { return !m_undo.empty(); }
  [[nodiscard]] bool canRedo() const { return !m_redo.empty(); }
  bool flush(std::string *error = nullptr);

  [[nodiscard]] const CapcomTrackIR *irTrack(int trackIndex, std::string *error = nullptr);

  // Current serialized size vs the song's original ARAM allocation.
  // Returns false when the sequence can't be parsed/serialized.
  bool byteUsage(uint32_t *usedOut, uint32_t *budgetOut);

 private:
  bool parseTrack(int trackIndex, uint32_t trackOffset, CapcomTrackData &outTrack);
  static uint32_t lengthFromIndex(uint8_t lenIndex, bool dotted, bool triplet);
  static uint32_t durationFromLength(uint32_t len, uint8_t durationRate, bool slurred);
  static uint8_t chooseLenIndex(uint32_t targetLen, bool dotted, bool triplet);
  static int computeMidiKey(uint8_t keyIndex,
                            uint8_t octave,
                            bool octaveUp,
                            int8_t transpose,
                            int8_t globalTranspose);
  static uint8_t clampKeyIndex(int candidate);
  std::string instrumentNameForProgram(uint8_t program) const;

  static void recordEdit(RawFile *raw, uint32_t offset);

 private:
  CapcomSnesSeq *m_seq;
  RawFile *m_raw;
  RawFile *m_writableRaw;
  std::vector<CapcomTrackData> m_tracks;
  std::vector<std::pair<uint8_t, std::string>> m_programNames;
  std::unique_ptr<CapcomSeqIR> m_ir;
  uint32_t m_allocationFloor{0};

  struct EditEntry {
    RawFile *raw;
    uint32_t offset;
    uint8_t oldVal;
    uint8_t newVal;
  };
  std::vector<std::vector<EditEntry>> m_undo;
  std::vector<std::vector<EditEntry>> m_redo;

  void pushUndoBatch(std::vector<EditEntry> batch);

  // Snapshot/diff helpers so structural (IR-reserialized) edits are undoable:
  // capture the raw before the edit, then push the byte-diff as an undo batch.
  [[nodiscard]] std::vector<uint8_t> captureRawSnapshot() const;
  void pushRawDiffUndo(const std::vector<uint8_t> &before);

  static std::vector<std::pair<RawFile *, uint32_t>> s_recentEdits;

};
