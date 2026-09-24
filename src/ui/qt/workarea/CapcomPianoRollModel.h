#pragma once

#include <QObject>
#include <vector>
#include <string>
#include <memory>
#include <set>

#include "RawFile.h"
#include "formats/CapcomSnes/CapcomSnesSeq.h"
#include "formats/CapcomSnes/CapcomSeqIR.h"

struct CapcomNoteEvent {
  // Every encoded event contributing to this displayed span, including its
  // own pitch/timing context. A tie is not necessarily adjacent raw bytes.
  std::vector<CapcomCmdIR> segments;
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
  // Replayed after the song-loop GOTO: same bytes as a first-pass event, so
  // edits must never target it (they would land at the first-pass tick).
  bool isSongLoopReplay{false};
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
  // The song loop: the first GOTO whose destination was already played. A
  // GOTO into bytes not yet played (a channel borrowing another channel's
  // melody) is not a loop. Every event at or after songLoopTick is a replay
  // of the first pass (see CapcomNoteEvent::isSongLoopReplay).
  bool hasSongLoop{false};
  uint32_t songLoopTick{0};        // where the loop jumps back = end of the first pass
  uint32_t songLoopDestTick{0};    // where playback resumes
  uint32_t songLoopGotoOffset{0};  // raw offset of the looping GOTO
};

class CapcomPianoRollModel : public QObject {
  Q_OBJECT
 public:
  // Groups existing edit primitives into one history entry. Scope exit rolls
  // back unless committed, including the original undo/redo histories.
  class EditTransaction;
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
                        std::string *error = nullptr,
                        const CapcomNoteEvent *sourceNote = nullptr);

  // Append a note past the end of a track's events, filling the gap with
  // rests. This is the only way to put notes on a still-empty track, where
  // InsertNoteAtTick has no rest event to split.
  bool AppendNoteAtTick(int trackIndex,
                        uint32_t tick,
                        int midiKey,
                        uint32_t lenTicks,
                        std::string *error = nullptr,
                        const CapcomNoteEvent *sourceNote = nullptr);

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
  // End the song's loop: replace every track's trailing whole-song GOTO with
  // an END, so the song plays through once and stops instead of repeating.
  // No-op (returns true) when nothing loops. Undoable.
  bool endSongLoops(std::string *error = nullptr);
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

  // Exact single-event lengths in ticks (PPQN 48), including dotted/triplet.
  // A pending dotted command and available surrounding silence can restrict these.
  static std::vector<uint32_t> supportedNoteLengths(bool pendingDotted = false);

 private:
  static bool buildTimedEvent(uint32_t ticks, uint8_t key, bool dotted, bool triplet,
                              std::vector<CapcomCmdIR> &commands);
  static bool buildRests(uint32_t ticks, bool dotted, bool triplet,
                         std::vector<CapcomCmdIR> &commands);
  static bool buildArticulatedNote(uint32_t ticks, uint8_t key, bool dotted, bool triplet,
                                  bool slurred, uint8_t durationRate, uint8_t program,
                                  const CapcomNoteEvent *sourceNote, std::vector<CapcomCmdIR> &commands);
  bool replaceTimedEvent(int trackIndex, uint32_t offset,
                         const std::vector<CapcomCmdIR> &commands, std::string *error,
                         int replaceCount = 1);
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

  // Channels can play notes stored in another channel's bytes (Capcom's
  // melody sharing). Edits made through the borrowing channel would be dropped
  // by the serializer, which writes each command from its home track only, so
  // they are refused with a message naming the owning track.
  [[nodiscard]] int homeTrackOfOffset(uint32_t offset) const;
  bool rejectBorrowed(int trackIndex, uint32_t offset, std::string *error) const;

  // Self-cleaning for instrument edits: drop a ProgramChange that is either
  // overwritten at the same tick with nothing audible in between (dead
  // store) or re-sets the program the voice already holds. Both are in the
  // inaudible class by corpus render measurement (see regression test
  // test_instrument_edits_self_clean). Jump landings are never removed and
  // reset the known state. Returns the number of commands dropped.
  int dropDeadProgramChanges(const std::set<int> &trackIndices);

  static std::vector<std::pair<RawFile *, uint32_t>> s_recentEdits;

};

class CapcomPianoRollModel::EditTransaction {
 public:
  explicit EditTransaction(CapcomPianoRollModel &model);
  ~EditTransaction();
  EditTransaction(const EditTransaction &) = delete;
  EditTransaction &operator=(const EditTransaction &) = delete;
  bool commit(std::string *error = nullptr);
  bool rollback(std::string *error = nullptr);

 private:
  CapcomPianoRollModel &m_model;
  RawFile *m_raw;
  std::vector<uint8_t> m_before;
  std::vector<std::vector<EditEntry>> m_undo;
  std::vector<std::vector<EditEntry>> m_redo;
  std::vector<std::pair<RawFile *, uint32_t>> m_recentEdits;
  uint32_t m_allocationFloor;
  bool m_active{true};
};
