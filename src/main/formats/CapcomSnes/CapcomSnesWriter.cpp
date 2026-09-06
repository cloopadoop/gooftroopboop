/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "CapcomSnesWriter.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <limits>

CapcomSnesWriter::CapcomSnesWriter() {}

namespace {

uint8_t NormalizeProgram(uint8_t program, const CapcomSnesWriter::WriterConfig& config) {
  uint8_t mappedProgram = program;
  if (!config.programMap.empty() && mappedProgram < config.programMap.size()) {
    mappedProgram = config.programMap[mappedProgram];
  }

  if (config.enforceProgramLimit && mappedProgram > config.maxProgram) {
    return config.defaultProgram;
  }

  return mappedProgram;
}

}  // namespace

bool CapcomSnesWriter::convertFromMidi(const MidiReader& midi, const WriterConfig& config) {
  m_config = config;
  m_warnings.clear();
  m_errorMessage.clear();
  m_sequenceData.clear();
  m_tracks.clear();

  // Step 1: Quantize MIDI to SEQ_PPQN (48)
  if (!quantizeMidiToSEQPPQN(midi)) {
    return false;
  }

  // Step 2: Allocate tracks (max 8)
  if (!allocateTracks()) {
    return false;
  }

  // Step 3: Encode all tracks
  if (!encodeAllTracks()) {
    return false;
  }

  // Step 4: Write header
  if (!writeHeader()) {
    return false;
  }

  return true;
}

bool CapcomSnesWriter::quantizeMidiToSEQPPQN(const MidiReader& midi) {
  const auto& midiTracks = midi.getTracks();
  
  if (midiTracks.empty()) {
    m_errorMessage = "MIDI file contains no tracks";
    return false;
  }

  // Ensure repeated conversions start from a clean state.
  m_tempos.clear();
  double ppqnRatio = static_cast<double>(SEQ_PPQN) / midi.getPPQN();
  
  m_trackNotes.resize(midiTracks.size());
  m_trackControls.resize(midiTracks.size());
  m_trackPrograms.resize(midiTracks.size());

  // Quantize per-track events.
  for (size_t t = 0; t < midiTracks.size(); t++) {
    m_trackNotes[t].clear();
    m_trackControls[t].clear();
    m_trackPrograms[t].clear();

    std::vector<Note> tempNotes;
    
    for (const auto& note : midiTracks[t].notes) {
      Note quantNote;
      quantNote.absTime = static_cast<uint32_t>(std::round(note.absTime * ppqnRatio));
      quantNote.channel = note.channel;
      quantNote.key = note.key;
      quantNote.velocity = note.velocity;
      quantNote.duration = static_cast<uint32_t>(std::round(note.duration * ppqnRatio));
      if (quantNote.duration == 0) quantNote.duration = 1;
      tempNotes.push_back(quantNote);
    }

    std::sort(tempNotes.begin(), tempNotes.end(), [](const Note& a, const Note& b) {
      if (a.absTime != b.absTime) {
        return a.absTime < b.absTime;
      }
      if (a.key != b.key) {
        return a.key < b.key;
      }
      return a.velocity < b.velocity;
    });
    m_trackNotes[t] = std::move(tempNotes);

    // Quantize and decimate controllers (keep only significant changes)
    uint8_t lastVol = 255;
    uint8_t lastPan = 255;
    bool volWritten = false;
    bool panWritten = false;
    
    for (const auto& ctrl : midiTracks[t].controllers) {
      MidiReader::ControlChange quantCtrl = ctrl;
      quantCtrl.absTime = static_cast<uint32_t>(std::round(ctrl.absTime * ppqnRatio));
      
      // Only keep controller if value changed significantly
      bool keep = false;
      if (ctrl.controller == 7) {  // Volume
        if (m_config.stripAutomation) {
          // Keep only first volume
          keep = !volWritten;
          volWritten = true;
        } else {
          if (lastVol == 255 || std::abs((int)ctrl.value - (int)lastVol) >= 4) {
            keep = true;
            lastVol = ctrl.value;
          }
        }
      }
      else if (ctrl.controller == 10) {  // Pan
        if (m_config.stripAutomation) {
          // Keep only first pan
          keep = !panWritten;
          panWritten = true;
        } else {
          if (lastPan == 255 || std::abs((int)ctrl.value - (int)lastPan) >= 8) {
            keep = true;
            lastPan = ctrl.value;
          }
        }
      }
      else {
        keep = true;  // Keep other controllers
      }
      
      if (keep) {
        m_trackControls[t].push_back(quantCtrl);
      }
    }
    std::sort(m_trackControls[t].begin(), m_trackControls[t].end(),
      [](const auto& a, const auto& b) {
        if (a.absTime != b.absTime) {
          return a.absTime < b.absTime;
        }
        if (a.controller != b.controller) {
          return a.controller < b.controller;
        }
        return a.value < b.value;
      });

    // Quantize programs  
    uint8_t lastProg = 255;
    for (const auto& prog : midiTracks[t].programs) {
      MidiReader::ProgramChange quantProg = prog;
      quantProg.absTime = static_cast<uint32_t>(std::round(prog.absTime * ppqnRatio));
      
      // Strip automation: keep only first program change per track
      if (m_config.stripAutomation) {
        if (lastProg == 255) {
          m_trackPrograms[t].push_back(quantProg);
          lastProg = prog.program;
        }
      } else {
        // Keep program changes when they differ
        if (prog.program != lastProg) {
          m_trackPrograms[t].push_back(quantProg);
          lastProg = prog.program;
        }
      }
    }
    std::sort(m_trackPrograms[t].begin(), m_trackPrograms[t].end(),
      [](const auto& a, const auto& b) {
        if (a.absTime != b.absTime) {
          return a.absTime < b.absTime;
        }
        return a.program < b.program;
      });

    auto resolveTrackProgramForNote = [&](const Note& note) -> uint8_t {
      const auto& programs = m_trackPrograms[t];
      if (programs.empty()) {
        return m_config.defaultProgram;
      }

      // Prefer program changes on the same MIDI channel as the note.
      bool haveChannelProgram = false;
      uint8_t channelProgram = programs.front().program;
      bool haveAnyProgram = false;
      uint8_t anyProgram = programs.front().program;

      for (const auto& prog : programs) {
        if (prog.absTime > note.absTime) {
          break;
        }

        haveAnyProgram = true;
        anyProgram = prog.program;
        if (prog.channel == note.channel) {
          haveChannelProgram = true;
          channelProgram = prog.program;
        }
      }

      if (haveChannelProgram) {
        return channelProgram;
      }
      if (haveAnyProgram) {
        return anyProgram;
      }
      return programs.front().program;
    };

    for (auto& quantNote : m_trackNotes[t]) {
      quantNote.sourceProgram = resolveTrackProgramForNote(quantNote);
    }

    // Quantize tempos (collect from all tracks, will merge later)
    for (const auto& tempo : midiTracks[t].tempos) {
      MidiReader::TempoChange quantTempo = tempo;
      quantTempo.absTime = static_cast<uint32_t>(std::round(tempo.absTime * ppqnRatio));
      m_tempos.push_back(quantTempo);
    }
  }

  // Sort and compact tempos.
  std::stable_sort(m_tempos.begin(), m_tempos.end(),
    [](const auto& a, const auto& b) { return a.absTime < b.absTime; });

  std::vector<MidiReader::TempoChange> compactTempos;
  compactTempos.reserve(m_tempos.size());
  for (const auto& tempo : m_tempos) {
    if (!compactTempos.empty() && compactTempos.back().absTime == tempo.absTime) {
      // If multiple tempo events land on the same tick, keep the last one.
      compactTempos.back() = tempo;
      continue;
    }
    if (!compactTempos.empty() &&
        compactTempos.back().microSecondsPerQuarter == tempo.microSecondsPerQuarter) {
      // Drop redundant tempo repeats at different ticks.
      continue;
    }
    compactTempos.push_back(tempo);
  }
  m_tempos = std::move(compactTempos);

  // Ensure there is an explicit initial tempo event.
  if (m_tempos.empty()) {
    m_tempos.push_back({0, 500000});  // 120 BPM default MIDI tempo
  }
  else if (m_tempos.front().absTime != 0) {
    m_tempos.insert(m_tempos.begin(), {0, m_tempos.front().microSecondsPerQuarter});
  }

  // Split polyphonic source tracks into monophonic lanes, then allocate lanes to 8 hardware tracks.
  struct VoiceLane {
    std::vector<Note> notes;
    uint32_t lastEnd = 0;
    uint64_t weight = 0;
  };

  std::vector<std::vector<VoiceLane>> splitLanes(m_trackNotes.size());
  std::vector<uint64_t> sourceTrackWeight(m_trackNotes.size(), 0);
  std::vector<size_t> noteSourceTracks;
  noteSourceTracks.reserve(m_trackNotes.size());

  for (size_t t = 0; t < m_trackNotes.size(); t++) {
    const auto& notes = m_trackNotes[t];
    if (notes.empty()) {
      continue;
    }

    noteSourceTracks.push_back(t);
    auto& lanes = splitLanes[t];

    for (const auto& note : notes) {
      size_t laneIndex = lanes.size();
      for (size_t i = 0; i < lanes.size(); i++) {
        if (note.absTime >= lanes[i].lastEnd) {
          laneIndex = i;
          break;
        }
      }

      if (laneIndex == lanes.size()) {
        lanes.push_back({});
      }

      lanes[laneIndex].notes.push_back(note);
      lanes[laneIndex].lastEnd = note.absTime + note.duration;
      lanes[laneIndex].weight += note.duration;
      sourceTrackWeight[t] += note.duration;
    }

    std::stable_sort(lanes.begin(), lanes.end(),
      [](const VoiceLane& a, const VoiceLane& b) {
        return a.weight > b.weight;
      });
  }

  if (!noteSourceTracks.empty()) {
    std::vector<size_t> selectedLaneCount(m_trackNotes.size(), 0);

    if (noteSourceTracks.size() > MAX_TRACKS) {
      std::stable_sort(noteSourceTracks.begin(), noteSourceTracks.end(),
        [&](size_t a, size_t b) {
          return sourceTrackWeight[a] > sourceTrackWeight[b];
        });

      size_t keepTracks = static_cast<size_t>(MAX_TRACKS);
      for (size_t i = 0; i < noteSourceTracks.size(); i++) {
        if (i < keepTracks) {
          selectedLaneCount[noteSourceTracks[i]] = 1;
        }
      }

      addWarning("MIDI uses more than 8 active note tracks; lower-weight tracks were dropped");
    }
    else {
      for (size_t sourceTrack : noteSourceTracks) {
        selectedLaneCount[sourceTrack] = 1;
      }

      size_t extraBudget = static_cast<size_t>(MAX_TRACKS) - noteSourceTracks.size();
      while (extraBudget > 0) {
        size_t bestTrack = m_trackNotes.size();
        uint64_t bestWeight = 0;

        for (size_t sourceTrack : noteSourceTracks) {
          const auto& lanes = splitLanes[sourceTrack];
          size_t nextLane = selectedLaneCount[sourceTrack];
          if (nextLane >= lanes.size()) {
            continue;
          }

          uint64_t laneWeight = lanes[nextLane].weight;
          if (bestTrack == m_trackNotes.size() || laneWeight > bestWeight) {
            bestTrack = sourceTrack;
            bestWeight = laneWeight;
          }
        }

        if (bestTrack == m_trackNotes.size()) {
          break;
        }

        selectedLaneCount[bestTrack]++;
        extraBudget--;
      }
    }

    auto getProgramAtTime = [&](size_t sourceTrack, uint32_t absTime, uint8_t channel) -> uint8_t {
      const auto& programs = m_trackPrograms[sourceTrack];
      if (programs.empty()) {
        return m_config.defaultProgram;
      }

      bool haveChannelProgram = false;
      uint8_t channelProgram = programs.front().program;
      bool haveAnyProgram = false;
      uint8_t anyProgram = programs.front().program;

      for (const auto& prog : programs) {
        if (prog.absTime > absTime) {
          break;
        }

        haveAnyProgram = true;
        anyProgram = prog.program;
        if (prog.channel == channel) {
          haveChannelProgram = true;
          channelProgram = prog.program;
        }
      }

      if (haveChannelProgram) {
        return channelProgram;
      }
      if (haveAnyProgram) {
        return anyProgram;
      }
      return programs.front().program;
    };

    auto mapProgram = [&](uint8_t program) -> uint8_t {
      if (!m_config.programMap.empty() && program < m_config.programMap.size()) {
        return m_config.programMap[program];
      }
      return program;
    };

    size_t expandedTracks = 0;
    size_t droppedPolyLanes = 0;
    size_t keptPolyLanes = 0;
    size_t droppedPolyNotes = 0;
    uint64_t droppedPolyTicks = 0;
    for (size_t sourceTrack : noteSourceTracks) {
      const auto& lanes = splitLanes[sourceTrack];
      size_t selected = std::min(selectedLaneCount[sourceTrack], lanes.size());
      keptPolyLanes += selected;
      if (selected > 1) {
        expandedTracks += (selected - 1);
      }
      if (lanes.size() > selected) {
        droppedPolyLanes += (lanes.size() - selected);
        for (size_t lane = selected; lane < lanes.size(); lane++) {
          droppedPolyNotes += lanes[lane].notes.size();
          droppedPolyTicks += lanes[lane].weight;
        }
      }
    }

    struct PackedTrack {
      size_t sourceTrack = 0;
      std::vector<Note> notes;
      std::vector<MidiReader::ControlChange> controls;
      std::vector<MidiReader::ProgramChange> programs;
    };

    struct DroppedVoiceNote {
      size_t sourceTrack = 0;
      uint8_t donorProgram = 0;
      Note note;
    };

    std::vector<PackedTrack> packedTracks;
    std::vector<DroppedVoiceNote> droppedVoiceNotes;
    packedTracks.reserve(MAX_TRACKS);
    droppedVoiceNotes.reserve(droppedPolyNotes);

    for (size_t sourceTrack = 0; sourceTrack < m_trackNotes.size(); sourceTrack++) {
      size_t selected = selectedLaneCount[sourceTrack];
      const auto& lanes = splitLanes[sourceTrack];
      size_t laneCount = std::min(selected, lanes.size());
      for (size_t lane = 0; lane < laneCount; lane++) {
        if (packedTracks.size() >= MAX_TRACKS) {
          break;
        }
        PackedTrack host;
        host.sourceTrack = sourceTrack;
        host.notes = lanes[lane].notes;
        host.controls = m_trackControls[sourceTrack];
        host.programs = m_trackPrograms[sourceTrack];
        packedTracks.push_back(std::move(host));
      }

      for (size_t lane = laneCount; lane < lanes.size(); lane++) {
        for (const auto& note : lanes[lane].notes) {
          const uint8_t donorProgram =
              (note.sourceProgram != 0xFF)
                  ? note.sourceProgram
                  : getProgramAtTime(sourceTrack, note.absTime, note.channel);
          droppedVoiceNotes.push_back({sourceTrack, donorProgram, note});
        }
      }
    }

    size_t recoveredPackedNotes = 0;
    uint64_t recoveredPackedTicks = 0;
    size_t recoveredPackedProgramEvents = 0;

    auto tryFindInsertion = [](const std::vector<Note>& hostNotes,
                               const Note& candidate,
                               size_t& insertPos,
                               uint32_t& gapWaste) -> bool {
      if (candidate.duration <= 1) {
        return false;
      }

      const uint32_t candidateStart = candidate.absTime;
      const uint32_t candidateEnd = candidate.absTime + candidate.duration;
      auto it = std::lower_bound(hostNotes.begin(), hostNotes.end(), candidateStart,
                                 [](const Note& a, uint32_t start) {
                                   return a.absTime < start;
                                 });

      uint32_t prevEnd = 0;
      if (it != hostNotes.begin()) {
        const auto& prev = *(it - 1);
        prevEnd = prev.absTime + prev.duration;
        if (prevEnd > candidateStart) {
          return false;
        }
      }

      uint32_t nextStart = candidateEnd;
      if (it != hostNotes.end()) {
        nextStart = it->absTime;
        if (candidateEnd > nextStart) {
          return false;
        }
      }

      if (candidateStart < prevEnd || candidateEnd > nextStart) {
        return false;
      }

      insertPos = static_cast<size_t>(std::distance(hostNotes.begin(), it));
      uint32_t span = nextStart - prevEnd;
      if (span < candidate.duration) {
        return false;
      }
      gapWaste = span - candidate.duration;
      return true;
    };

    std::stable_sort(droppedVoiceNotes.begin(), droppedVoiceNotes.end(),
      [](const DroppedVoiceNote& a, const DroppedVoiceNote& b) {
        if (a.note.duration != b.note.duration) {
          return a.note.duration > b.note.duration;
        }
        if (a.note.absTime != b.note.absTime) {
          return a.note.absTime < b.note.absTime;
        }
        return a.note.key < b.note.key;
      });

    for (const auto& dropped : droppedVoiceNotes) {
      size_t bestHost = packedTracks.size();
      size_t bestInsertPos = 0;
      int bestSwapCost = std::numeric_limits<int>::max();
      int bestSourcePenalty = std::numeric_limits<int>::max();
      uint32_t bestGapWaste = std::numeric_limits<uint32_t>::max();

      for (size_t hostIndex = 0; hostIndex < packedTracks.size(); hostIndex++) {
        auto& host = packedTracks[hostIndex];

        size_t insertPos = 0;
        uint32_t gapWaste = 0;
        if (!tryFindInsertion(host.notes, dropped.note, insertPos, gapWaste)) {
          continue;
        }

        const uint8_t hostProgramStart =
            getProgramAtTime(host.sourceTrack, dropped.note.absTime, dropped.note.channel);
        const uint8_t hostProgramEnd =
            getProgramAtTime(host.sourceTrack, dropped.note.absTime + dropped.note.duration, dropped.note.channel);
        const uint8_t donorProgramMapped = mapProgram(dropped.donorProgram);
        const uint8_t hostProgramStartMapped = mapProgram(hostProgramStart);
        const uint8_t hostProgramEndMapped = mapProgram(hostProgramEnd);
        const int swapCost = static_cast<int>(hostProgramStartMapped != donorProgramMapped)
                             + static_cast<int>(hostProgramEndMapped != donorProgramMapped);
        if (!m_config.gapPackAllowProgramSwaps && swapCost > 0) {
          continue;
        }
        const int sourcePenalty = (host.sourceTrack == dropped.sourceTrack) ? 0 : 1;

        const bool better =
          (swapCost < bestSwapCost) ||
          (swapCost == bestSwapCost && sourcePenalty < bestSourcePenalty) ||
          (swapCost == bestSwapCost && sourcePenalty == bestSourcePenalty && gapWaste < bestGapWaste);

        if (better) {
          bestHost = hostIndex;
          bestInsertPos = insertPos;
          bestSwapCost = swapCost;
          bestSourcePenalty = sourcePenalty;
          bestGapWaste = gapWaste;
        }
      }

      if (bestHost == packedTracks.size()) {
        continue;
      }

      auto& host = packedTracks[bestHost];
      host.notes.insert(host.notes.begin() + static_cast<std::ptrdiff_t>(bestInsertPos), dropped.note);

      recoveredPackedNotes++;
      recoveredPackedTicks += dropped.note.duration;
    }

    std::vector<std::vector<Note>> reassignedNotes;
    std::vector<std::vector<MidiReader::ControlChange>> reassignedControls;
    std::vector<std::vector<MidiReader::ProgramChange>> reassignedPrograms;
    reassignedNotes.reserve(packedTracks.size());
    reassignedControls.reserve(packedTracks.size());
    reassignedPrograms.reserve(packedTracks.size());

    for (auto& host : packedTracks) {
      std::stable_sort(host.notes.begin(), host.notes.end(),
        [](const Note& a, const Note& b) {
          if (a.absTime != b.absTime) {
            return a.absTime < b.absTime;
          }
          if (a.key != b.key) {
            return a.key < b.key;
          }
          return a.velocity < b.velocity;
        });

      std::stable_sort(host.programs.begin(), host.programs.end(),
        [](const auto& a, const auto& b) {
          return a.absTime < b.absTime;
        });

      auto lastProgram = std::unique(host.programs.begin(), host.programs.end(),
        [](const auto& a, const auto& b) {
          return a.absTime == b.absTime && a.program == b.program;
        });
      host.programs.erase(lastProgram, host.programs.end());

      reassignedNotes.push_back(std::move(host.notes));
      reassignedControls.push_back(std::move(host.controls));
      reassignedPrograms.push_back(std::move(host.programs));
    }

    if (!reassignedNotes.empty()) {
      m_trackNotes = std::move(reassignedNotes);
      m_trackControls = std::move(reassignedControls);
      m_trackPrograms = std::move(reassignedPrograms);
    }

    if (expandedTracks > 0) {
      addWarning("Polyphony split expanded source tracks across additional SNES channels (kept lanes: "
                 + std::to_string(keptPolyLanes) + ")");
    }

    if (recoveredPackedNotes > 0) {
      addWarning("Gap packing recovered dropped voices (recovered notes: "
                 + std::to_string(recoveredPackedNotes)
                 + ", recovered ticks: " + std::to_string(recoveredPackedTicks)
                 + ", inserted program events: " + std::to_string(recoveredPackedProgramEvents) + ")");
    }

    if (droppedPolyLanes > 0) {
      addWarning("Some polyphonic voices were dropped due to the 8-channel SNES limit (dropped lanes: "
                 + std::to_string(droppedPolyLanes)
                 + ", dropped notes: " + std::to_string(droppedPolyNotes - recoveredPackedNotes)
                 + ", dropped ticks: " + std::to_string(droppedPolyTicks - recoveredPackedTicks) + ")");
    }
  }
  
  return true;
}

bool CapcomSnesWriter::allocateTracks() {
  // Allocate all 8 tracks (even if empty) to preserve track indices
  m_tracks.resize(MAX_TRACKS);
  
  if (m_trackNotes.size() > MAX_TRACKS) {
    addWarning("MIDI has more than 8 tracks; only first 8 will be used");
  }

  return true;
}

bool CapcomSnesWriter::encodeAllTracks() {
  for (size_t i = 0; i < m_tracks.size(); i++) {
    if (!encodeTrack(i)) {
      return false;
    }
  }
  return true;
}

bool CapcomSnesWriter::encodeTrack(int trackIndex) {
  std::vector<uint8_t>& out = m_tracks[trackIndex].data;

  static const std::vector<Note> kEmptyNotes;
  static const std::vector<MidiReader::ControlChange> kEmptyControls;
  static const std::vector<MidiReader::ProgramChange> kEmptyPrograms;

  const auto& notes =
      (trackIndex < static_cast<int>(m_trackNotes.size())) ? m_trackNotes[trackIndex] : kEmptyNotes;
  const auto& controls =
      (trackIndex < static_cast<int>(m_trackControls.size())) ? m_trackControls[trackIndex] : kEmptyControls;
  const auto& programs =
      (trackIndex < static_cast<int>(m_trackPrograms.size())) ? m_trackPrograms[trackIndex] : kEmptyPrograms;
  
  // Skip empty tracks entirely
  bool hasTempos = (trackIndex == 0 && !m_tempos.empty());
  if (notes.empty() && controls.empty() && programs.empty() && !hasTempos) {
    // Capcom V1 commonly uses valid pointers for all 8 channels.
    // Emit an explicit empty track so header pointers stay non-zero.
    write8(out, EVENT_END);
    return true;
  }
  
  TrackState state;
  state.channel = trackIndex;
  state.octave = 4;
  state.octaveUp = false;
  state.tripletMode = false;
  state.slurMode = false;
  state.durationRate = m_config.initialDurationRate;
  state.volume = 0xFF;  // Invalid sentinel to force explicit first volume write.
  state.pan = 0;  // Capcom default (center after conversion)
  state.program = 0xFF;  // Invalid program to force first emission
  state.transpose = 0;
  state.tuning = 0;
  uint16_t lastTempo = 0xFFFF;

  // Emit explicit starting tempo for every active track.
  // This prevents engine/default tempo drift when only one track carries tempo events.
  if (!m_tempos.empty() && (!notes.empty() || !controls.empty() || !programs.empty())) {
    double startBpm = m_tempos.front().getBPM();
    uint16_t capcomTempo = BPMToTempo(startBpm);
    write8(out, EVENT_TEMPO);
    write16BE(out, capcomTempo);
    lastTempo = capcomTempo;
  }

  // Emit a deterministic initial program for tracks that contain notes.
  // Some MIDI files rely on DAW defaults and omit explicit program changes.
  if (!notes.empty()) {
    uint8_t initialProgram = m_config.defaultProgram;
    if (!programs.empty()) {
      initialProgram = NormalizeProgram(programs.front().program, m_config);
    } else {
      initialProgram = NormalizeProgram(initialProgram, m_config);
    }
    write8(out, EVENT_PROGRAM_CHANGE);
    write8(out, initialProgram);
    state.program = initialProgram;
    emitProgramPitchCompensation(initialProgram, state, out);

    // Always emit an initial channel volume so ROM playback does not depend on
    // previous-song DSP state when MIDI has no CC7 automation.
    uint8_t initialMidiVolume = notes.front().velocity;
    if (initialMidiVolume == 0) {
      initialMidiVolume = 100;
    }
    for (const auto& ctrl : controls) {
      if (ctrl.controller == 7 && ctrl.absTime <= notes.front().absTime) {
        initialMidiVolume = ctrl.value;
        break;
      }
    }
    uint8_t initialCapcomVolume = midiVolToCapcom(initialMidiVolume);
    write8(out, EVENT_VOLUME);
    write8(out, initialCapcomVolume);
    state.volume = initialCapcomVolume;

    // Emit explicit initial octave to avoid relying on implicit engine state.
    write8(out, EVENT_OCTAVE);
    write8(out, state.octave);
  }

  // Merge all events by time
  struct Event {
    enum Type { NOTE, CONTROL, PROGRAM, TEMPO } type;
    uint32_t absTime;
    size_t index;  // Index into respective array
  };

  std::vector<Event> events;
  
  for (size_t i = 0; i < notes.size(); i++) {
    events.push_back({Event::NOTE, notes[i].absTime, i});
  }
  for (size_t i = 0; i < controls.size(); i++) {
    events.push_back({Event::CONTROL, controls[i].absTime, i});
  }
  for (size_t i = 0; i < programs.size(); i++) {
    events.push_back({Event::PROGRAM, programs[i].absTime, i});
  }
  // Tempo on track 0 only
  if (trackIndex == 0) {
    for (size_t i = 0; i < m_tempos.size(); i++) {
      events.push_back({Event::TEMPO, m_tempos[i].absTime, i});
    }
  }

  std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
    if (a.absTime != b.absTime) {
      return a.absTime < b.absTime;
    }
    const auto priority = [](Event::Type t) {
      switch (t) {
        case Event::TEMPO: return 0;
        case Event::CONTROL: return 1;
        case Event::PROGRAM: return 2;
        case Event::NOTE: return 3;
      }
      return 4;
    };
    // Ensure control state is established before note emission at the same tick.
    return priority(a.type) < priority(b.type);
  });

  // Track state for deduplication
  uint32_t currentTime = 0;
  bool durationWritten = false;

  for (const auto& event : events) {
    if (event.absTime > currentTime) {
      uint32_t gapTicks = event.absTime - currentTime;
      if (gapTicks > 1) {
        emitRest(gapTicks, state, out);
      }
      // 1 tick cannot be represented by Capcom note lengths; skip such tiny gaps.
      currentTime = event.absTime;
    }

    switch (event.type) {
      case Event::NOTE: {
        const auto& note = notes[event.index];

        uint32_t noteTicks = note.duration;
        if (event.absTime < currentTime) {
          // Track format is monophonic; if MIDI overlaps notes, keep timing anchored by
          // shrinking late notes so their end times stay aligned.
          uint32_t overlap = currentTime - event.absTime;
          if (overlap >= noteTicks) {
            // Note is fully masked by an already-sounding note.
            break;
          }
          noteTicks -= overlap;
        }

        // Capcom note lengths cannot represent 1 tick.
        if (noteTicks <= 1) {
          break;
        }
        
        // Emit duration on first note if not yet written
        if (!durationWritten) {
          write8(out, EVENT_DURATION);
          write8(out, state.durationRate);
          durationWritten = true;
        }

        // Keep packed notes on their original MIDI source instrument mapping.
        // This avoids host-lane program context bleeding into recovered notes.
        if (note.sourceProgram != 0xFF) {
          uint8_t sourceInstrNum = NormalizeProgram(note.sourceProgram, m_config);
          if (sourceInstrNum != state.program) {
            write8(out, EVENT_PROGRAM_CHANGE);
            write8(out, sourceInstrNum);
            state.program = sourceInstrNum;
            emitProgramPitchCompensation(sourceInstrNum, state, out);
          }
        }
        
        emitNote(note.key, noteTicks, state, out);
        // Keep timeline monotonic; overlapping notes must not move time backward.
        uint32_t noteEnd = currentTime + noteTicks;
        if (noteEnd > currentTime) {
          currentTime = noteEnd;
        }
        break;
      }

      case Event::CONTROL: {
        const auto& ctrl = controls[event.index];
        if (ctrl.controller == 7) {  // Volume
          uint8_t capcomVol = midiVolToCapcom(ctrl.value);
          if (capcomVol != state.volume) {
            write8(out, EVENT_VOLUME);
            write8(out, capcomVol);
            state.volume = capcomVol;
          }
        }
        else if (ctrl.controller == 10) {  // Pan
          int8_t capcomPan = midiPanToCapcom(ctrl.value);
          if (capcomPan != state.pan) {
            write8(out, EVENT_PAN);
            write8(out, static_cast<uint8_t>(capcomPan));
            state.pan = capcomPan;
          }
        }
        break;
      }

      case Event::PROGRAM: {
        const auto& prog = programs[event.index];
        uint8_t instrNum = NormalizeProgram(prog.program, m_config);
        if (instrNum != state.program) {
          write8(out, EVENT_PROGRAM_CHANGE);
          write8(out, instrNum);
          state.program = instrNum;
          emitProgramPitchCompensation(instrNum, state, out);
        }
        break;
      }

      case Event::TEMPO: {
        const auto& tempo = m_tempos[event.index];
        double bpm = tempo.getBPM();
        uint16_t capcomTempo = BPMToTempo(bpm);
        if (capcomTempo != lastTempo) {
          write8(out, EVENT_TEMPO);
          write16BE(out, capcomTempo);
          lastTempo = capcomTempo;
        }
        break;
      }
    }
  }

  // End of track
  if (m_config.loopStartTick >= 0 && trackIndex == 0) {
    // TODO: Add loop support
    addWarning("Loop support not yet implemented");
  }
  
  write8(out, EVENT_END);

  return true;
}

void CapcomSnesWriter::emitRest(uint32_t ticks, TrackState& state, std::vector<uint8_t>& out) {
  // Rest is encoded as a note with keyIndex=0
  while (ticks > 0) {
    bool useDotted = false;
    bool useTriplet = false;
    uint8_t lenIndex = getLengthIndex(ticks, useDotted, useTriplet);
    
    uint32_t lenTicks = 192 >> (7 - lenIndex);
    if (useDotted) lenTicks = lenTicks + lenTicks / 2;
    if (useTriplet) lenTicks = lenTicks * 2 / 3;

    if (useDotted) {
      write8(out, EVENT_DOTTED_NOTE_ON);
    }
    
    // Only toggle triplet if state changed
    if (useTriplet != state.tripletMode) {
      write8(out, EVENT_TOGGLE_TRIPLET);
      state.tripletMode = useTriplet;
    }

    // keyIndex = 0 means rest
    uint8_t noteEvent = (lenIndex << 5) | 0;
    write8(out, noteEvent);

    ticks -= std::min(ticks, lenTicks);
  }
}

void CapcomSnesWriter::emitNote(uint8_t key, uint32_t ticks, TrackState& state, std::vector<uint8_t>& out) {
  // Calculate octave and key index
  uint8_t octave = key / 12;
  uint8_t keyInOctave = key % 12;
  bool needOctaveUp = false;

  if (octave >= 8) {
    needOctaveUp = true;
    octave = (key - 24) / 12;
    keyInOctave = (key - 24) % 12;
  }

  octave = std::min(octave, uint8_t(7));
  
  // Set octave if changed
  if (octave != state.octave) {
    write8(out, EVENT_OCTAVE);
    write8(out, octave);
    state.octave = octave;
  }

  // Set octave-up flag if changed
  if (needOctaveUp != state.octaveUp) {
    write8(out, EVENT_TOGGLE_OCTAVE_UP);
    state.octaveUp = needOctaveUp;
  }

  // Emit note(s) to cover duration
  while (ticks > 0) {
    bool useDotted = false;
    bool useTriplet = false;
    uint8_t lenIndex = getLengthIndex(ticks, useDotted, useTriplet);
    
    uint32_t lenTicks = 192 >> (7 - lenIndex);
    if (useDotted) lenTicks = lenTicks + lenTicks / 2;
    if (useTriplet) lenTicks = lenTicks * 2 / 3;

    if (useDotted) {
      write8(out, EVENT_DOTTED_NOTE_ON);
    }
    if (useTriplet != state.tripletMode) {
      write8(out, EVENT_TOGGLE_TRIPLET);
      state.tripletMode = useTriplet;
    }

    // keyIndex: 1-31 for notes (keyInOctave + 1)
    uint8_t keyIndex = keyInOctave + 1;
    if (keyIndex > 31) keyIndex = 31;  // Clamp
    
    uint8_t noteEvent = (lenIndex << 5) | keyIndex;
    write8(out, noteEvent);

    ticks -= std::min(ticks, lenTicks);
  }
}

uint8_t CapcomSnesWriter::getLengthIndex(uint32_t ticks, bool& useDotted, bool& useTriplet) {
  useDotted = false;
  useTriplet = false;

  // Length table: L = 192 >> (7 - lenIndex)
  // NOTE: Capcom note events are status bytes >= 0x20,
  // so lenIndex 0 is not usable for note/rest encoding.
  // lenIndex 1: 3 ticks
  // lenIndex 2: 6 ticks
  // lenIndex 3: 12 ticks
  // lenIndex 4: 24 ticks
  // lenIndex 5: 48 ticks
  // lenIndex 6: 96 ticks
  // lenIndex 7: 192 ticks

  if (ticks <= 1) {
    // Smallest representable length is triplet(lenIndex=1) == 2 ticks.
    useTriplet = true;
    return 1;
  }

  // Try to find exact match first
  for (int i = 7; i >= 1; i--) {
    uint32_t len = 192 >> (7 - i);
    if (ticks == len) {
      return i;
    }
    
    // Try dotted (adds 50%)
    uint32_t dottedLen = len + len / 2;
    if (ticks == dottedLen && len % 2 == 0 && len < 128) {
      useDotted = true;
      return i;
    }

    // Try triplet (multiply by 2/3)
    uint32_t tripletLen = len * 2 / 3;
    if (ticks == tripletLen) {
      useTriplet = true;
      return i;
    }
  }

  // No exact match, find closest smaller representable length.
  uint8_t bestIndex = 1;
  uint32_t bestLen = 0;
  bool bestDotted = false;
  bool bestTriplet = false;

  for (int i = 7; i >= 1; i--) {
    uint32_t len = 192 >> (7 - i);
    if (len <= ticks) {
      if (len > bestLen) {
        bestLen = len;
        bestIndex = static_cast<uint8_t>(i);
        bestDotted = false;
        bestTriplet = false;
      }
    }

    if (len % 2 == 0 && len < 128) {
      uint32_t dottedLen = len + len / 2;
      if (dottedLen <= ticks && dottedLen > bestLen) {
        bestLen = dottedLen;
        bestIndex = static_cast<uint8_t>(i);
        bestDotted = true;
        bestTriplet = false;
      }
    }

    uint32_t tripletLen = len * 2 / 3;
    if (tripletLen <= ticks && tripletLen > bestLen) {
      bestLen = tripletLen;
      bestIndex = static_cast<uint8_t>(i);
      bestDotted = false;
      bestTriplet = true;
    }
  }

  useDotted = bestDotted;
  useTriplet = bestTriplet;
  return bestIndex;
}

uint16_t CapcomSnesWriter::BPMToTempo(double bpm) {
  // BPM = 60000000.0 / (SEQ_PPQN * (125 * 0x40) * 2) * (tempo / 256.0)
  // Solving for tempo:
  // tempo = BPM * SEQ_PPQN * (125 * 0x40) * 2 * 256 / 60000000.0
  // tempo = BPM * 48 * 8000 * 2 * 256 / 60000000.0
  // tempo = BPM * 3.2768
  
  double tempo = bpm * 3.2768;
  return static_cast<uint16_t>(std::round(tempo));
}

uint8_t CapcomSnesWriter::midiVolToCapcom(uint8_t midiVol) {
  // V1 format uses linear volume: capcomVol = midiVol * 2
  // Clamp to 0-255
  uint16_t vol = static_cast<uint16_t>(midiVol) * 2;
  return static_cast<uint8_t>(std::min(vol, uint16_t(255)));
}

int8_t CapcomSnesWriter::midiPanToCapcom(uint8_t midiPan) {
  // V1 format uses linear pan
  // MIDI pan: 0 (left) to 127 (right), center = 64
  // Capcom pan: signed -128 to 127, stored as unsigned after conversion
  // Formula: signedPan = (midiPan << 1) - 128
  
  int16_t pan = (static_cast<int16_t>(midiPan) << 1) - 128;
  pan = std::max(int16_t(-128), std::min(int16_t(127), pan));
  return static_cast<int8_t>(pan);
}

int8_t CapcomSnesWriter::programTransposeFor(uint8_t program) const {
  if (!m_config.programTransposeMap.empty() && program < m_config.programTransposeMap.size()) {
    return m_config.programTransposeMap[program];
  }
  return 0;
}

int8_t CapcomSnesWriter::programTuningFor(uint8_t program) const {
  if (!m_config.programTuningMap.empty() && program < m_config.programTuningMap.size()) {
    return m_config.programTuningMap[program];
  }
  return 0;
}

void CapcomSnesWriter::emitProgramPitchCompensation(uint8_t program, TrackState& state, std::vector<uint8_t>& out) {
  const int8_t targetTranspose = programTransposeFor(program);
  if (targetTranspose != state.transpose) {
    write8(out, EVENT_TRANSPOSE);
    write8(out, static_cast<uint8_t>(targetTranspose));
    state.transpose = targetTranspose;
  }

  const int8_t targetTuning = programTuningFor(program);
  if (targetTuning != state.tuning) {
    write8(out, EVENT_TUNING);
    write8(out, static_cast<uint8_t>(targetTuning));
    state.tuning = targetTuning;
  }
}

bool CapcomSnesWriter::writeHeader() {
  uint16_t headerSize = (m_config.priorityInHeader ? 1 : 0) + MAX_TRACKS * 2;
  uint16_t currentAddr = m_config.baseAddress + headerSize;

  // V1 stores header entries in track order 7->0.
  // To keep pointers ascending and compatible with original data layout,
  // pack the track data in that same order.
  for (int i = MAX_TRACKS - 1; i >= 0; i--) {
    if (i < static_cast<int>(m_tracks.size()) && !m_tracks[i].data.empty()) {
      m_tracks[i].address = currentAddr;
      currentAddr += static_cast<uint16_t>(m_tracks[i].data.size());
    }
    else if (i < static_cast<int>(m_tracks.size())) {
      m_tracks[i].address = 0;
    }
  }

  // Build header
  std::vector<uint8_t> header;
  
  if (m_config.priorityInHeader) {
    write8(header, m_config.priority);
  }

  // Write track pointers (8 tracks, BE) in REVERSE order (7 down to 0).
  // Pointers are absolute ARAM addresses for Goof Troop/Capcom V1.
  for (int i = MAX_TRACKS - 1; i >= 0; i--) {
    if (i < static_cast<int>(m_tracks.size()) && !m_tracks[i].data.empty()) {
      write16BE(header, m_tracks[i].address);
    } else {
      write16BE(header, 0);  // Unused/empty track
    }
  }

  // Assemble final sequence data
  m_sequenceData = header;
  for (int i = MAX_TRACKS - 1; i >= 0; i--) {
    if (i < static_cast<int>(m_tracks.size()) && !m_tracks[i].data.empty()) {
      const auto& track = m_tracks[i];
      m_sequenceData.insert(m_sequenceData.end(), track.data.begin(), track.data.end());
    }
  }

  return true;
}

bool CapcomSnesWriter::writeToFile(const std::string& filepath) {
  std::ofstream file(filepath, std::ios::binary);
  if (!file.is_open()) {
    m_errorMessage = "Failed to open file for writing: " + filepath;
    return false;
  }

  file.write(reinterpret_cast<const char*>(m_sequenceData.data()), m_sequenceData.size());
  
  if (!file.good()) {
    m_errorMessage = "Failed to write to file: " + filepath;
    return false;
  }

  return true;
}

void CapcomSnesWriter::write8(std::vector<uint8_t>& out, uint8_t value) {
  out.push_back(value);
}

void CapcomSnesWriter::write16BE(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back((value >> 8) & 0xFF);
  out.push_back(value & 0xFF);
}

void CapcomSnesWriter::addWarning(const std::string& msg) {
  m_warnings.push_back(msg);
}



