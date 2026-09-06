#include "GTBItem.h"
#include "RawFile.h"
#include "GTBFile.h"
#include "Root.h"
#include "helper.h"

GTBItem::GTBItem() : m_GTBfile(nullptr), dwOffset(0), unLength(0), type(Type::Unknown) {
}

GTBItem::GTBItem(GTBFile *GTBfile, uint32_t offset, uint32_t length, std::string name, Type type)
    : m_GTBfile(GTBfile), m_name(std::move(name)), dwOffset(offset), unLength(length), type(type) {
}

GTBItem::~GTBItem() {
  deleteVect(m_children);
}

bool operator>(const GTBItem &item1, const GTBItem &item2) {
  return item1.dwOffset > item2.dwOffset;
}

bool operator<=(const GTBItem &item1, const GTBItem &item2) {
  return item1.dwOffset <= item2.dwOffset;
}

bool operator<(const GTBItem &item1, const GTBItem &item2) {
  return item1.dwOffset < item2.dwOffset;
}

bool operator>=(const GTBItem &item1, const GTBItem &item2) {
  return item1.dwOffset >= item2.dwOffset;
}

RawFile *GTBItem::rawFile() const {
  return m_GTBfile->rawFile();
}

bool GTBItem::isItemAtOffset(uint32_t offset, bool matchStartOffset) {
  return getItemAtOffset(offset, matchStartOffset) != nullptr;
}

GTBItem* GTBItem::getItemAtOffset(uint32_t offset, bool matchStartOffset) {
  if (m_children.empty()) {
    if ((matchStartOffset ? offset == dwOffset : offset >= dwOffset) && (offset < dwOffset + unLength)) {
      return this;
    }
  }

  for (const auto child : m_children) {
    if (GTBItem *foundItem = child->getItemAtOffset(offset, matchStartOffset))
      return foundItem;
  }

  return nullptr;
}

void GTBItem::addToUI(GTBItem *parent, void *UI_specific) {
  pRoot->UI_addItem(this, parent, name(), UI_specific);

  for (const auto child : m_children) {
    child->addToUI(this, UI_specific);
  }
}

uint32_t GTBItem::readBytes(uint32_t nIndex, uint32_t nCount, void *pBuffer) const {
  return m_GTBfile->readBytes(nIndex, nCount, pBuffer);
}

uint8_t GTBItem::readByte(uint32_t offset) const {
  return m_GTBfile->readByte(offset);
}

uint16_t GTBItem::readShort(uint32_t offset) const {
  return m_GTBfile->readShort(offset);
}

uint32_t GTBItem::getWord(uint32_t offset) const {
  return m_GTBfile->readWord(offset);
}

// GetShort Big Endian
uint16_t GTBItem::getShortBE(uint32_t offset) const {
  return m_GTBfile->readShortBE(offset);
}

// GetWord Big Endian
uint32_t GTBItem::getWordBE(uint32_t offset) const {
  return m_GTBfile->readWordBE(offset);
}

bool GTBItem::isValidOffset(uint32_t offset) const {
  return m_GTBfile->isValidOffset(offset);
}

GTBItem* GTBItem::addChild(GTBItem *item) {
  m_children.emplace_back(item);
  return item;
}

GTBItem* GTBItem::addChild(uint32_t offset, uint32_t length, const std::string &name) {
  auto child = new GTBItem(gtbFile(), offset, length, name, Type::Header);
  m_children.emplace_back(child);
  return child;
}

GTBItem* GTBItem::addUnknownChild(uint32_t offset, uint32_t length) {
  auto child = new GTBItem(gtbFile(), offset, length, "Unknown");
  m_children.emplace_back(child);
  return child;
}

GTBHeader* GTBItem::addHeader(uint32_t offset, uint32_t length, const std::string &name) {
  auto *header = new GTBHeader(this, offset, length, name);
  m_children.emplace_back(header);
  return header;
}

void GTBItem::removeChildren() {
  m_children.clear();
}

void GTBItem::transferChildren(GTBItem* destination) {
  destination->addChildren(m_children);
  m_children.clear();
}

void GTBItem::sortChildrenByOffset() {
  std::ranges::sort(m_children, [](const GTBItem *a, const GTBItem *b) {
    return a->dwOffset < b->dwOffset;
  });

  // Recursively sort the children of each child, if they have any children
  for (GTBItem* child : m_children) {
    if (!child->children().empty()) {
      child->sortChildrenByOffset();
    }
  }
}

// Guess length of a container from its descendants
uint32_t GTBItem::guessLength() {
  if (m_children.empty()) {
    return unLength;
  }
  // NOTE: child items can sometimes overlap each other
  uint32_t guessedLength = 0;
  for (const auto child : m_children) {
    assert(dwOffset <= child->dwOffset);

    uint32_t itemLength = child->unLength;
    if (unLength == 0) {
      itemLength = child->guessLength();
    }

    uint32_t expectedLength = child->dwOffset + itemLength - dwOffset;
    if (guessedLength < expectedLength) {
      guessedLength = expectedLength;
    }
  }
  return guessedLength;
}

void GTBItem::setGuessedLength() {
  for (const auto child : m_children) {
    child->setGuessedLength();
  }
  if (unLength == 0) {
    unLength = guessLength();
  }
}
