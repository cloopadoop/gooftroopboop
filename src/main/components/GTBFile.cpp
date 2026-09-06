/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "Root.h"
#include "GTBFile.h"
#include "Format.h"

GTBFile::GTBFile(std::string fmt, RawFile *theRawFile, uint32_t offset,
                 uint32_t length, std::string name)
    : GTBItem(this, offset, length, std::move(name)),
      m_rawfile(theRawFile),
      m_format(std::move(fmt)),
      m_id(-1) {}

// Only difference between this AddToUI and GTBItemContainer's version is that we do not add
// this as an item because we do not want the GTBFile to be itself an item in the Item View
void GTBFile::addToUI(GTBItem* /*parent*/, void* UI_specific) {
  for (const auto child : children()) {
    child->addToUI(this, UI_specific);
  }
}

Format *GTBFile::format() const {
  return Format::formatFromName(m_format);
}

std::string GTBFile::formatName() {
  return m_format;
}

std::string GTBFile::description() {
  auto filename = this->m_rawfile->name();
  auto formatName = this->format()->getName();
  return "Format: " + formatName + "     Source File: \"" + filename + "\"";
}

void GTBFile::addCollAssoc(GTBColl *coll) {
  assocColls.push_back(coll);
}

void GTBFile::removeCollAssoc(GTBColl *coll) {
  auto iter = std::ranges::find(assocColls, coll);
  if (iter != assocColls.end())
    assocColls.erase(iter);
}

// These functions are common to all GTBItems, but no reason to refer to GTBfile
// or call rawFile() if the item itself is a GTBFile
RawFile *GTBFile::rawFile() const {
  return m_rawfile;
}

uint32_t GTBFile::readBytes(uint32_t nIndex, uint32_t nCount, void *pBuffer) const {
  // if unLength != 0, verify that we're within the bounds of the file, and truncate num read
  // bytes to end of file
  if (unLength != 0) {
    uint32_t endOff = dwOffset + unLength;
    assert(nIndex >= dwOffset && nIndex < endOff);
    if (nIndex + nCount > endOff)
      nCount = endOff - nIndex;
  }

  return m_rawfile->readBytes(nIndex, nCount, pBuffer);
}

// *********
// GTBHeader
// *********

GTBHeader::GTBHeader(const GTBItem *parItem, uint32_t offset, uint32_t length, const std::string &name)
    : GTBItem(parItem->gtbFile(), offset, length, name) {}

GTBHeader::~GTBHeader() = default;

void GTBHeader::addPointer(uint32_t offset, uint32_t length, uint32_t /*destAddress*/, bool /*notNull*/,
                           const std::string &name) {
  addChild(new GTBHeaderItem(this, GTBHeaderItem::HIT_POINTER, offset, length, name));
}

void GTBHeader::addTempo(uint32_t offset, uint32_t length, const std::string &name) {
  addChild(new GTBHeaderItem(this, GTBHeaderItem::HIT_TEMPO, offset, length, name));
}

void GTBHeader::addSig(uint32_t offset, uint32_t length, const std::string &name) {
  addChild(new GTBHeaderItem(this, GTBHeaderItem::HIT_SIG, offset, length, name));
}

// *************
// GTBHeaderItem
// *************

GTBHeaderItem::GTBHeaderItem(const GTBHeader *hdr, HdrItemType headerType, uint32_t offset, uint32_t length,
                             const std::string &name)
    : GTBItem(hdr->gtbFile(), offset, length, name, resolveType(headerType)) {
}
