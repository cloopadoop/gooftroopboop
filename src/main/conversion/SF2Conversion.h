/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
*/
#pragma once

#include <vector>

class SF2File;
class GTBColl;
class GTBInstrSet;
class GTBSampColl;
class SynthFile;
class GTBSamp;

namespace conversion {

SF2File* createSF2File(const GTBColl& coll);
SF2File* createSF2File(
  const std::vector<GTBInstrSet*>& instrsets,
  const std::vector<GTBSampColl*>& sampcolls,
  const GTBColl* coll
);
SynthFile* createSynthFile(
  const std::vector<GTBInstrSet*>& instrsets,
  const std::vector<GTBSampColl*>& sampcolls
);
void unpackSampColl(SynthFile &synthfile, const GTBSampColl *sampColl, std::vector<GTBSamp *> &finalSamps);

} // conversion
