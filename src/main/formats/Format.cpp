/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "Format.h"
#include "Scanner.h"
#include "Matcher.h"
#include "GTBColl.h"

FormatMap &Format::registry() {
  static FormatMap registry;
  return registry;
}

Format::Format(const std::string &formatName) : matcher(nullptr), scanner(nullptr) {
  registry().insert(make_pair(formatName, this));
}

Format::~Format() {
  delete scanner;
  delete matcher;
}

Format *Format::formatFromName(const std::string &name) {
  auto findIt = registry().find(name);
  if (findIt == registry().end())
    return nullptr;
  return (*findIt).second;
}

bool Format::onNewFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file) {
  if (!matcher) {
    return false;
  }
  return matcher->onNewFile(file);
}

GTBColl *Format::newCollection() {
  return new GTBColl();
}

bool Format::onCloseFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file) {
  if (!matcher) {
    return false;
  }
  return matcher->onCloseFile(file);
}

bool Format::init() {
  scanner = newScanner();
  matcher = newMatcher();
  return true;
}
