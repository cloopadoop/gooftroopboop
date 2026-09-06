/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "GTBSeqNoTrks.h"
#include "Options.h"

GTBSeqNoTrks::GTBSeqNoTrks(const std::string &format, RawFile *file, uint32_t offset, std::string name)
    : GTBSeq(format, file, offset, 0, std::move(name)), SeqTrack(this) {
  GTBSeqNoTrks::resetVars();
}

GTBSeqNoTrks::~GTBSeqNoTrks() = default;

void GTBSeqNoTrks::resetVars() {
  midiTracks.clear();  // no need to delete the contents, that happens when the midi is deleted
  tryExpandMidiTracks(nNumTracks);

  channel = 0;
  setCurTrack(0);

  GTBSeq::resetVars();
  SeqTrack::resetVars();
}

// LoadMain() - loads all sequence data into the class
bool GTBSeqNoTrks::load() {
  this->SeqTrack::readMode = READMODE_ADD_TO_UI;
  this->GTBSeq::readMode = READMODE_ADD_TO_UI;
  if (!parseHeader())
    return false;

  if (!loadEvents())
    return false;

  // Workaround for this GTBSeqNoTrks' multiple inheritance diamond problem. Both GTBSeq and
  // SeqTrack have their own m_children fields. GTBSeq is the one we care about. We need to transfer
  // SeqTrack::m_children into GTBSeq::m_children and then clear it from SeqTrack so that their
  // destructors don't doubly delete the children.
  SeqTrack::transferChildren(static_cast<GTBSeq*>(this));

  if (length() == 0) {
    GTBSeq::setGuessedLength();
  }

  return true;
}

bool GTBSeqNoTrks::loadEvents(long stopTime) {
  resetVars();
  if (alwaysWriteInitialTempo())
    addTempoBPMNoItem(initialTempoBPM);
  if (alwaysWriteInitialVol())
    for (int i = 0; i < 16; i++) {
      channel = i;
      addVolNoItem(initialVolume());
    }
  if (alwaysWriteInitialExpression())
    for (int i = 0; i < 16; i++) {
      channel = i;
      addExpressionNoItem(initialExpression());
    }
  if (alwaysWriteInitialReverb())
    for (int i = 0; i < 16; i++) {
      channel = i;
      addReverbNoItem(initialReverbLevel());
    }
  if (alwaysWriteInitialPitchBendRange())
    for (int i = 0; i < 16; i++) {
      channel = i;
      addPitchBendRangeNoItem(initialPitchBendRange());
    }

  bInLoop = false;
  curOffset = eventsOffset();  // start at beginning of track
  while (curOffset < rawFile()->size()) {
    if (getTime() >= static_cast<u_long>(stopTime)) {
      break;
    }

    if (!readEvent()) {
      break;
    }
  }
  return true;
}

MidiFile *GTBSeqNoTrks::convertToMidi(const GTBColl* coll) {
  this->SeqTrack::readMode = this->GTBSeq::readMode = READMODE_FIND_DELTA_LENGTH;

  useColl(coll);

  if (!loadEvents())
    return nullptr;
  if (!postLoad())
    return nullptr;

  long stopTime = totalTicks;

  MidiFile *newmidi = new MidiFile(this);
  this->midi = newmidi;
  this->SeqTrack::readMode = this->GTBSeq::readMode = READMODE_CONVERT_TO_MIDI;
  if (!loadEvents(stopTime)) {
    delete midi;
    this->midi = nullptr;
    return nullptr;
  }
  if (!postLoad()) {
    delete midi;
    this->midi = nullptr;
    return nullptr;
  }
  this->midi = nullptr;
  return newmidi;
}

MidiTrack *GTBSeqNoTrks::firstMidiTrack() {
  if (midiTracks.size() > 0) {
    return midiTracks[0];
  } else {
    return pMidiTrack;
  }
}

// checks whether or not we have already created the given number of MidiTracks.  If not, it appends
// the extra tracks. doesn't ever need to be called directly by format code, since SetCurMidiTrack
// does so automatically.
void GTBSeqNoTrks::tryExpandMidiTracks(uint32_t numTracks) {
  if (GTBSeq::readMode != READMODE_CONVERT_TO_MIDI)
    return;
  if (midiTracks.size() < numTracks) {
    size_t initialTrackSize = midiTracks.size();
    for (size_t i = initialTrackSize; i < numTracks; i++) {
      auto* midiTrack = midi->addTrack();
      midiTracks.push_back(midiTrack);
      if (i == 9 && ConversionOptions::the().skipChannel10()) {
        midiTrack->setChannelGroup(1);
        midiTrack->addMidiPort(1);
      }
    }
  }
}

void GTBSeqNoTrks::setChannel(u8 newChannel) {
  setCurTrack(newChannel);
  if (newChannel == 9 && ConversionOptions::the().skipChannel10())
    channel = 0;
  else
    channel = newChannel;
}

void GTBSeqNoTrks::setCurTrack(uint32_t trackNum) {
  if (GTBSeq::readMode != READMODE_CONVERT_TO_MIDI)
    return;

  tryExpandMidiTracks(trackNum + 1);
  pMidiTrack = midiTracks[trackNum];
}

void GTBSeqNoTrks::setTime(uint32_t newTime) {
  time = newTime;
  if (GTBSeq::readMode == READMODE_CONVERT_TO_MIDI)
    for (uint32_t i = 0; i < midiTracks.size(); i++)
      pMidiTrack->setDelta(newTime);
}

void GTBSeqNoTrks::addTime(uint32_t delta) {
  time += delta;
  if (GTBSeq::readMode == READMODE_CONVERT_TO_MIDI) {
    for (uint32_t i = 0; i < midiTracks.size(); i++)
      midiTracks[i]->addDelta(delta);
  }
}
