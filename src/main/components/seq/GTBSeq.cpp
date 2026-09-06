/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include <climits>
#include <ranges>
#include <algorithm>
#undef min
#undef max

#include "GTBSeq.h"
#include "SeqEvent.h"
#include "SeqSlider.h"
#include "Options.h"
#include "Root.h"
#include "Format.h"
#include "helper.h"

GTBSeq::GTBSeq(const std::string &format, RawFile *file, uint32_t offset, uint32_t length, std::string name)
    : GTBFile(format, file, offset, length, std::move(name)),
      midi(nullptr),
      nNumTracks(0),
      readMode(READMODE_ADD_TO_UI),
      time(0),
      m_use_monophonic_tracks(false),
      m_use_linear_amplitude_scale(false),
      m_use_linear_pan_amplitude_scale(false),
      m_always_write_initial_tempo(false),
      m_always_write_initial_vol(false),
      m_always_write_initial_expression(false),
      m_always_write_initial_reverb(false),
      m_always_write_initial_pitch_bend_range(false),
      m_always_write_initial_mono_mode(false),
      m_allow_discontinuous_track_data(false),
      bLoadTickByTick(false),
      bIncTickAfterProcessingTracks(true),
      m_initial_volume(100),                    // GM standard (dls1 spec p16)
      m_initial_expression(127),             //''
      m_initial_reverb_level(40),                  // GM standard
      m_initial_pitch_bend_range_cents(200), // GM standard.  Means +/- 2 semitones (4 total range)
      initialTempoBPM(120),
      m_use_reverb(false),
      m_track_control_flow_state(false) {
}

GTBSeq::~GTBSeq() {
  deleteVect<ISeqSlider>(aSliders);
  delete midi;
}

bool GTBSeq::loadGTBFile(bool useMatcher) {
  if (!load()) {
    return false;
  }

  rawFile()->addContainedGTBFile(std::make_shared<std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *,
    GTBMiscFile *>>(this));
  pRoot->addGTBFile(this);

  if (useMatcher) {
    if (auto fmt = format(); fmt) {
      fmt->onNewFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>(this));
    }
  }

  return true;
}

MidiFile *GTBSeq::convertToMidi(const GTBColl* coll) {
  size_t numTracks = aTracks.size();

  if (!loadTracks(READMODE_FIND_DELTA_LENGTH)) {
      return nullptr;
  }

  useColl(coll);

  // Find the greatest length of all tracks to use as stop point for every track
  long stopTime = 0;
  for (size_t i = 0; i < numTracks; i++)
    stopTime = std::max(stopTime, aTracks[i]->totalTicks);

  auto *newmidi = new MidiFile(this);
  this->midi = newmidi;
  if (!loadTracks(READMODE_CONVERT_TO_MIDI, stopTime)) {
    delete midi;
    this->midi = nullptr;
    return nullptr;
  }
  this->midi = nullptr;
  return newmidi;
}

MidiTrack *GTBSeq::firstMidiTrack() {
  return aTracks.empty() ? nullptr : aTracks[0]->pMidiTrack;
}

bool GTBSeq::load() {
  readMode = READMODE_ADD_TO_UI;

  if (!parseHeader())
    return false;
  if (!parseTrackPointers())
    return false;
  nNumTracks = static_cast<uint32_t>(aTracks.size());
  if (nNumTracks == 0)
    return false;

  return loadTracks(readMode);
}

bool GTBSeq::postLoad() {
  if (readMode == READMODE_ADD_TO_UI) {
    std::ranges::sort(aInstrumentsUsed);

    for (auto & track : aTracks) {
      track->sortChildrenByOffset();
    }
    addChildren(aTracks);

    setGuessedLength();
    if (unLength == 0) {
      return false;
    }
  } else if (readMode == READMODE_CONVERT_TO_MIDI) {
    midi->sort();
  }

  return true;
}

bool GTBSeq::loadTracks(ReadMode readMode, uint32_t stopTime) {
  // set read mode
  this->readMode = readMode;
  for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
    aTracks[trackNum]->readMode = readMode;
  }

  // reset variables
  resetVars();
  for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
    if (!aTracks[trackNum]->loadTrackInit(trackNum, nullptr))
      return false;
  }

  loadTracksMain(stopTime);

  return postLoad();
}

void GTBSeq::loadTracksMain(uint32_t stopTime) {
  // determine the stop offsets
  uint32_t *aStopOffset = new uint32_t[nNumTracks];
  for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
    if (readMode == READMODE_ADD_TO_UI) {
      aStopOffset[trackNum] = endOffset();
      if (unLength != 0) {
        aStopOffset[trackNum] = dwOffset + unLength;
      } else {
        if (!m_allow_discontinuous_track_data) {
          // set length from the next track by offset
          for (uint32_t j = 0; j < nNumTracks; j++) {
            if (aTracks[j]->dwOffset > aTracks[trackNum]->dwOffset &&
                aTracks[j]->dwOffset < aStopOffset[trackNum]) {
              aStopOffset[trackNum] = aTracks[j]->dwOffset;
            }
          }
        }
      }
    } else {
      aStopOffset[trackNum] = aTracks[trackNum]->dwOffset + aTracks[trackNum]->unLength;
    }
  }

  // load all tracks
  if (bLoadTickByTick) {
    while (hasActiveTracks()) {
      // check time limit
      if (time >= stopTime) {
        if (readMode == READMODE_ADD_TO_UI) {
          L_WARN("{} - reached tick-by-tick stop time during load.", name());
        }

        deactivateAllTracks();
        break;
      }

      // process tracks
      for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
        if (!aTracks[trackNum]->active)
          continue;

        // tick
        aTracks[trackNum]->loadTrackMainLoop(aStopOffset[trackNum], stopTime);
      }

      // process sliders
      auto itrSlider = aSliders.begin();
      while (itrSlider != aSliders.end()) {
        auto itrNextSlider = itrSlider + 1;

        ISeqSlider *slider = *itrSlider;
        if (slider->isStarted(time)) {
          if (slider->isActive(time)) {
            slider->write(time);
          } else {
            itrNextSlider = aSliders.erase(itrSlider);
          }
        }

        itrSlider = itrNextSlider;
      }

      if (bIncTickAfterProcessingTracks == true) {
        time++;
      }
      bIncTickAfterProcessingTracks = true;
      if (readMode == READMODE_CONVERT_TO_MIDI) {
        for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
          if (aTracks.at(trackNum)->pMidiTrack != nullptr) {
            aTracks[trackNum]->pMidiTrack->setDelta(time);
          }
        }
      }

      // check loop count
      const int desiredLoopRepeats =
          (readMode == READMODE_ADD_TO_UI) ? 0 : ConversionOptions::the().numSequenceLoops();
      const int requiredPlayThroughs = desiredLoopRepeats + 1;  // include the initial playthrough
      if (foreverLoopCount() >= requiredPlayThroughs) {
        deactivateAllTracks();
        break;
      }
    }
  } else {
    uint32_t initialTime = time;  // preserve current time for multi section sequence

    // load track by track
    for (uint32_t trackNum = 0; trackNum < nNumTracks && trackNum < aTracks.size(); trackNum++) {
      time = initialTime;

      aTracks[trackNum]->loadTrackMainLoop(aStopOffset[trackNum], stopTime);
      aTracks[trackNum]->active = false;
    }
  }
  delete[] aStopOffset;
}

bool GTBSeq::hasActiveTracks() {
  for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
    if (aTracks[trackNum]->active)
      return true;
  }
  return false;
}

void GTBSeq::deactivateAllTracks() {
  for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
    aTracks[trackNum]->active = false;
  }
}

int GTBSeq::foreverLoopCount() {
  if (nNumTracks == 0)
    return 0;

  int foreverLoops = INT_MAX;
  for (uint32_t trackNum = 0; trackNum < nNumTracks; trackNum++) {
    if (!aTracks[trackNum]->active)
      continue;

    if (foreverLoops > aTracks[trackNum]->infiniteLoops)
      foreverLoops = aTracks[trackNum]->infiniteLoops;
  }
  return (foreverLoops != INT_MAX) ? foreverLoops : 0;
}

bool GTBSeq::parseHeader() {
  return true;
}

// GetTrackPointers() should contain logic for parsing track pointers
// and instantiating/adding each track in the sequence
bool GTBSeq::parseTrackPointers() {
  return true;
}

void GTBSeq::resetVars() {
  time = 0;
  tempoBPM = initialTempoBPM;

  deleteVect<ISeqSlider>(aSliders);

  if (readMode == READMODE_ADD_TO_UI) {
    aInstrumentsUsed.clear();
    m_referencedBanks.clear();
  }
}

void GTBSeq::setPPQN(uint16_t ppqn) {
  this->m_ppqn = ppqn;
  if (readMode == READMODE_CONVERT_TO_MIDI)
    midi->setPPQN(ppqn);
}

uint16_t GTBSeq::ppqn() const {
  return this->m_ppqn;
  //return midi->GetPPQN();
}

void GTBSeq::addInstrumentRef(uint32_t progNum) {
  if (std::ranges::find(aInstrumentsUsed, progNum) == aInstrumentsUsed.end()) {
    aInstrumentsUsed.push_back(progNum);
  }
}

void GTBSeq::addBankReference(uint16_t bank) {
  m_referencedBanks.insert(bank);
}

const std::set<uint16_t>& GTBSeq::referencedBanks() const {
  return m_referencedBanks;
}

bool GTBSeq::saveAsMidi(const std::string &filepath, const GTBColl* coll) {
  MidiFile *midi = this->convertToMidi(coll);
  if (!midi)
    return false;
  bool result = midi->saveMidiFile(filepath);
  delete midi;
  return result;
}
