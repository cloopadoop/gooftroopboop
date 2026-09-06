/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once

#include "common.h"
#include <vector>

class GTBSeq;
class GTBInstrSet;
class GTBSampColl;
class GTBMiscFile;
class GTBFile;
class GTBSamp;
class DLSFile;
class SF2File;
class SynthFile;

class GTBColl {
 public:
  explicit GTBColl(std::string name = "Unnamed collection");
  virtual ~GTBColl() = default;

  void removeFileAssocs();
  [[nodiscard]] const std::string& name() const;
  void setName(const std::string& newName);
  [[nodiscard]] GTBSeq* seq() const;
  void useSeq(GTBSeq* theSeq);
  void addInstrSet(GTBInstrSet* theInstrSet);
  void addSampColl(GTBSampColl* theSampColl);
  void addMiscFile(GTBMiscFile* theMiscFile);
  bool load();
  virtual bool loadMain() { return true; }
  virtual void preSynthFileCreation() const {}
  virtual void postSynthFileCreation() const {}

  bool containsGTBFile(const GTBFile*) const;

  const std::vector<GTBInstrSet*>& instrSets() const { return m_instrsets; }
  const std::vector<GTBSampColl*>& sampColls() const { return m_sampcolls; }
  const std::vector<GTBMiscFile*>& miscFiles() const { return m_miscfiles; }

 private:
  std::vector<GTBInstrSet*> m_instrsets;
  std::vector<GTBSampColl*> m_sampcolls;
  std::vector<GTBMiscFile*> m_miscfiles;

  GTBSeq* m_seq{};
  std::string m_name;
};
