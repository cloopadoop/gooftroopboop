/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "Matcher.h"

Matcher::Matcher(Format *format) {
  fmt = format;
}

bool Matcher::onNewFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file) {
  if(auto seq = std::get_if<GTBSeq *>(&file)) {
    onNewSeq(*seq);
  } else if(auto instr = std::get_if<GTBInstrSet *>(&file)) {
    onNewInstrSet(*instr);
  } else if(auto sampcoll = std::get_if<GTBSampColl *>(&file)) {
    onNewSampColl(*sampcoll);
}

  return false;
}

bool Matcher::onCloseFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file) {
  if(auto seq = std::get_if<GTBSeq *>(&file)) {
    onCloseSeq(*seq);
  } else if(auto instr = std::get_if<GTBInstrSet *>(&file)) {
    onCloseInstrSet(*instr);
  } else if(auto sampcoll = std::get_if<GTBSampColl *>(&file)) {
    onCloseSampColl(*sampcoll);
  }
  return false;
}
