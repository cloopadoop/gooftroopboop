/*
 * VGMTrans (c) - 2002-2024
 * Licensed under the zlib license
 * See the included LICENSE for more information
*/
#pragma once

#include <vector>

class DLSFile;
class GTBInstrSet;
class GTBSampColl;
class GTBSamp;
class GTBColl;

namespace conversion {

bool createDLSFile(DLSFile& dls, const GTBColl& coll);
bool createDLSFile(
  DLSFile& dls,
  const std::vector<GTBInstrSet*>& instrsets,
  const std::vector<GTBSampColl*>& sampcolls,
  const GTBColl* coll
);
bool mainDLSCreation(
  DLSFile& dls,
  const std::vector<GTBInstrSet*>& instrsets,
  const std::vector<GTBSampColl*>& sampcolls
);
void unpackSampColl(DLSFile& dls, const GTBSampColl* sampColl, std::vector<GTBSamp*>& finalSamps);

} // conversion

