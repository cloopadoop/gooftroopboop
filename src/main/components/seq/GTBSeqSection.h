#pragma once

#include "GTBItem.h"
#include "SeqTrack.h"

class GTBMultiSectionSeq;

class GTBSeqSection
    : public GTBItem {
 public:
  GTBSeqSection(GTBMultiSectionSeq *parentFile,
                uint32_t theOffset,
                uint32_t theLength = 0,
                const std::string& name = "Section",
                Type type = Type::Header);

  virtual bool load();
  virtual bool parseTrackPointers();
  virtual bool postLoad();

  GTBMultiSectionSeq *parentSeq;
  std::vector<SeqTrack *> aTracks;
};
