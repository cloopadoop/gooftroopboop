/**
 * VGMTrans (c) - 2002-2024
 * Licensed under the zlib license
 * See the included LICENSE for more information
 */
#pragma once

#include <variant>
#include <string>
#include <map>
#include <vector>
#include "Scanner.h"

class GTBColl;
class Matcher;
class GTBScanner;

#define DECLARE_FORMAT(_name_)               \
  _name_##Format _name_##FormatRegisterThis; \
  const std::string _name_##Format::name = #_name_;

#define BEGIN_FORMAT(_name_)                                    \
  class _name_##Format : public Format {                      \
    public:                                                  \
      static const _name_##Format _name_##FormatRegisterThis; \
      static const std::string name;                          \
      _name_##Format() : Format(#_name_) { init(); }          \
      virtual const std::string& getName() { return name; }

#define END_FORMAT() \
  }                  \
  ;

#define USING_SCANNER(scanner) \
  virtual GTBScanner* newScanner() { return new scanner(this); }

#define USING_MATCHER(matcher) \
  virtual Matcher* newMatcher() { return new matcher(this); }

#define USING_MATCHER_WITH_ARG(matcher, arg) \
  virtual Matcher* newMatcher() { return new matcher(this, arg); }

#define USING_COLL(coll) \
  virtual GTBColl* newCollection() { return new coll(); }

#define USES_COLLECTION_FOR_SEQ_CONVERSION() \
  bool usesCollectionDataForSeqConversion() override { return true; }

class Format;
class GTBFile;
class GTBSeq;
class GTBInstrSet;
class GTBSampColl;
class GTBMiscFile;

using FormatMap = std::map<std::string, Format *>;

class Format {
public:
  Format(const std::string &formatName);
  virtual ~Format();

  static Format *formatFromName(const std::string &name);
  static std::vector<Format*> formats();

  virtual bool init();
  virtual const std::string &getName() = 0;
  virtual GTBScanner *newScanner() { return nullptr; }
  GTBScanner &getScanner() const { return *scanner; }
  virtual Matcher *newMatcher() { return nullptr; }
  virtual GTBColl *newCollection();
  virtual bool onNewFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file);
  virtual bool onCloseFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file);
  virtual bool onMatch(std::vector<GTBFile *> &) { return true; }
  virtual bool usesCollectionDataForSeqConversion() { return false; }

  Matcher *matcher;
  GTBScanner *scanner;

protected:
    static FormatMap &registry();
};
