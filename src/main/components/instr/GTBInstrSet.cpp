/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "GTBInstrSet.h"
#include <spdlog/fmt/fmt.h>
#include "GTBSampColl.h"
#include "GTBSamp.h"
#include "GTBRgn.h"
#include "GTBColl.h"
#include "Root.h"
#include "Format.h"
#include "LogManager.h"
#include "helper.h"

// ***********
// GTBInstrSet
// ***********

GTBInstrSet::GTBInstrSet(const std::string &format, RawFile *file, uint32_t offset, uint32_t length,
                         std::string name, GTBSampColl *theSampColl)
    : GTBFile(format, file, offset, length, std::move(name)),
      sampColl(theSampColl) {
}

GTBInstrSet::~GTBInstrSet() {
  delete sampColl;
}

GTBInstr *GTBInstrSet::addInstr(uint32_t offset, uint32_t length, uint32_t bank,
                                uint32_t instrNum, const std::string &instrName) {
  GTBInstr *instr =
      new GTBInstr(this, offset, length, bank, instrNum,
                   instrName.empty() ? fmt::format("Instrument {}", aInstrs.size()) : instrName);
  aInstrs.push_back(instr);
  return instr;
}

bool GTBInstrSet::loadGTBFile(bool useMatcher) {
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

bool GTBInstrSet::load() {
  if (!parseHeader())
    return false;
  if (!parseInstrPointers())
    return false;
  if (!loadInstrs())
    return false;

  if (m_auto_add_instruments_as_children)
    addChildren(aInstrs);

  if (unLength == 0) {
    setGuessedLength();
  }

  if (sampColl != nullptr) {
    if (!sampColl->load()) {
      L_WARN("Failed to load GTBSampColl");
    } else {
      sampColl->transferChildren(this);
    }
  }

  rawFile()->addContainedGTBFile(
      std::make_shared<std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>>(this));
  pRoot->addGTBFile(this);
  return true;
}

bool GTBInstrSet::parseHeader() {
  return true;
}

bool GTBInstrSet::parseInstrPointers() {
  return true;
}

bool GTBInstrSet::loadInstrs() {
  size_t nInstrs = aInstrs.size();
  for (size_t i = 0; i < nInstrs; i++) {
    if (!aInstrs[i]->loadInstr())
      return false;
  }
  return true;
}

// ********
// GTBInstr
// ********

GTBInstr::GTBInstr(GTBInstrSet *instrSet, uint32_t offset, uint32_t length, uint32_t theBank,
                   uint32_t theInstrNum, std::string name, float reverb)
    : GTBItem(instrSet, offset, length, std::move(name), Type::Instrument),
      bank(theBank), instrNum(theInstrNum), parInstrSet(instrSet), reverb(reverb) {
}

void GTBInstr::setBank(uint32_t bankNum) {
  bank = bankNum;
}

void GTBInstr::setInstrNum(uint32_t theInstrNum) {
  instrNum = theInstrNum;
}

GTBRgn *GTBInstr::addRgn(GTBRgn *rgn) {
  m_regions.emplace_back(rgn);
  if (m_auto_add_regions_as_children)
    addChild(rgn);
  return rgn;
}

GTBRgn *GTBInstr::addRgn(uint32_t offset, uint32_t length, int sampNum, uint8_t keyLow,
                         uint8_t keyHigh, uint8_t velLow, uint8_t velHigh) {
  GTBRgn *newRgn = new GTBRgn(this, offset, length, keyLow, keyHigh, velLow, velHigh, sampNum);
  m_regions.emplace_back(newRgn);
  if (m_auto_add_regions_as_children)
    addChild(newRgn);
  return newRgn;
}

void GTBInstr::deleteRegions() {
  deleteVect(m_regions);
}
