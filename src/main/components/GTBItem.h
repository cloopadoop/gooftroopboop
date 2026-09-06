#pragma once

#include "common.h"
#include <algorithm>
#include <vector>
#include <ranges>

template <class T>
class Menu;
class RawFile;
class GTBFile;
class GTBHeader;

class GTBItem {
public:
  enum class Type {
    Adsr,
    BankSelect,
    ChangeState,
    Control,
    UseDrumKit,
    DurationChange,
    DurationNote,
    Expression,
    ExpressionSlide,
    FineTune,
    Header,
    Instrument,
    Jump,
    JumpConditional,
    Loop,
    LoopBreak,
    LoopForever,
    Lfo,
    Marker,
    MasterVolume,
    MasterVolumeSlide,
    Misc,
    Modulation,
    Mute,
    Noise,
    Nop,
    NoteOff,
    NoteOn,
    Octave,
    Pan,
    PanLfo,
    PanSlide,
    PanEnvelope,
    PitchBend,
    PitchBendSlide,
    PitchBendRange,
    PitchEnvelope,
    Portamento,
    PortamentoTime,
    Priority,
    ProgramChange,
    RepeatStart,
    RepeatEnd,
    Rest,
    Reverb,
    Sample,
    Sustain,
    Tempo,
    Tie,
    TimeSignature,
    Track,
    TrackEnd,
    Transpose,
    Tremelo,
    Unrecognized,
    Unknown,
    Vibrato,
    Volume,
    VolumeSlide,
    VolumeEnvelope,
  };

public:
  GTBItem();
  GTBItem(GTBFile *GTBfile,
          uint32_t offset,
          uint32_t length = 0,
          std::string name = "",
          Type type = Type::Unknown);
  virtual ~GTBItem();

  friend bool operator>(GTBItem &item1, GTBItem &item2);
  friend bool operator<=(GTBItem &item1, GTBItem &item2);
  friend bool operator<(GTBItem &item1, GTBItem &item2);
  friend bool operator>=(GTBItem &item1, GTBItem &item2);

  [[nodiscard]] const std::string& name() const noexcept { return m_name; }
  void setName(const std::string& newName) { m_name = newName; }

  [[nodiscard]] class GTBFile* gtbFile() const { return m_GTBfile; }
  [[nodiscard]] RawFile* rawFile() const;

  virtual bool isItemAtOffset(uint32_t offset, bool matchStartOffset = false);
  GTBItem* getItemAtOffset(uint32_t offset, bool matchStartOffset = false);

  virtual uint32_t guessLength();
  virtual void setGuessedLength();
  virtual std::string description() { return ""; }
  virtual void addToUI(GTBItem *parent, void *UI_specific);

  const std::vector<GTBItem*>& children() { return m_children; }
  GTBItem* addChild(GTBItem* child);
  GTBItem* addChild(uint32_t offset, uint32_t length, const std::string &name);
  GTBItem* addUnknownChild(uint32_t offset, uint32_t length);
  GTBHeader* addHeader(uint32_t offset, uint32_t length, const std::string &name = "Header");
  void removeChildren();
  void transferChildren(GTBItem* destination);

  template <std::ranges::input_range Range>
  requires std::convertible_to<std::ranges::range_value_t<Range>, GTBItem*>
  void addChildren(const Range& items) {
    std::ranges::copy(items, std::back_inserter(m_children));
  }

  void sortChildrenByOffset();

protected:
  uint32_t readBytes(uint32_t index, uint32_t count, void *buffer) const;
  [[nodiscard]] uint8_t readByte(uint32_t offset) const;
  [[nodiscard]] uint16_t readShort(uint32_t offset) const;
  [[nodiscard]] uint32_t getWord(uint32_t offset) const;
  [[nodiscard]] uint16_t getShortBE(uint32_t offset) const;
  [[nodiscard]] uint32_t getWordBE(uint32_t offset) const;
  bool isValidOffset(uint32_t offset) const;
  // FIXME: clearChildren() is a workaround for GTBSeqNoTrks' multiple inheritance diamond problem

public:
  uint32_t dwOffset;  // offset in the pDoc data buffer
  uint32_t unLength;  // num of bytes the event engulfs
  const Type type;

private:
  std::vector<GTBItem *> m_children;
  GTBFile *m_GTBfile;
  std::string m_name;
};

struct ItemPtrOffsetCmp {
  bool operator()(const GTBItem *a, const GTBItem *b) const { return (a->dwOffset < b->dwOffset); }
};
