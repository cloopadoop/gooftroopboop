/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once
#include "GTBSeq.h"
#include "SeqTrack.h"
#include "SeqEvent.h"

class GTBSeqNoTrks : public GTBSeq, public SeqTrack {
public:
  GTBSeqNoTrks(const std::string &format, RawFile *file, uint32_t offset,
               std::string name = "GTB Sequence");

public:
  ~GTBSeqNoTrks() override;

  void resetVars() override;

  using GTBSeq::readBytes;
  using GTBSeq::readByte;
  using GTBSeq::readShort;
  using GTBSeq::readWord;
  using GTBSeq::readShortBE;
  using GTBSeq::readWordBE;
  [[nodiscard]] inline uint32_t &offset() { return GTBSeq::dwOffset; }
  [[nodiscard]] inline uint32_t &length() { return GTBSeq::unLength; }
  [[nodiscard]] inline std::string name() { return GTBSeq::name(); }

  [[nodiscard]] inline RawFile* rawFile() { return GTBSeq::rawFile(); }

  [[nodiscard]] inline uint32_t &eventsOffset() { return dwEventsOffset; }

  // this function must be called in GetHeaderInfo or before LoadEvents is called
  inline void setEventsOffset(uint32_t offset) {
    dwEventsOffset = offset;
    if (SeqTrack::readMode == READMODE_ADD_TO_UI) {
      SeqTrack::dwOffset = offset;
    }
  }

  void setTime(uint32_t newTime) override;
  void addTime(uint32_t delta) override;

  void setChannel(u8 newChannel);
  void tryExpandMidiTracks(uint32_t numTracks);

  bool load() override;  // Function to load all the information about the sequence
  virtual bool loadEvents(long stopTime = 1000000);
  MidiFile *convertToMidi(const GTBColl* coll) override;
  MidiTrack *firstMidiTrack() override;

  uint32_t dwEventsOffset;

protected:
  void setCurTrack(uint32_t trackNum);

  // an array of midi tracks... we will change pMidiTrack, which all the SeqTrack functions write
  // to, to the corresponding MidiTrack in this vector before we write every event
  std::vector<MidiTrack *> midiTracks;
};
