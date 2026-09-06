/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
*/

#pragma once

#include "SimpleMatcher.h"

class GetIdMatcher : public SimpleMatcher<uint32_t> {
public:
  explicit GetIdMatcher(Format *format, bool bRequiresSampColl = false)
      : SimpleMatcher(format, bRequiresSampColl) {}

  bool seqId(GTBSeq *seq, uint32_t &id) override {
    id = seq->id();
    return (id != -1u);
  }

  bool instrSetId(GTBInstrSet *instrset, uint32_t &id) override {
    id = instrset->id();
    return (id != -1u);
  }

  bool sampCollId(GTBSampColl *sampcoll, uint32_t &id) override {
    id = sampcoll->id();
    return (id != -1u);
  }
};