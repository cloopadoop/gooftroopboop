/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "CapcomSnesFormat.h"
#include "MidiReader.h"

class CapcomSnesWriter {
 public:
  struct WriterConfig {
    CapcomSnesVersion version = CAPCOMSNES_V1_BGM_IN_LIST;
    uint16_t baseAddress = 0x0D20;  // ARAM load address (Goof Troop default)
    uint8_t priority = 0;
    bool priorityInHeader = false;  // V1 typically has no priority byte
    uint8_t initialDurationRate = 0xC0;  // ~75% gate time (engine default)
    uint8_t defaultProgram = 0;  // Used when a track has notes but no MIDI program change
    bool enforceProgramLimit = true;
    uint8_t maxProgram = 21;  // Goof Troop instrument set uses slots 0-21
    int loopStartTick = -1;  // -1 = no loop
    bool stripAutomation = false;  // Remove volume/pan changes, keep only initial
    bool gapPackAllowProgramSwaps = true;  // Enabled by default; note-source mapping preserves donor instrument.
    
    // Program number mapping: MIDI program -> CapcomSnes instrument index
    // If empty, uses MIDI program numbers directly
    std::vector<uint8_t> programMap;

    // Per-output-program pitch compensation. Capcom instruments carry their own
    // pitch scale (CapcomSnesInstrSet bytes 4-5 BE), so instruments are not
    // pitch-neutral relative to each other (e.g. programs 1/5/15 sit ~11.24
    // semitones above 6/8/10). Indexed by the *mapped* Capcom program number.
    // `programTransposeMap`: signed semitone shift emitted via EVENT_TRANSPOSE.
    // `programTuningMap`: signed EVENT_TUNING units (1/256 semitone).
    std::vector<int8_t> programTransposeMap;
    std::vector<int8_t> programTuningMap;
  };

  struct EncodedTrack {
    std::vector<uint8_t> data;
    uint16_t address;  // ARAM address for this track
  };

  CapcomSnesWriter();
  ~CapcomSnesWriter() = default;

  bool convertFromMidi(const MidiReader& midi, const WriterConfig& config);
  bool writeToFile(const std::string& filepath);
  const std::vector<uint8_t>& getSequenceData() const { return m_sequenceData; }
  
  std::string getErrorMessage() const { return m_errorMessage; }
  const std::vector<std::string>& getWarnings() const { return m_warnings; }

 private:
  static constexpr int MAX_TRACKS = 8;
  static constexpr int SEQ_PPQN = 48;
  
  // Event opcodes
  static constexpr uint8_t EVENT_TOGGLE_TRIPLET = 0x00;
  static constexpr uint8_t EVENT_TOGGLE_SLUR = 0x01;
  static constexpr uint8_t EVENT_DOTTED_NOTE_ON = 0x02;
  static constexpr uint8_t EVENT_TOGGLE_OCTAVE_UP = 0x03;
  static constexpr uint8_t EVENT_NOTE_ATTRIBUTES = 0x04;
  static constexpr uint8_t EVENT_TEMPO = 0x05;
  static constexpr uint8_t EVENT_DURATION = 0x06;
  static constexpr uint8_t EVENT_VOLUME = 0x07;
  static constexpr uint8_t EVENT_PROGRAM_CHANGE = 0x08;
  static constexpr uint8_t EVENT_OCTAVE = 0x09;
  static constexpr uint8_t EVENT_GLOBAL_TRANSPOSE = 0x0A;
  static constexpr uint8_t EVENT_TRANSPOSE = 0x0B;
  static constexpr uint8_t EVENT_TUNING = 0x0C;
  static constexpr uint8_t EVENT_PORTAMENTO_TIME = 0x0D;
  static constexpr uint8_t EVENT_GOTO = 0x16;
  static constexpr uint8_t EVENT_END = 0x17;
  static constexpr uint8_t EVENT_PAN = 0x18;
  static constexpr uint8_t EVENT_MASTER_VOLUME = 0x19;
  static constexpr uint8_t EVENT_LFO = 0x1A;
  static constexpr uint8_t EVENT_ECHO_PARAM = 0x1B;
  static constexpr uint8_t EVENT_ECHO_ONOFF = 0x1C;
  static constexpr uint8_t EVENT_RELEASE_RATE = 0x1D;
  
  struct TrackState {
    uint8_t channel;
    uint8_t octave;
    bool octaveUp;
    bool tripletMode;
    bool slurMode;
    uint8_t durationRate;
    uint8_t volume;
    uint8_t pan;
    uint8_t program;
    int8_t transpose;
    int8_t tuning;
  };

  struct Note {
    uint32_t absTime;
    uint8_t channel = 0;
    uint8_t key;
    uint8_t velocity;
    uint32_t duration;
    uint8_t sourceProgram = 0xFF;
  };

  bool quantizeMidiToSEQPPQN(const MidiReader& midi);
  bool allocateTracks();
  bool encodeAllTracks();
  bool encodeTrack(int trackIndex);
  bool writeHeader();
  
  void emitRest(uint32_t ticks, TrackState& state, std::vector<uint8_t>& out);
  void emitNote(uint8_t key, uint32_t ticks, TrackState& state, std::vector<uint8_t>& out);
  uint8_t getLengthIndex(uint32_t ticks, bool& useDotted, bool& useTriplet);
  uint8_t tempoToBPM(uint16_t tempo);
  uint16_t BPMToTempo(double bpm);
  uint8_t midiVolToCapcom(uint8_t midiVol);
  int8_t midiPanToCapcom(uint8_t midiPan);
  int8_t programTransposeFor(uint8_t program) const;
  int8_t programTuningFor(uint8_t program) const;
  void emitProgramPitchCompensation(uint8_t program, TrackState& state, std::vector<uint8_t>& out);
  
  void write8(std::vector<uint8_t>& out, uint8_t value);
  void write16BE(std::vector<uint8_t>& out, uint16_t value);
  void addWarning(const std::string& msg);

  WriterConfig m_config;
  std::vector<EncodedTrack> m_tracks;
  std::vector<uint8_t> m_sequenceData;
  std::string m_errorMessage;
  std::vector<std::string> m_warnings;
  
  // Quantized MIDI data
  std::vector<std::vector<Note>> m_trackNotes;
  std::vector<std::vector<MidiReader::ControlChange>> m_trackControls;
  std::vector<std::vector<MidiReader::ProgramChange>> m_trackPrograms;
  std::vector<MidiReader::TempoChange> m_tempos;
};

