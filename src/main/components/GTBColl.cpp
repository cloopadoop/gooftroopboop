/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "GTBColl.h"
#include "GTBSeq.h"
#include "GTBInstrSet.h"
#include "GTBSampColl.h"
#include "Root.h"
#include "GTBMiscFile.h"

GTBColl::GTBColl(std::string theName) : m_name(std::move(theName)) {}

void GTBColl::removeFileAssocs() {
  if (m_seq) {
    m_seq->removeCollAssoc(this);
    m_seq = nullptr;
  }

  for (auto set : m_instrsets) {
    set->removeCollAssoc(this);
  }
  for (auto samp : m_sampcolls) {
    samp->removeCollAssoc(this);
  }
  for (auto file : m_miscfiles) {
    file->removeCollAssoc(this);
  }
}

const std::string &GTBColl::name() const {
    return m_name;
}

void GTBColl::setName(const std::string& newName) {
  m_name = newName;
}

GTBSeq *GTBColl::seq() const {
  return m_seq;
}

void GTBColl::useSeq(GTBSeq *theSeq) {
  if (theSeq != nullptr)
    theSeq->addCollAssoc(this);
  if (m_seq && (theSeq != m_seq))  // if we associated with a previous sequence
    m_seq->removeCollAssoc(this);
  m_seq = theSeq;
}

void GTBColl::addInstrSet(GTBInstrSet *theInstrSet) {
  if (theInstrSet != nullptr) {
    theInstrSet->addCollAssoc(this);
    m_instrsets.push_back(theInstrSet);
  }
}

void GTBColl::addSampColl(GTBSampColl *theSampColl) {
  if (theSampColl != nullptr) {
    theSampColl->addCollAssoc(this);
    m_sampcolls.push_back(theSampColl);
  }
}

void GTBColl::addMiscFile(GTBMiscFile *theMiscFile) {
  if (theMiscFile != nullptr) {
    theMiscFile->addCollAssoc(this);
    m_miscfiles.push_back(theMiscFile);
  }
}

bool GTBColl::load() {
  if (!loadMain())
    return false;
  pRoot->addGTBColl(this);
  return true;
}

// A helper lambda function to search for a file in a vector of GTBFile*
template<typename T>
bool contains(const std::vector<T*>& vec, const GTBFile* file) {
  return std::any_of(vec.begin(), vec.end(), [file](const GTBFile* elem) {
    return elem == file;
  });
}

bool GTBColl::containsGTBFile(const GTBFile* file) const {
  // First, check if the file matches the seq property directly
  if (m_seq == file) {
    return true;
  }

  // Then, check if the file is present in any of the file vectors
  if (contains(m_instrsets, file) || contains(m_sampcolls, file) || contains(m_miscfiles, file)) {
    return true;
  }
  return false;
}