#pragma once

#include "GTBSeq.h"
#include "GTBSeqSection.h"

class GTBMultiSectionSeq : public GTBSeq {
 public:
  GTBMultiSectionSeq(const std::string& format,
                     RawFile *file,
                     uint32_t offset,
                     uint32_t length = 0,
                     std::string name = "GTB Sequence");

  void resetVars() override;
  bool load() override;

  void addSection(GTBSeqSection *section);
  bool addLoopForeverNoItem();
  GTBSeqSection *getSectionAtOffset(uint32_t offset);

 protected:
  bool loadTracks(ReadMode readMode, uint32_t stopTime = 1000000) override;
  bool postLoad() override;
  virtual bool loadSection(GTBSeqSection *section, uint32_t stopTime = 1000000);
  virtual bool isOffsetUsed(uint32_t offset);
  virtual bool readEvent(long stopTime);

 public:
  uint32_t dwStartOffset;
  uint32_t curOffset;

  std::vector<GTBSeqSection *> aSections;

 protected:
  int foreverLoops;

 private:
  bool parseTrackPointers() override { return false; }
};
