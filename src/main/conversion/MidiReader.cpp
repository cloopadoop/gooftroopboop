/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "MidiReader.h"
#include <fstream>
#include <cstring>
#include <unordered_map>

MidiReader::MidiReader() : m_ppqn(480), m_format(1) {}

bool MidiReader::loadFromFile(const std::string& filepath) {
  std::ifstream file(filepath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    m_errorMessage = "Failed to open file: " + filepath;
    return false;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(size);
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    m_errorMessage = "Failed to read file: " + filepath;
    return false;
  }

  return loadFromBuffer(buffer);
}

bool MidiReader::loadFromBuffer(const std::vector<uint8_t>& buffer) {
  m_tracks.clear();
  m_errorMessage.clear();

  if (buffer.size() < 14) {
    m_errorMessage = "File too small to be a valid MIDI file";
    return false;
  }

  size_t offset = 0;
  const uint8_t* data = buffer.data();
  size_t size = buffer.size();

  // Parse header
  if (!parseHeader(data, size, offset)) {
    return false;
  }

  // Parse tracks
  uint16_t numTracks = m_format == 0 ? 1 : read16BE(data, 10);
  for (uint16_t i = 0; i < numTracks; i++) {
    if (!parseTrack(data, size, offset)) {
      return false;
    }
  }

  return true;
}

bool MidiReader::parseHeader(const uint8_t* data, size_t size, size_t& offset) {
  // Check MThd signature
  if (offset + 14 > size) {
    m_errorMessage = "Unexpected end of file in header";
    return false;
  }

  if (std::memcmp(data + offset, "MThd", 4) != 0) {
    m_errorMessage = "Invalid MIDI header signature";
    return false;
  }
  offset += 4;

  // Header length (should be 6)
  uint32_t headerLen = read32BE(data, offset);
  offset += 4;

  if (headerLen != 6) {
    m_errorMessage = "Invalid MIDI header length";
    return false;
  }

  // Format type
  m_format = read16BE(data, offset);
  offset += 2;

  // Number of tracks
  uint16_t numTracks = read16BE(data, offset);
  offset += 2;

  // PPQN (ticks per quarter note)
  m_ppqn = read16BE(data, offset);
  offset += 2;

  // Check for SMPTE time division (not supported)
  if (m_ppqn & 0x8000) {
    m_errorMessage = "SMPTE time division not supported";
    return false;
  }

  return true;
}

bool MidiReader::parseTrack(const uint8_t* data, size_t size, size_t& offset) {
  // Check MTrk signature
  if (offset + 8 > size) {
    m_errorMessage = "Unexpected end of file in track header";
    return false;
  }

  if (std::memcmp(data + offset, "MTrk", 4) != 0) {
    m_errorMessage = "Invalid MIDI track signature";
    return false;
  }
  offset += 4;

  // Track length
  uint32_t trackLen = read32BE(data, offset);
  offset += 4;

  size_t trackEnd = offset + trackLen;
  if (trackEnd > size) {
    m_errorMessage = "Track length exceeds file size";
    return false;
  }

  MidiTrackData track;
  uint32_t absTime = 0;
  uint8_t runningStatus = 0;

  // Track note on events to pair with note offs
  std::unordered_map<uint16_t, std::pair<uint32_t, uint8_t>> activeNotes;  // key: (channel << 8) | note, value: (startTime, velocity)

  while (offset < trackEnd) {
    // Read delta time
    uint32_t deltaTime = readVarLength(data, size, offset);
    absTime += deltaTime;

    if (offset >= trackEnd) {
      break;
    }

    // Read event
    uint8_t status = data[offset];
    
    // Handle running status
    if (status < 0x80) {
      status = runningStatus;
    } else {
      offset++;
      runningStatus = status;
    }

    uint8_t eventType = status & 0xF0;
    uint8_t channel = status & 0x0F;

    // Note Off
    if (eventType == 0x80) {
      if (offset + 2 > trackEnd) break;
      uint8_t note = data[offset++];
      uint8_t velocity = data[offset++];
      
      uint16_t noteKey = (channel << 8) | note;
      if (activeNotes.find(noteKey) != activeNotes.end()) {
        auto [startTime, startVel] = activeNotes[noteKey];
        track.notes.push_back({startTime, channel, note, startVel, absTime - startTime});
        activeNotes.erase(noteKey);
      }
    }
    // Note On
    else if (eventType == 0x90) {
      if (offset + 2 > trackEnd) break;
      uint8_t note = data[offset++];
      uint8_t velocity = data[offset++];
      
      uint16_t noteKey = (channel << 8) | note;
      
      // Velocity 0 is note off
      if (velocity == 0) {
        if (activeNotes.find(noteKey) != activeNotes.end()) {
          auto [startTime, startVel] = activeNotes[noteKey];
          track.notes.push_back({startTime, channel, note, startVel, absTime - startTime});
          activeNotes.erase(noteKey);
        }
      } else {
        // Close previous note if still active
        if (activeNotes.find(noteKey) != activeNotes.end()) {
          auto [startTime, startVel] = activeNotes[noteKey];
          track.notes.push_back({startTime, channel, note, startVel, absTime - startTime});
        }
        activeNotes[noteKey] = {absTime, velocity};
      }
    }
    // Polyphonic Key Pressure
    else if (eventType == 0xA0) {
      if (offset + 2 > trackEnd) break;
      offset += 2;  // Skip
    }
    // Control Change
    else if (eventType == 0xB0) {
      if (offset + 2 > trackEnd) break;
      uint8_t controller = data[offset++];
      uint8_t value = data[offset++];
      track.controllers.push_back({absTime, channel, controller, value});
    }
    // Program Change
    else if (eventType == 0xC0) {
      if (offset + 1 > trackEnd) break;
      uint8_t program = data[offset++];
      track.programs.push_back({absTime, channel, program});
    }
    // Channel Pressure
    else if (eventType == 0xD0) {
      if (offset + 1 > trackEnd) break;
      offset += 1;  // Skip
    }
    // Pitch Bend
    else if (eventType == 0xE0) {
      if (offset + 2 > trackEnd) break;
      uint8_t lsb = data[offset++];
      uint8_t msb = data[offset++];
      int16_t value = ((msb << 7) | lsb) - 8192;
      track.pitchBends.push_back({absTime, channel, value});
    }
    // System/Meta events
    else if (status == 0xFF) {
      if (offset + 1 > trackEnd) break;
      uint8_t metaType = data[offset++];
      uint32_t metaLen = readVarLength(data, size, offset);
      
      if (offset + metaLen > trackEnd) break;

      // Tempo
      if (metaType == 0x51 && metaLen == 3) {
        uint32_t tempo = (data[offset] << 16) | (data[offset + 1] << 8) | data[offset + 2];
        track.tempos.push_back({absTime, tempo});
      }
      // Time Signature
      else if (metaType == 0x58 && metaLen == 4) {
        track.timeSigs.push_back({absTime, data[offset], data[offset + 1], data[offset + 2], data[offset + 3]});
      }
      // Track Name
      else if (metaType == 0x03) {
        track.trackName = std::string(reinterpret_cast<const char*>(data + offset), metaLen);
      }
      // End of Track
      else if (metaType == 0x2F) {
        // End of track
      }

      offset += metaLen;
    }
    // SysEx
    else if (status == 0xF0 || status == 0xF7) {
      uint32_t sysexLen = readVarLength(data, size, offset);
      if (offset + sysexLen > trackEnd) break;
      offset += sysexLen;
    }
    else {
      // Unknown event
      break;
    }
  }

  // Close any remaining active notes
  for (auto& [noteKey, noteData] : activeNotes) {
    auto [startTime, velocity] = noteData;
    uint8_t note = noteKey & 0xFF;
    uint8_t chan = (noteKey >> 8) & 0xFF;
    track.notes.push_back({startTime, chan, note, velocity, absTime - startTime});
  }

  m_tracks.push_back(std::move(track));
  offset = trackEnd;
  return true;
}

uint32_t MidiReader::readVarLength(const uint8_t* data, size_t size, size_t& offset) {
  uint32_t value = 0;
  uint8_t byte;

  do {
    if (offset >= size) {
      return value;
    }
    byte = data[offset++];
    value = (value << 7) | (byte & 0x7F);
  } while (byte & 0x80);

  return value;
}

uint16_t MidiReader::read16BE(const uint8_t* data, size_t offset) {
  return (data[offset] << 8) | data[offset + 1];
}

uint32_t MidiReader::read32BE(const uint8_t* data, size_t offset) {
  return (data[offset] << 24) | (data[offset + 1] << 16) | 
         (data[offset + 2] << 8) | data[offset + 3];
}
