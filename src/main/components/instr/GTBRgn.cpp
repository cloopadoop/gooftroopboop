/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#include "ScaleConversion.h"
#include "GTBRgn.h"
#include "GTBInstrSet.h"

// ******
// GTBRgn
// ******

GTBRgn::GTBRgn(GTBInstr *instr, uint32_t offset, uint32_t length, std::string name)
    : GTBItem(instr->parInstrSet, offset, length, std::move(name)),
      parInstr(instr)
{}

GTBRgn::GTBRgn(GTBInstr *instr, uint32_t offset, uint32_t length, uint8_t theKeyLow, uint8_t theKeyHigh,
               uint8_t theVelLow, uint8_t theVelHigh, int theSampNum, std::string name)
    : GTBItem(instr->parInstrSet, offset, length, std::move(name)),
      parInstr(instr),
      keyLow(theKeyLow),
      keyHigh(theKeyHigh),
      velLow(theVelLow),
      velHigh(theVelHigh),
      sampNum(theSampNum)
{}

void GTBRgn::setRanges(uint8_t theKeyLow, uint8_t theKeyHigh, uint8_t theVelLow, uint8_t theVelHigh) {
  keyLow = theKeyLow;
  keyHigh = theKeyHigh;
  velLow = theVelLow;
  velHigh = theVelHigh;
}

void GTBRgn::setUnityKey(uint8_t theUnityKey) {
  unityKey = theUnityKey;
}

void GTBRgn::setSampNum(uint8_t sampNumber) {
  sampNum = sampNumber;
}

void GTBRgn::setPan(uint8_t p) {
  //pan = thePan;
  pan = p;
  if (pan == 127)
    pan = 1.0;
  else if (pan == 0)
    pan = 0;
  else if (pan == 64)
    pan = 0.5;
  else
    pan = pan / static_cast<double>(127);
}

void GTBRgn::setLoopInfo(int theLoopStatus, uint32_t theLoopStart, uint32_t theLoopLength) {
  loop.loopStatus = theLoopStatus;
  loop.loopStart = theLoopStart;
  loop.loopLength = theLoopLength;
}


void GTBRgn::setADSR(long attackTime, uint16_t atkTransform, long decayTime, long sustainLev,
                     uint16_t rlsTransform, long releaseTime) {
  attack_time = attackTime;
  attack_transform = atkTransform;
  decay_time = decayTime;
  sustain_level = sustainLev;
  release_transform = rlsTransform;
  release_time = releaseTime;
}

void GTBRgn::addGeneralItem(uint32_t offset, uint32_t length, const std::string &name) {
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_GENERIC, offset, length, name));
}

void GTBRgn::addUnknown(uint32_t offset, uint32_t length) {
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_UNKNOWN, offset, length, "Unknown"));
}

//assumes pan is given as 0-127 value, converts it to our double -1.0 to 1.0 format
void GTBRgn::addPan(uint8_t p, uint32_t offset, uint32_t length, const std::string& name) {
  setPan(p);
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_PAN, offset, length, name));
}

void GTBRgn::setVolume(double vol) {
  m_attenDb = ampToDb(vol);
}

void GTBRgn::setAttenuation(double attenDb) {
  m_attenDb = attenDb;
}

void GTBRgn::addVolume(double vol, uint32_t offset, uint32_t length) {
  setVolume(vol);
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_VOL, offset, length, "Volume"));
}

void GTBRgn::addAttenuation(double attenDb, uint32_t offset, uint32_t length) {
  setAttenuation(attenDb);
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_VOL, offset, length, "Attenuation"));
}

void GTBRgn::addUnityKey(uint8_t uk, uint32_t offset, uint32_t length) {
  this->unityKey = uk;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_UNITYKEY, offset, length, "Unity Key"));
}

void GTBRgn::addCoarseTune(int16_t relativeSemitones, uint32_t offset, uint32_t length) {
  this->coarseTune = relativeSemitones;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_FINETUNE, offset, length, "Coarse Tune"));
}

void GTBRgn::addFineTune(int16_t relativePitchCents, uint32_t offset, uint32_t length) {
  this->fineTune = relativePitchCents;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_FINETUNE, offset, length, "Fine Tune"));
}

void GTBRgn::addKeyLow(uint8_t kl, uint32_t offset, uint32_t length) {
  keyLow = kl;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_KEYLOW, offset, length, "Note Range: Low Key"));
}

void GTBRgn::addKeyHigh(uint8_t kh, uint32_t offset, uint32_t length) {
  keyHigh = kh;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_KEYHIGH, offset, length, "Note Range: High Key"));
}

void GTBRgn::addVelLow(uint8_t vl, uint32_t offset, uint32_t length) {
  velLow = vl;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_VELLOW, offset, length, "Vel Range: Low"));
}

void GTBRgn::addVelHigh(uint8_t vh, uint32_t offset, uint32_t length) {
  velHigh = vh;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_VELHIGH, offset, length, "Vel Range: High"));
}

void GTBRgn::addSampNum(int sn, uint32_t offset, uint32_t length) {
  sampNum = sn;
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_SAMPNUM, offset, length, "Sample Number"));
}

void GTBRgn::addADSRValue(uint32_t offset, uint32_t length, const std::string& name) {
  addChild(new GTBRgnItem(this, GTBRgnItem::RIT_ADSR, offset, length, name));
}


// **********
// GTBRgnItem
// **********

GTBRgnItem::GTBRgnItem(const GTBRgn *rgn, RgnItemType rgnItemType, uint32_t offset, uint32_t length, std::string name)
    : GTBItem(rgn->gtbFile(), offset, length, std::move(name), resolveType(rgnItemType)) {
}
