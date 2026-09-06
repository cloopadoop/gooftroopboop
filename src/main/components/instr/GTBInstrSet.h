#pragma once
#include "common.h"
#include "GTBFile.h"

class GTBSampColl;
class GTBInstr;
class GTBRgn;
class GTBSamp;
class GTBRgnItem;

constexpr float defaultReverbPercent = 0.25;

// ***********
// GTBInstrSet
// ***********

class GTBInstrSet : public GTBFile {
public:
  GTBInstrSet(const std::string &format, RawFile *file, uint32_t offset, uint32_t length = 0,
              std::string name = "GTBInstrSet", GTBSampColl *theSampColl = nullptr);
  ~GTBInstrSet() override;

  bool loadGTBFile(bool useMatcher = true) override;
  bool load() override;
  virtual bool parseHeader();
  virtual bool parseInstrPointers();
  virtual bool loadInstrs();
  virtual bool isViableSampCollMatch(GTBSampColl*) { return true; }

  GTBInstr *addInstr(uint32_t offset, uint32_t length, uint32_t bank, uint32_t instrNum,
                     const std::string &instrName = "");

  std::vector<GTBInstr *> aInstrs;
  GTBSampColl *sampColl;

protected:
   void disableAutoAddInstrumentsAsChildren() { m_auto_add_instruments_as_children = false; }

private:
   bool m_auto_add_instruments_as_children{true};
};

// ********
// GTBInstr
// ********

class GTBInstr : public GTBItem {
public:
  GTBInstr(GTBInstrSet *parInstrSet, uint32_t offset, uint32_t length, uint32_t bank,
           uint32_t instrNum, std::string name = "Instrument",
           float reverb = defaultReverbPercent);

  const std::vector<GTBRgn*>& regions() { return m_regions; }

  inline void setBank(uint32_t bankNum);
  inline void setInstrNum(uint32_t theInstrNum);

  GTBRgn *addRgn(GTBRgn *rgn);
  GTBRgn *addRgn(uint32_t offset, uint32_t length, int sampNum, uint8_t keyLow = 0,
                 uint8_t keyHigh = 0x7F, uint8_t velLow = 0, uint8_t velHigh = 0x7F);

  virtual bool loadInstr() { return true; }

  uint32_t bank;
  uint32_t instrNum;
  GTBInstrSet *parInstrSet;
  float reverb;


protected:
  void disableAutoAddRegionsAsChildren() { m_auto_add_regions_as_children = false; }
  void deleteRegions();

private:
  bool m_auto_add_regions_as_children{true};
  std::vector<GTBRgn*> m_regions;
};
