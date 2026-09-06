/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#pragma once

#include <variant>

class Format;
class RawFile;
class GTBSeq;
class GTBInstrSet;
class GTBSampColl;
class GTBMiscFile;

// *******
// Matcher
// *******

class Matcher {
public:
  explicit Matcher(Format *format);
  virtual ~Matcher() = default;

  virtual void onFinishedScan(RawFile *rawfile) {}

  virtual bool onNewFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file);
  virtual bool onCloseFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file);

protected:
  virtual bool onNewSeq(GTBSeq *) { return false; }
  virtual bool onNewInstrSet(GTBInstrSet *) { return false; }
  virtual bool onNewSampColl(GTBSampColl *) { return false; }
  virtual bool onCloseSeq(GTBSeq *) { return false; }
  virtual bool onCloseInstrSet(GTBInstrSet *) { return false; }
  virtual bool onCloseSampColl(GTBSampColl *) { return false; }

  Format *fmt;
};
