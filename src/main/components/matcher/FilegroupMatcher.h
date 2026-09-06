/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
*/

#pragma once

#include "Matcher.h"
#include <list>

class GTBSeq;
class GTBInstrSet;
class GTBSampColl;

// ****************
// FilegroupMatcher
// ****************

// FilegroupMatcher handles formats where the primary method of associating sequences, instrument
// sets, and sample collections is that they are loaded together within the same RawFile. When
// loading is complete, FilegroupMatcher processes the GTBFiles in the order they were added and
// groups them into collections: thus, it assumes association by order. FilegroupMatcher does not
// retain state between loads, so it will never create a collection that spans multiple RawFiles,
// with the exception that FilegroupMatcher processes a psf file and all of its psflib dependencies
// together as a single load.

// When loading is complete, FilegroupMatcher:
// 1) Sorts sequences, instrument sets, and sample collections by their offsets.
// 2) Attempts to pair each instrument set with a sample collection. It calls
// GTBInstrSet::isViableSampCollMatch() to filter out non-viable sample collections. If the file
// contains a single instrument set and sample collection, they are always paired. After pairing, a
// sample collection is removed from the pool for matching, unless it is the last one.
// 3) Creates a new collection for each sequence by iterating over the instrument set / sample
// collection pairs. If that list is exhausted before the sequence list, it reuses the last
// instrument set / sample collection pair.



class FilegroupMatcher : public Matcher {
public:
  explicit FilegroupMatcher(Format *format);

  void onFinishedScan(RawFile *rawfile) override;

protected:
  bool onNewSeq(GTBSeq *seq) override;
  bool onNewInstrSet(GTBInstrSet *instrset) override;
  bool onNewSampColl(GTBSampColl *sampcoll) override;

  bool onCloseSeq(GTBSeq *seq) override;
  bool onCloseInstrSet(GTBInstrSet *instrset) override;
  bool onCloseSampColl(GTBSampColl *sampcoll) override;

  virtual void lookForMatch();

protected:
  std::list<GTBSeq *> seqs;
  std::list<GTBInstrSet *> instrsets;
  std::list<GTBSampColl *> sampcolls;
};
