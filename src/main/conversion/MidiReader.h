/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once
#include <string>
#include <vector>
#include <cstdint>

class MidiReader {
 public:
  struct TempoChange {
    uint32_t absTime;
    uint32_t microSecondsPerQuarter;
    
    double getBPM() const {
      return 60000000.0 / microSecondsPerQuarter;
    }
  };

  struct ProgramChange {
    uint32_t absTime;
    uint8_t channel;
    uint8_t program;
  };

  struct ControlChange {
    uint32_t absTime;
    uint8_t channel;
    uint8_t controller;
    uint8_t value;
  };

  struct PitchBend {
    uint32_t absTime;
    uint8_t channel;
    int16_t value;  // -8192 to +8191
  };

  struct MidiNote {
    uint32_t absTime;
    uint8_t channel;
    uint8_t key;
    uint8_t velocity;
    uint32_t duration;  // in ticks
  };

  struct TimeSig {
    uint32_t absTime;
    uint8_t numerator;
    uint8_t denominator;
    uint8_t clocksPerClick;
    uint8_t num32ndNotesPerQuarter;
  };

  struct MidiTrackData {
    std::vector<MidiNote> notes;
    std::vector<ControlChange> controllers;
    std::vector<ProgramChange> programs;
    std::vector<PitchBend> pitchBends;
    std::vector<TempoChange> tempos;
    std::vector<TimeSig> timeSigs;
    std::string trackName;
  };

  MidiReader();
  ~MidiReader() = default;

  bool loadFromFile(const std::string& filepath);
  bool loadFromBuffer(const std::vector<uint8_t>& buffer);
  
  uint16_t getPPQN() const { return m_ppqn; }
  uint16_t getFormat() const { return m_format; }
  uint16_t getNumTracks() const { return static_cast<uint16_t>(m_tracks.size()); }
  const std::vector<MidiTrackData>& getTracks() const { return m_tracks; }
  
  std::string getErrorMessage() const { return m_errorMessage; }

 private:
  bool parseHeader(const uint8_t* data, size_t size, size_t& offset);
  bool parseTrack(const uint8_t* data, size_t size, size_t& offset);
  uint32_t readVarLength(const uint8_t* data, size_t size, size_t& offset);
  uint16_t read16BE(const uint8_t* data, size_t offset);
  uint32_t read32BE(const uint8_t* data, size_t offset);

  uint16_t m_ppqn;
  uint16_t m_format;
  std::vector<MidiTrackData> m_tracks;
  std::string m_errorMessage;
};
