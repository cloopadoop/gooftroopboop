/*
* VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#include <ranges>

#include "helper.h"
#include "GTBMultiSectionSeq.h"
#include "SeqEvent.h"

GTBSeqSection::GTBSeqSection(GTBMultiSectionSeq *parentFile,
                             uint32_t theOffset,
                             uint32_t theLength,
                             const std::string& name,
                             Type type)
    : GTBItem(parentFile, theOffset, theLength, name, type),
      parentSeq(parentFile) {
}

bool GTBSeqSection::load() {
  ReadMode readMode = parentSeq->readMode;

  if (readMode == READMODE_ADD_TO_UI) {
    if (!parseTrackPointers()) {
      return false;
    }
  }

  return true;
}

bool GTBSeqSection::parseTrackPointers() {
  return true;
}

bool GTBSeqSection::postLoad() {
  if (parentSeq->readMode == READMODE_ADD_TO_UI) {
    for (const auto track : aTracks) {
      track->sortChildrenByOffset();
    }
  }

  return true;
}
