/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once

#include "GTBItem.h"
#include "RawFile.h"

class GTBColl;
class Format;

class GTBFile : public GTBItem {
public:
  GTBFile(std::string format, RawFile *theRawFile, uint32_t offset, uint32_t length = 0,
          std::string name = "GTB File");
  ~GTBFile() override = default;

  void addToUI(GTBItem *parent, void *UI_specific) override;

  [[nodiscard]] std::string description() override;

  virtual bool loadGTBFile(bool useMatcher) = 0;
  virtual bool load() = 0;
  Format* format() const;
  [[nodiscard]] std::string formatName();

  virtual uint32_t id() const { return m_id; }
  void setId(uint32_t newId) { m_id = newId; }

  void addCollAssoc(GTBColl *coll);
  void removeCollAssoc(GTBColl *coll);
  [[nodiscard]] RawFile *rawFile() const;

  [[nodiscard]] size_t size() const noexcept { return unLength; }

  uint32_t readBytes(uint32_t nIndex, uint32_t nCount, void *pBuffer) const;

  inline uint8_t readByte(uint32_t offset) const { return m_rawfile->readByte(offset); }
  inline uint16_t readShort(uint32_t offset) const { return m_rawfile->readShort(offset); }
  inline uint32_t readWord(uint32_t offset) const { return m_rawfile->readWord(offset); }
  inline uint16_t readShortBE(uint32_t offset) const { return m_rawfile->readShortBE(offset); }
  inline uint32_t readWordBE(uint32_t offset) const { return m_rawfile->readWordBE(offset); }
  inline bool isValidOffset(uint32_t offset) const { return m_rawfile->isValidOffset(offset); }

  uint32_t startOffset() const { return dwOffset; }
  /*
   * For whatever reason, you can create null-length GTBItems.
   * The only safe way for now is to
   * assume maximum length
   */
  uint32_t endOffset() const { return static_cast<uint32_t>(m_rawfile->size()); }

  [[nodiscard]] const char *data() const { return m_rawfile->data() + dwOffset; }

  std::vector<GTBColl*> assocColls;

private:
  RawFile* m_rawfile;
  std::string m_format;
  uint32_t m_id;
};

// *********
// GTBHeader
// *********

class GTBHeader : public GTBItem {
public:
  GTBHeader(const GTBItem *parItem, uint32_t offset = 0, uint32_t length = 0,
            const std::string &name = "Header");
  ~GTBHeader() override;

  void addPointer(uint32_t offset, uint32_t length, uint32_t destAddress, bool notNull,
                  const std::string &name = "Pointer");
  void addTempo(uint32_t offset, uint32_t length, const std::string &name = "Tempo");
  void addSig(uint32_t offset, uint32_t length, const std::string &name = "Signature");
};

// *************
// GTBHeaderItem
// *************

class GTBHeaderItem : public GTBItem {
public:
  enum HdrItemType {
    HIT_POINTER,
    HIT_TEMPO,
    HIT_SIG,
    HIT_GENERIC,
    HIT_UNKNOWN
  };  // HIT = Header Item Type

  GTBHeaderItem(const GTBHeader *hdr, HdrItemType headerType, uint32_t offset, uint32_t length,
                const std::string &name);

private:
  static Type resolveType(HdrItemType headerType) {
    switch (headerType) {
      case HIT_UNKNOWN: return Type::Unknown;
      case HIT_POINTER: return Type::Misc;
      case HIT_TEMPO:   return Type::Tempo;
      case HIT_SIG:     return Type::Misc;
      default:          return Type::Misc;
    }
  }
};
