/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#include "GTBSampColl.h"

#include "GTBSamp.h"
#include "Root.h"
#include "Format.h"
#include "helper.h"

// ***********
// GTBSampColl
// ***********

GTBSampColl::GTBSampColl(const std::string &format, RawFile *rawfile, uint32_t offset, uint32_t length,
                         std::string theName)
    : GTBFile(format, rawfile, offset, length, std::move(theName)),
      m_should_load_on_instr_set_match(false),
      bLoaded(false),
      sampDataOffset(0),
      parInstrSet(nullptr) {
}

GTBSampColl::GTBSampColl(const std::string &format, RawFile *rawfile, GTBInstrSet *instrset,
                         uint32_t offset, uint32_t length, std::string theName)
    : GTBFile(format, rawfile, offset, length, std::move(theName)),
      m_should_load_on_instr_set_match(false),
      bLoaded(false),
      sampDataOffset(0),
      parInstrSet(instrset) {
}

bool GTBSampColl::loadGTBFile(bool useMatcher) {
  bool val = load();
  if (!val) {
    return false;
  }

  if (useMatcher) {
    if (auto fmt = format(); fmt) {
      fmt->onNewFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>(this));
    }
  }

  return val;
}


bool GTBSampColl::load() {
  if (bLoaded)
    return true;
  if (!parseHeader())
    return false;
  if (!parseSampleInfo())
    return false;

  if (samples.size() == 0)
    return false;

  addChildren(samples);

  if (unLength == 0) {
    for (std::vector<GTBSamp *>::iterator itr = samples.begin(); itr != samples.end(); ++itr) {
      GTBSamp *samp = *itr;

      // Some formats can have negative sample offset
      // For example, Konami's SNES format and Hudson's SNES format
      // TODO: Fix negative sample offset without breaking instrument
      //assert(dwOffset <= samp->dwOffset);

      //if (dwOffset > samp->dwOffset)
      //{
      //	unLength += samp->dwOffset - dwOffset;
      //	dwOffset = samp->dwOffset;
      //}

      if (dwOffset + unLength < samp->dwOffset + samp->unLength) {
        unLength = (samp->dwOffset + samp->unLength) - dwOffset;
      }
    }
  }

  if (!parInstrSet) {
    rawFile()->addContainedGTBFile(std::make_shared<std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>>(this));
    pRoot->addGTBFile(this);
  }

  bLoaded = true;
  return true;
}

bool GTBSampColl::parseHeader() {
  return true;
}

bool GTBSampColl::parseSampleInfo() {
  return true;
}

GTBSamp *GTBSampColl::addSamp(uint32_t offset, uint32_t length, uint32_t dataOffset,
                              uint32_t dataLength, uint8_t nChannels, uint16_t bps,
                              uint32_t theRate, std::string name) {
  GTBSamp *newSamp = new GTBSamp(this, offset, length, dataOffset, dataLength, nChannels, bps, theRate, std::move(name));
  samples.push_back(newSamp);
  return newSamp;
}
