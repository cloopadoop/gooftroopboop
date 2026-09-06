/*
 * VGMTrans (c) 2002-2019
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once

#include "GTBFile.h"

// ***********
// GTBMiscFile
// ***********

class RawFile;

class GTBMiscFile : public GTBFile {
public:
  GTBMiscFile(const std::string &format, RawFile *file, uint32_t offset, uint32_t length = 0,
              std::string name = "GTBMiscFile");

  bool loadGTBFile(bool useMatcher = true) override;
  virtual bool loadMain();
  bool load() override;
};
