/*
 * VGMTrans (c) 2002-2019
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "GTBMiscFile.h"
#include "Root.h"
#include "Format.h"

// ***********
// GTBMiscFile
// ***********

GTBMiscFile::GTBMiscFile(const std::string &format, RawFile *file, uint32_t offset, uint32_t length,
                         std::string name)
    : GTBFile(format, file, offset, length, std::move(name)) {
}

bool GTBMiscFile::loadMain() {
  return true;
}

bool GTBMiscFile::loadGTBFile(bool useMatcher) {
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

bool GTBMiscFile::load() {
  if (!loadMain()) {
    return false;
  }
  if (unLength == 0) {
    return false;
  }

  rawFile()->addContainedGTBFile(std::make_shared<std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>>(this));
  pRoot->addGTBFile(this);
  return true;
}
