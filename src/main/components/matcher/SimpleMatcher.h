/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
*/

#pragma once

#include <map>

#include "Matcher.h"
#include "Format.h"
#include "GTBSeq.h"
#include "GTBInstrSet.h"
#include "GTBSampColl.h"
#include "GTBColl.h"

// *************
// SimpleMatcher
// *************

// Simple Matcher is used for those collections that use only 1 Instr Set
// and optionally 1 SampColl

template <class IdType>
class SimpleMatcher : public Matcher {
protected:
  // The following functions should return with the id variable containing the retrieved id of the
  // file. The bool return value is a flag for error: true on success and false on fail.
  virtual bool seqId(GTBSeq *seq, IdType &id) = 0;
  virtual bool instrSetId(GTBInstrSet *instrset, IdType &id) = 0;
  virtual bool sampCollId(GTBSampColl *sampcoll, IdType &id) = 0;

  explicit SimpleMatcher(Format *format, bool bUsingSampColl = false)
      : Matcher(format), bRequiresSampColl(bUsingSampColl) {}

  bool onNewSeq(GTBSeq *seq) override {
    IdType id;
    bool success = this->seqId(seq, id);
    if (!success)
      return false;

    seqs.insert(std::pair<IdType, GTBSeq *>(id, seq));

    if (GTBInstrSet *matchingInstrSet = instrsets[id]) {
      if (bRequiresSampColl) {
        if (GTBSampColl *matchingSampColl = sampcolls[id]) {
          GTBColl *coll = fmt->newCollection();
          if (!coll)
            return false;
          coll->setName(seq->name());
          coll->useSeq(seq);
          coll->addInstrSet(matchingInstrSet);
          coll->addSampColl(matchingSampColl);
          if (!coll->load()) {
            delete coll;
            return false;
          }
        }
      } else {
        GTBColl *coll = fmt->newCollection();
        if (!coll)
          return false;
        coll->setName(seq->name());
        coll->useSeq(seq);
        coll->addInstrSet(matchingInstrSet);
        if (!coll->load()) {
          delete coll;
          return false;
        }
      }
    }

    return true;
  }

  bool onNewInstrSet(GTBInstrSet *instrset) override {
    IdType id;
    bool success = this->instrSetId(instrset, id);
    if (!success)
      return false;
    if (instrsets[id])
      return false;
    instrsets[id] = instrset;

    GTBSampColl *matchingSampColl = nullptr;
    if (bRequiresSampColl) {
      matchingSampColl = sampcolls[id];
      if (matchingSampColl && matchingSampColl->shouldLoadOnInstrSetMatch()) {
        matchingSampColl->useInstrSet(instrset);
        if (!matchingSampColl->load()) {
          onCloseSampColl(matchingSampColl);
          return false;
        }
      }
    }

    std::pair<typename std::multimap<IdType, GTBSeq *>::iterator,
              typename std::multimap<IdType, GTBSeq *>::iterator>
        itPair;
    // equal_range(b) returns pair<iterator,iterator> representing the range
    // of element with key b
    itPair = seqs.equal_range(id);
    // Loop through range of maps with id key
    for (auto it2 = itPair.first; it2 != itPair.second; ++it2) {
      GTBSeq *matchingSeq = (*it2).second;

      if (bRequiresSampColl) {
        if (matchingSampColl) {
          GTBColl *coll = fmt->newCollection();
          if (!coll)
            return false;
          coll->setName(matchingSeq->name());
          coll->useSeq(matchingSeq);
          coll->addInstrSet(instrset);
          coll->addSampColl(matchingSampColl);
          coll->load();
        }
      } else {
        GTBColl *coll = fmt->newCollection();
        if (!coll)
          return false;
        coll->setName(matchingSeq->name());
        coll->useSeq(matchingSeq);
        coll->addInstrSet(instrset);
        if (!coll->load()) {
          delete coll;
          return false;
        }
      }
    }
    return true;
  }

  bool onNewSampColl(GTBSampColl *sampcoll) override {
    if (bRequiresSampColl) {
      IdType id;
      bool success = this->sampCollId(sampcoll, id);
      if (!success)
        return false;
      if (sampcolls[id])
        return false;
      sampcolls[id] = sampcoll;

      GTBInstrSet *matchingInstrSet = instrsets[id];

      if (matchingInstrSet) {
        if (sampcoll->shouldLoadOnInstrSetMatch()) {
          sampcoll->useInstrSet(matchingInstrSet);
          if (!sampcoll->load()) {
            onCloseSampColl(sampcoll);
            return false;
          }
        }
      }

      std::pair<typename std::multimap<IdType, GTBSeq *>::iterator,
                typename std::multimap<IdType, GTBSeq *>::iterator>
          itPair;
      // equal_range(b) returns pair<iterator,iterator> representing the range
      // of element with key b
      itPair = seqs.equal_range(id);
      // Loop through range of maps with id key
      for (auto it2 = itPair.first; it2 != itPair.second; ++it2) {
        GTBSeq *matchingSeq = (*it2).second;

        if (matchingSeq && matchingInstrSet) {
          GTBColl *coll = fmt->newCollection();
          if (!coll)
            return false;
          coll->setName(matchingSeq->name());
          coll->useSeq(matchingSeq);
          coll->addInstrSet(matchingInstrSet);
          coll->addSampColl(sampcoll);
          if (!coll->load()) {
            delete coll;
            return false;
          }
        }
      }
      return true;
    }
    return true;
  }

  bool onCloseSeq(GTBSeq *seq) override {
    IdType id;
    bool success = this->seqId(seq, id);
    if (!success)
      return false;

    // Find the first matching key.
    auto itr = seqs.find(id);
    // Search for the specific seq to remove.
    if (itr != seqs.end()) {
      do {
        if (itr->second == seq) {
          seqs.erase(itr);
          break;
        }

        ++itr;
      } while (itr != seqs.upper_bound(id));
    }
    return true;
  }

  bool onCloseInstrSet(GTBInstrSet *instrset) override {
    IdType id;
    bool success = this->instrSetId(instrset, id);
    if (!success)
      return false;
    instrsets.erase(id);
    return true;
  }

  bool onCloseSampColl(GTBSampColl *sampcoll) override {
    IdType id;
    bool success = this->sampCollId(sampcoll, id);
    if (!success)
      return false;
    sampcolls.erase(id);
    return true;
  }

private:
  bool bRequiresSampColl;

  std::multimap<IdType, GTBSeq *> seqs;
  std::map<IdType, GTBInstrSet *> instrsets;
  std::map<IdType, GTBSampColl *> sampcolls;
};
