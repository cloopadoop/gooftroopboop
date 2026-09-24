// gtb-engine: headless JSON edit engine for the web front-end.
//
// Wraps the same CapcomPianoRollModel the Qt editor uses (no widgets, no
// window) and speaks line-delimited JSON over stdin/stdout. The Tauri app
// bundles this as a sidecar. Also usable straight from a terminal:
//
//   echo {"cmd":"open","path":"song.spc"} | gtb-engine
//
// Protocol: one JSON request per line on stdin, one JSON response per line on
// stdout. Request: {"id":N,"cmd":"...",<params>}. Response on success:
// {"id":N,"ok":true,"state":{...}}; on failure: {"id":N,"ok":false,"error":"..."}.

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <process.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "formats/CapcomSnes/CapcomSeqIR.h"

#include "nlohmann/json.hpp"

#include "RawFile.h"
#include "AsmSourceRepair.h"
#include "Root.h"
#include "formats/CapcomSnes/CapcomSnesSeq.h"
#include "formats/CapcomSnes/CapcomSnesInstr.h"
#include "formats/CapcomSnes/CapcomTrackTraversal.h"
#include "workarea/CapcomPianoRollModel.h"

#include "Snes_Spc.h"
#include "Spc_Filter.h"

using nlohmann::json;

namespace {

constexpr uint32_t kAramSize = 0x10000;
constexpr uint32_t kSpcAramFileOffset = 0x100;
constexpr char kSpcSignature[] = "SNES-SPC700 Sound File Data";

// Distinct per-process AND per-operation paths: replacing a session destroys
// the old backing file, so even two successive imports must not share a name.
std::filesystem::path operationTempPath(const char* role, const char* extension) {
  static const auto epoch = std::chrono::steady_clock::now().time_since_epoch().count();
  static uint64_t sequence = 0;
  return std::filesystem::temp_directory_path() /
         (std::string("gtb-") + role + "-" + std::to_string(::_getpid()) + "-" +
          std::to_string(epoch) + "-" + std::to_string(++sequence) + extension);
}

class TestRoot final : public GTBRoot {
 public:
  void UI_setRootPtr(GTBRoot** theRoot) override { *theRoot = this; }
  std::string UI_getSaveFilePath(const std::string&, const std::string&) override { return {}; }
  std::string UI_getSaveDirPath(const std::string&) override { return {}; }
};

const char* settingTypeName(CapcomSettingType t) {
  switch (t) {
    case CapcomSettingType::Tempo: return "tempo";
    case CapcomSettingType::Volume: return "volume";
    case CapcomSettingType::Pan: return "pan";
    case CapcomSettingType::Duration: return "duration";
    case CapcomSettingType::Octave: return "octave";
    case CapcomSettingType::Transpose: return "transpose";
    case CapcomSettingType::GlobalTranspose: return "globalTranspose";
    case CapcomSettingType::LFO: return "lfo";
    case CapcomSettingType::Echo: return "echo";
    case CapcomSettingType::ReleaseRate: return "releaseRate";
    case CapcomSettingType::Portamento: return "portamento";
    default: return "unknown";
  }
}

CapcomSettingType settingTypeFromName(const std::string& s) {
  if (s == "tempo") return CapcomSettingType::Tempo;
  if (s == "volume") return CapcomSettingType::Volume;
  if (s == "pan") return CapcomSettingType::Pan;
  if (s == "duration") return CapcomSettingType::Duration;
  if (s == "octave") return CapcomSettingType::Octave;
  if (s == "transpose") return CapcomSettingType::Transpose;
  if (s == "globalTranspose") return CapcomSettingType::GlobalTranspose;
  if (s == "lfo") return CapcomSettingType::LFO;
  if (s == "echo") return CapcomSettingType::Echo;
  if (s == "releaseRate" || s == "release") return CapcomSettingType::ReleaseRate;
  return CapcomSettingType::Unknown;
}

// One open song. Holds the original file bytes so we can save back into the
// same container (SPC or raw .bin) with the edited ARAM patched in.
// Where a ROM-sourced song came from, so IR -> ROM can write back in place.
struct RomContext {
  std::vector<uint8_t> rom;   // full ROM file (incl. any 512-byte copier header)
  uint32_t headerOffset = 0;  // 512 for headered .smc, else 0
  int slot = -1;              // song table slot the sequence was read from
  uint32_t blobPc = 0;        // PC offset (headerless) of [size, loadPtr, seq]
  uint32_t capacity = 0;      // max sequence bytes the slot can hold
  std::vector<uint8_t> origSeq;  // seq bytes as stored (may include tail padding)
  std::filesystem::path path;
};

struct Session {
  std::vector<uint8_t> originalBytes;
  uint32_t aramFileOffset = 0;  // where the 64KB ARAM lives in originalBytes
  bool isSpc = false;
  uint32_t base = 0x0D20;
  bool priorityInHeader = false;
  std::unique_ptr<RomContext> rom;
  std::filesystem::path sourcePath;
  // The user file this song was opened from, and so the default target of a
  // path-less save. Empty for ROM slots, sessions, imports and new songs, where
  // sourcePath is a label or a temp file that must never be overwritten.
  std::filesystem::path userPath;
  std::filesystem::path tempAram;
  std::unique_ptr<CapcomSnesSeq> seq;
  std::unique_ptr<CapcomPianoRollModel> model;
  RawFile* raw = nullptr;  // owned by seq? no - we new it; freed on close

  ~Session() { close(); }

  void close() {
    model.reset();
    seq.reset();
    delete raw;
    raw = nullptr;
    if (!tempAram.empty()) {
      std::error_code ec;
      std::filesystem::remove(tempAram, ec);
    }
  }
};

std::unique_ptr<Session> g_session;

// Every Goof Troop song is uploaded to ARAM $0D20 and nothing else lives
// between there and the SFX bank at $4000 (verified on the driver's ARAM map
// and on all 19 stock SPC dumps: the gap is 0xFF fill). That gap, not the
// song's original footprint or ROM slot, is the real size limit; ROM export
// relocates a song that outgrows its stock slot.
constexpr uint32_t kSongAramBase = 0x0D20;
constexpr uint32_t kSongAramCapacity = 0x4000 - kSongAramBase;  // 13024 bytes

void applySongCapacityFloor(Session& s) {
  if (s.model && s.base == kSongAramBase) {
    s.model->setAllocationFloor(kSongAramCapacity);
  }
}

bool loadSong(const std::string& path, uint32_t base, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot open file";
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  auto s = std::make_unique<Session>();
  s->originalBytes = bytes;
  s->sourcePath = path;
  s->base = base;

  std::vector<uint8_t> aram(kAramSize, 0);
  const bool isSpc = bytes.size() >= kSpcAramFileOffset + kAramSize &&
                     std::memcmp(bytes.data(), kSpcSignature, sizeof(kSpcSignature) - 1) == 0;
  if (isSpc) {
    s->isSpc = true;
    s->aramFileOffset = kSpcAramFileOffset;
    std::copy_n(bytes.begin() + kSpcAramFileOffset, kAramSize, aram.begin());
  } else {
    if (base + bytes.size() > kAramSize) {
      err = "image does not fit in ARAM";
      return false;
    }
    s->aramFileOffset = 0;  // bin: origin is the song image itself
    std::copy(bytes.begin(), bytes.end(), aram.begin() + base);
  }
  s->priorityInHeader = aram[base] == 0x00;

  s->tempAram = operationTempPath("open", ".aram");
  {
    std::ofstream out(s->tempAram, std::ios::binary);
    out.write(reinterpret_cast<const char*>(aram.data()), static_cast<std::streamsize>(aram.size()));
  }

  s->raw = new EditableRawFile(s->tempAram.string());
  if (s->raw->size() != kAramSize) {
    err = "staged ARAM wrong size";
    return false;
  }
  s->seq = std::make_unique<CapcomSnesSeq>(s->raw, CAPCOMSNES_V1_BGM_IN_LIST, base, s->priorityInHeader,
                                           "engine");
  s->seq->parseTrackPointers();
  s->model = std::make_unique<CapcomPianoRollModel>(s->seq.get());
  if (!s->model->reload()) {
    err = "model reload failed";
    return false;
  }
  applySongCapacityFloor(*s);
  g_session = std::move(s);
  return true;
}

// Build the JSON snapshot the UI renders from.
json stateJson() {
  json out;
  if (!g_session || !g_session->model) {
    out["tracks"] = json::array();
    return out;
  }
  auto& m = *g_session->model;
  out["source"] = g_session->sourcePath.filename().string();
  out["writable"] = m.canWrite();
  out["canUndo"] = m.canUndo();
  out["canRedo"] = m.canRedo();

  json programs = json::array();
  for (const auto& [prog, name] : m.programNames()) {
    programs.push_back({{"program", prog}, {"name", name}});
  }
  out["programs"] = programs;

  uint32_t used = 0, budget = 0;
  if (m.byteUsage(&used, &budget)) {
    out["budget"] = {{"used", used}, {"total", budget}};
  }

  // Initial tempo (BPM) from the first Tempo setting, for playhead mapping.
  double tempoBpm = 120.0;
  for (int t = 0; t < m.trackCount(); ++t) {
    const auto* d = m.trackData(t);
    if (!d) continue;
    bool found = false;
    for (const auto& s : d->settings) {
      if (s.type == CapcomSettingType::Tempo) {
        const uint16_t tw = static_cast<uint16_t>((s.value1 << 8) | s.value2);
        if (tw != 0) {
          tempoBpm = 60000000.0 / (48.0 * 125.0 * 64.0 * 2.0) * (tw / 256.0);
        }
        found = true;
        break;
      }
    }
    if (found) break;
  }
  out["tempoBpm"] = tempoBpm;
  out["ppqn"] = 48;
  // Every tempo change in the first pass (tempo is global to the driver), so
  // the UI can map ticks to seconds for songs that change tempo mid-song.
  std::map<uint32_t, double> tempoChanges;

  json tracks = json::array();
  for (int t = 0; t < m.trackCount(); ++t) {
    const auto* data = m.trackData(t);
    json tj;
    tj["index"] = t;
    std::string instr;
    int noteCount = 0;
    json notes = json::array();
    if (data) {
      // The traversal follows the song loop for one extra pass, so every event
      // repeats past the loop tick - clip those so the UI shows one rendition.
      // parseTrack finds the loop as the first GOTO back into already-played
      // bytes; a GOTO into another channel's melody is not a loop and must not
      // hide the rest of the channel.
      uint32_t clipTick = 0xFFFFFFFF;
      if (data->hasSongLoop) {
        tj["songLoop"] = {{"tick", data->songLoopTick}, {"destTick", data->songLoopDestTick}};
        clipTick = data->songLoopTick;
      }
      for (size_t ni = 0; ni < data->notes.size(); ++ni) {
        const auto& n = data->notes[ni];
        if (n.startTick >= clipTick) continue;
        notes.push_back({{"i", ni},
                         {"tick", n.startTick},
                         {"len", n.deltaTicks},
                         {"dur", n.durationTicks},
                         {"timingLengths", CapcomPianoRollModel::supportedNoteLengths(n.dotted)},
                         {"pitch", n.midiKey},
                         {"rest", n.isRest},
                         {"program", n.program},
                         {"loopRepeat", n.isLoopRepeat}});
        if (!n.isRest) {
          ++noteCount;
          if (instr.empty() && !n.instrumentName.empty()) {
            instr = n.instrumentName;
          }
        }
      }
      json settings = json::array();
      for (size_t si = 0; si < data->settings.size(); ++si) {
        const auto& s = data->settings[si];
        if (s.tick >= clipTick) continue;
        if (s.type == CapcomSettingType::Tempo) {
          const uint16_t tw = static_cast<uint16_t>((s.value1 << 8) | s.value2);
          if (tw != 0) {
            tempoChanges[s.tick] = 60000000.0 / (48.0 * 125.0 * 64.0 * 2.0) * (tw / 256.0);
          }
        }
        settings.push_back({{"i", si},
                            {"tick", s.tick},
                            {"type", settingTypeName(s.type)},
                            {"value", s.value1},
                            {"value2", s.value2},
                            {"desc", s.description}});
      }
      tj["settings"] = settings;
      json loops = json::array();
      for (size_t li = 0; li < data->loops.size(); ++li) {
        const auto& l = data->loops[li];
        if (l.tick >= clipTick) continue;
        loops.push_back({{"i", li},
                         {"tick", l.tick},
                         {"slot", l.slot},
                         {"count", l.repeatCount},
                         {"destTick", l.destTick}});
      }
      tj["loops"] = loops;
    }
    tj["instrument"] = instr;
    tj["noteCount"] = noteCount;
    tj["notes"] = notes;
    tracks.push_back(tj);
  }
  out["tracks"] = tracks;
  json tempoMap = json::array();
  if (tempoChanges.empty() || tempoChanges.begin()->first != 0) {
    tempoMap.push_back({{"tick", 0}, {"bpm", tempoBpm}});
  }
  for (const auto& [tick, bpm] : tempoChanges) {
    if (!tempoMap.empty() && tempoMap.back()["bpm"].get<double>() == bpm) {
      continue;  // same tempo re-set: not a change
    }
    tempoMap.push_back({{"tick", tick}, {"bpm", bpm}});
  }
  out["tempoMap"] = tempoMap;
  return out;
}

bool writeFileAtomically(const std::string& path, const std::vector<uint8_t>& bytes, std::string& err);
bool currentSeqBytes(std::vector<uint8_t>& image, uint32_t& base, bool& prio, CapcomSeqIR& ir, std::string& err);

// Save the edited ARAM back into the original container and write to `path`.
bool saveSong(const std::string& path, std::string& err) {
  if (!g_session || !g_session->raw) {
    err = "no song open";
    return false;
  }
  if (path.empty()) {
    err = "save needs a destination path";
    return false;
  }
  std::vector<uint8_t> outBytes = g_session->originalBytes;
  if (g_session->isSpc) {
    for (uint32_t i = 0; i < kAramSize; ++i) {
      outBytes[g_session->aramFileOffset + i] = g_session->raw->readByte(i);
    }
  } else {
    // .bin: the song image region starting at base. It can outgrow the file it
    // came from (the budget is the whole ARAM window), so size the output to
    // the current footprint when that is larger; sizing it to the original
    // file dropped the tail and left track pointers past the end.
    std::vector<uint8_t> image;
    uint32_t imageBase = 0;
    bool prio = false;
    CapcomSeqIR ir;
    if (!currentSeqBytes(image, imageBase, prio, ir, err)) {
      return false;
    }
    outBytes.resize(std::max(outBytes.size(), image.size()));
    for (size_t i = 0; i < outBytes.size(); ++i) {
      outBytes[i] = g_session->raw->readByte(static_cast<uint32_t>(g_session->base + i));
    }
  }
  return writeFileAtomically(path, outBytes, err);
}

// ---- Native SPC playback ---------------------------------------------------

void writeWav(std::vector<uint8_t>& out, const std::vector<int16_t>& stereo, int rate) {
  const uint32_t dataBytes = static_cast<uint32_t>(stereo.size() * 2);
  const uint32_t byteRate = static_cast<uint32_t>(rate) * 2 * 2;
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((v >> (8 * i)) & 0xFF); };
  auto u16 = [&](uint16_t v) { out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF); };
  const char* riff = "RIFF"; out.insert(out.end(), riff, riff + 4);
  u32(36 + dataBytes);
  const char* wave = "WAVE"; out.insert(out.end(), wave, wave + 4);
  const char* fmt = "fmt "; out.insert(out.end(), fmt, fmt + 4);
  u32(16); u16(1); u16(2); u32(rate); u32(byteRate); u16(4); u16(16);
  const char* data = "data"; out.insert(out.end(), data, data + 4);
  u32(dataBytes);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(stereo.data());
  out.insert(out.end(), p, p + dataBytes);
}

bool currentSpc(std::vector<uint8_t>& out, std::string& err);
bool renderSpcWav(const std::vector<uint8_t>& spc, int seconds, int muteMask,
                  std::vector<uint8_t>& wavOut, std::string& err);

// Optimize with an empirical safety net: render before and after, and roll
// the whole pass back (single undo batch) unless the audio is byte-identical.
// The renderer is deterministic, so identical WAVs = proven-equivalent bytes.
// This catches driver subtleties (re-set side effects, slur interactions)
// that static IR reasoning cannot.
bool verifiedOptimize(uint32_t* b0, uint32_t* b1, std::string* err) {
  // Long enough to cover a full pass of any stock song plus its loop point;
  // edits late in a song were never checked at the old 15 s.
  const int kVerifySeconds = 90;
  std::vector<uint8_t> spcBefore;
  if (!currentSpc(spcBefore, *err)) return false;
  uint32_t before = 0, after = 0;
  if (!g_session->model->optimizeSequence(&before, &after, err)) return false;
  if (b0) *b0 = before;
  if (b1) *b1 = after;
  std::vector<uint8_t> wavA, wavB, spcAfter;
  std::string rerr;
  if (!currentSpc(spcAfter, rerr) || spcAfter == spcBefore) {
    return true;  // nothing was rewritten, so optimize pushed no undo batch
  }
  // Bytes were rewritten (optimize pushed exactly one undo batch). A rewrite
  // that saves nothing is pure risk - shared-data songs can even grow - so
  // roll it back without rendering; otherwise the render must match.
  if (after >= before || !renderSpcWav(spcBefore, kVerifySeconds, 0, wavA, rerr) ||
      !renderSpcWav(spcAfter, kVerifySeconds, 0, wavB, rerr) || wavA != wavB) {
    std::string uerr;
    g_session->model->undo(&uerr);  // optimize pushed exactly one undo batch
    if (b1) *b1 = before;
    L_WARN("verifiedOptimize: rolled back (render mismatch or render error)");
  }
  return true;
}

// Move every note of track src onto track dst (instruments pinned per note),
// then erase src's notes. Caller has verified non-overlap. Each primitive is
// individually undoable.
bool mergeTracksInto(int src, int dst, std::string* err) {
  auto* m = g_session->model.get();
  const auto* sd = m->trackData(src);
  if (!sd) { *err = "invalid source track"; return false; }
  struct SrcNote { uint32_t tick; int key; uint32_t len; uint8_t program; CapcomNoteEvent source; };
  std::vector<SrcNote> moves;
  for (const auto& n : sd->notes) {
    if (n.isRest || n.isLoopRepeat || n.midiKey < 0) continue;
    moves.push_back({n.startTick, n.midiKey, n.deltaTicks, n.program, n});
  }
  std::sort(moves.begin(), moves.end(),
            [](const SrcNote& a, const SrcNote& b) { return a.tick < b.tick; });
  for (const auto& mv : moves) {
    uint32_t insertedTick = 0;
    std::string e;
    bool placed = m->InsertNoteAtTick(dst, mv.tick, mv.key, mv.len, &insertedTick, nullptr, &e, &mv.source);
    if (!placed && e.find("No event at this position") != std::string::npos) {
      e.clear();
      placed = m->AppendNoteAtTick(dst, mv.tick, mv.key, mv.len, &e, &mv.source);
      insertedTick = mv.tick;
    }
    if (!placed && e.find("bytes are allocated") != std::string::npos) {
      uint32_t b0 = 0, b1 = 0;
      std::string oe;
      if (verifiedOptimize(&b0, &b1, &oe) && b1 < b0) {
        e.clear();
        placed = m->InsertNoteAtTick(dst, mv.tick, mv.key, mv.len, &insertedTick, nullptr, &e, &mv.source);
        if (!placed && e.find("No event at this position") != std::string::npos) {
          e.clear();
          placed = m->AppendNoteAtTick(dst, mv.tick, mv.key, mv.len, &e, &mv.source);
          insertedTick = mv.tick;
        }
      }
    }
    if (!placed) { *err = "merge stopped: " + e; return false; }
    const auto* dd = m->trackData(dst);
    if (dd) {
      for (size_t i = 0; i < dd->notes.size(); ++i) {
        if (!dd->notes[i].isRest && dd->notes[i].startTick == insertedTick) {
          if (dd->notes[i].program != mv.program) {
            std::string ie;
            if (!m->setInstrument({{dst, static_cast<int>(i)}}, mv.program, &ie)) {
              *err = "merge could not preserve an instrument: " + ie;
              return false;
            }
          }
          break;
        }
      }
    }
  }
  // erase src notes one at a time (indices shift after each erase)
  for (;;) {
    const auto* cur = m->trackData(src);
    if (!cur) break;
    int idx = -1;
    for (size_t i = 0; i < cur->notes.size(); ++i) {
      if (!cur->notes[i].isRest && !cur->notes[i].isLoopRepeat && cur->notes[i].midiKey >= 0) {
        idx = static_cast<int>(i);
        break;
      }
    }
    if (idx < 0) break;
    std::string e;
    if (!m->eraseNotes({{src, idx}}, &e)) { *err = "cleanup stopped: " + e; return false; }
  }
  return true;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64Encode(const std::vector<uint8_t>& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    uint32_t v = in[i] << 16;
    if (i + 1 < in.size()) v |= in[i + 1] << 8;
    if (i + 2 < in.size()) v |= in[i + 2];
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += (i + 1 < in.size()) ? kB64[(v >> 6) & 63] : '=';
    out += (i + 2 < in.size()) ? kB64[v & 63] : '=';
  }
  return out;
}
bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  out.clear();
  int buf = 0, bits = 0;
  for (char c : in) {
    if (c == '=') break;
    const int v = val(c);
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
    }
  }
  return !out.empty();
}

// Render an SPC image to a stereo 32kHz WAV. muteMask bits mute DSP voices.
// The snapshot's driver state is mid-song (whatever the dump captured), so we
// send the driver command 0xFB — "restart current song" in the Capcom
// protocol (see disassembly CODE_8099E3) — which re-reads the $0D20 header
// and re-inits every channel. That makes edited/injected sequences play
// correctly from the top instead of resuming stale per-channel pointers.
bool renderSpcWav(const std::vector<uint8_t>& spc, int seconds, int muteMask,
                  std::vector<uint8_t>& wavOut, std::string& err) {
  static Snes_Spc s_spc;
  static SPC_Filter s_filter;
  if (s_spc.init()) {  // returns error string (non-null) on failure
    err = "spc init failed";
    return false;
  }
  if (const char* e = s_spc.load_spc(spc.data(), static_cast<long>(spc.size()))) {
    err = std::string("load_spc: ") + e;
    return false;
  }
  s_spc.clear_echo();
  s_spc.mute_voices(muteMask);
  s_filter.clear();
  // Restart the song: param on port1, 0xFB command on port0 (SNES-side order).
  s_spc.write_port(0, 1, 0x00);
  s_spc.write_port(0, 3, 0x00);
  s_spc.write_port(0, 0, 0xFB);

  const int rate = Snes_Spc::sample_rate;
  const long total = static_cast<long>(seconds) * rate * 2;  // stereo samples
  std::vector<int16_t> buf(static_cast<size_t>(total), 0);
  long done = 0;
  while (done < total) {
    int chunk = static_cast<int>(std::min<long>(2048, total - done));
    if (const char* e = s_spc.play(chunk, buf.data() + done)) {
      err = std::string("play: ") + e;
      return false;
    }
    s_filter.run(buf.data() + done, chunk);
    done += chunk;
  }
  writeWav(wavOut, buf, rate);
  return true;
}

// The current song as a full SPC (original container + edited ARAM patched in).
bool currentSpc(std::vector<uint8_t>& out, std::string& err) {
  if (!g_session || !g_session->raw) {
    err = "no song";
    return false;
  }
  if (!g_session->isSpc) {
    err = "playback requires an .spc song";
    return false;
  }
  out = g_session->originalBytes;
  for (uint32_t i = 0; i < kAramSize; ++i) {
    out[g_session->aramFileOffset + i] = g_session->raw->readByte(i);
  }
  return true;
}

// Build a one-note preview SPC: keep the loaded song's samples/driver, replace
// the sequence with a single sustained note using the given instrument/pitch.
bool previewNoteSpc(int program, uint8_t octave, bool octaveUp, uint8_t durationRate,
                    uint8_t statusByte, std::vector<uint8_t>& out, std::string& err) {
  if (!currentSpc(out, err)) {
    return false;
  }
  const uint32_t base = g_session->base;
  const uint32_t off = g_session->aramFileOffset;
  const bool prio = g_session->priorityInHeader;
  const uint32_t hdr = base + (prio ? 1 : 0);

  std::vector<uint8_t> seq;
  seq.push_back(0x05); seq.push_back(0x01); seq.push_back(0x80);  // tempo
  seq.push_back(0x07); seq.push_back(0xFF);                       // volume
  seq.push_back(0x19); seq.push_back(0xFF);                       // master volume
  seq.push_back(0x08); seq.push_back(static_cast<uint8_t>(program));
  seq.push_back(0x09); seq.push_back(octave);
  if (octaveUp) { seq.push_back(0x03); }
  seq.push_back(0x06); seq.push_back(durationRate ? durationRate : 0xC0);
  // Force a long note so the preview is audible; keep the note's key index.
  seq.push_back(static_cast<uint8_t>((7u << 5) | (statusByte & 0x1F)));
  seq.push_back(0x17);  // end
  const uint32_t dataAddr = hdr + 16;
  const uint32_t endAddr = dataAddr + static_cast<uint32_t>(seq.size());

  auto put16be = [&](uint32_t aramAddr, uint16_t v) {
    out[off + aramAddr] = (v >> 8) & 0xFF;
    out[off + aramAddr + 1] = v & 0xFF;
  };
  for (int i = 0; i < 8; ++i) {
    put16be(hdr + i * 2, static_cast<uint16_t>(i == 0 ? dataAddr : endAddr));
  }
  for (size_t i = 0; i < seq.size(); ++i) {
    out[off + dataAddr + i] = seq[i];
  }
  out[off + endAddr] = 0x17;
  // Kill voices still ringing from the snapshot's dump moment: clear KON and
  // assert KOFF in the SPC image's DSP register block, so the preview note
  // starts from silence instead of over a popping chord.
  if (out.size() >= 0x10180) {
    out[0x10100 + 0x4C] = 0x00;
    out[0x10100 + 0x5C] = 0xFF;
  }
  return true;
}

std::filesystem::path renderToTemp(const std::vector<uint8_t>& wav, const char* tag) {
  // PID-suffixed so two app instances don't clobber each other's renders
  static const std::string pid = std::to_string(::_getpid());
  auto p = std::filesystem::temp_directory_path() /
           (std::string("gtb-render-") + tag + "-" + pid + ".wav");
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  return p;
}

// Locate an ffmpeg binary: explicit override, bundled beside the engine exe,
// then PATH. The Tauri app bundles ffmpeg as a resource and sets GTB_FFMPEG.
std::filesystem::path exeDir();

std::string findFfmpeg() {
  if (const char* e = std::getenv("GTB_FFMPEG")) {
    if (*e) return e;
  }
  std::error_code ec;
  // beside the engine exe as documented - not the working directory, which
  // depends on how the engine was launched
  const auto dir = exeDir();
  for (const char* name : {"ffmpeg.exe", "ffmpeg"}) {
    auto cand = dir / name;
    if (std::filesystem::exists(cand, ec)) return cand.string();
  }
  return "ffmpeg";  // rely on PATH
}

// Run a shell command with no visible console window. std::system() on
// Windows pops a cmd.exe console (the engine itself runs windowless, so its
// children would each allocate a fresh visible console).
int runHidden(const std::string& shellCmd) {
#ifdef _WIN32
  std::string full = "cmd.exe /C " + shellCmd;
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<char> buf(full.begin(), full.end());
  buf.push_back(0);
  if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return static_cast<int>(code);
#else
  return std::system(shellCmd.c_str());
#endif
}

// Transcode a WAV file to MP3 via ffmpeg. On Windows, std::system runs through
// cmd.exe, which needs the whole command wrapped in an extra pair of quotes
// when both the program and an argument are quoted.
bool wavToMp3(const std::filesystem::path& wav, const std::filesystem::path& mp3,
              const std::string& bitrate, std::string& err) {
  const std::string ff = findFfmpeg();
  std::string cmd = "\"\"" + ff + "\" -y -loglevel error -i \"" + wav.string() +
                    "\" -codec:a libmp3lame -b:a " + bitrate + " \"" + mp3.string() + "\"\"";
  const int rc = runHidden(cmd);
  if (rc != 0) {
    err = "ffmpeg exited with code " + std::to_string(rc) + " (is ffmpeg installed / GTB_FFMPEG set?)";
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::exists(mp3, ec)) {
    err = "ffmpeg produced no output";
    return false;
  }
  return true;
}

// ---- Format converters (ASM / MIDI) ---------------------------------------
//
// The lossless hub is the serialized Capcom sequence image (the exact ARAM
// bytes). Everything routes through it: X -> bytes -> parseFromImage -> IR,
// and IR -> serialize -> bytes -> Y.

std::string hex2(uint8_t v) {
  std::ostringstream s;
  s << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(v);
  return s.str();
}
std::string hex4(uint32_t v) {
  std::ostringstream s;
  s << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << (v & 0xFFFFu);
  return s.str();
}

// Grab the current song's serialized sequence bytes (image[k] == ARAM addr
// base+k) plus a fresh IR over those same bytes.
bool currentSeqBytes(std::vector<uint8_t>& image, uint32_t& base, bool& prio,
                     CapcomSeqIR& ir, std::string& err) {
  if (!g_session || !g_session->model || !g_session->raw) {
    err = "no song open";
    return false;
  }
  base = g_session->base;
  prio = g_session->priorityInHeader;
  if (!ir.parseFromImage(g_session->raw, base, prio)) {
    err = "sequence parse failed";
    return false;
  }
  if (!ir.serializeToBuffer(g_session->raw, &image, &err)) {
    return false;
  }
  return true;
}

std::string asmMnemonic(const CapcomCmdIR& c) {
  switch (c.type) {
    case CapcomCmdType::Note:
      return "note key=" + std::to_string(c.keyIndex) + " len=" + std::to_string(c.lenIndex);
    case CapcomCmdType::Rest: return "rest len=" + std::to_string(c.lenIndex);
    case CapcomCmdType::ToggleTriplet: return "toggle triplet";
    case CapcomCmdType::ToggleSlur: return "toggle slur";
    case CapcomCmdType::DottedNoteOn: return "dotted note";
    case CapcomCmdType::ToggleOctaveUp: return "octave up toggle";
    case CapcomCmdType::NoteAttributes: return "note attributes";
    case CapcomCmdType::Tempo: return "tempo";
    case CapcomCmdType::Duration: return "duration rate=" + std::to_string(c.durationRate);
    case CapcomCmdType::Volume: return "volume";
    case CapcomCmdType::Pan: return "pan";
    case CapcomCmdType::MasterVolume: return "master volume";
    case CapcomCmdType::ProgramChange: return "program " + std::to_string(c.program);
    case CapcomCmdType::Octave: return "octave";
    case CapcomCmdType::GlobalTranspose: return "global transpose " + std::to_string(c.globalTranspose);
    case CapcomCmdType::Transpose: return "transpose " + std::to_string(c.transpose);
    case CapcomCmdType::Tuning: return "tuning";
    case CapcomCmdType::PortamentoTime: return "portamento time";
    case CapcomCmdType::RepeatUntil:
      return "repeat_until slot=" + std::to_string(c.repeatSlot) + " count=" +
             std::to_string(c.repeatCount) + " -> $" + hex4(c.destWord);
    case CapcomCmdType::RepeatBreak: return "repeat_break -> $" + hex4(c.destWord);
    case CapcomCmdType::Goto: return "goto $" + hex4(c.destWord);
    case CapcomCmdType::End: return "end";
    case CapcomCmdType::LFO: return "lfo";
    case CapcomCmdType::EchoParam: return "echo param";
    case CapcomCmdType::EchoOnOff: return "echo on/off";
    case CapcomCmdType::ReleaseRate: return "release rate";
    default: return "cmd $" + hex2(c.statusByte);
  }
}

// Disassemble the current song to labeled WLA-style .db/.dw ASM. Byte-lossless:
// data lines carry verbatim bytes; header pointers become .dw <label> and jump
// destinations get labels, so the mini-assembler reproduces the exact image.
bool exportAsmText(std::string& out, std::string& err) {
  std::vector<uint8_t> image;
  uint32_t base;
  bool prio;
  CapcomSeqIR ir;
  if (!currentSeqBytes(image, base, prio, ir, err)) return false;

  const uint32_t hdr = base + (prio ? 1 : 0);
  const uint32_t bodyStart = hdr + 16;
  const uint32_t seqEnd = base + static_cast<uint32_t>(image.size());
  auto at = [&](uint32_t a) -> uint8_t { return image[a - base]; };

  std::array<uint32_t, 8> ch{};
  std::map<uint32_t, std::string> label;
  for (int i = 0; i < 8; ++i) {
    ch[i] = static_cast<uint32_t>((at(hdr + 2 * i) << 8) | at(hdr + 2 * i + 1));
    if (!label.count(ch[i])) label[ch[i]] = "Channel" + std::to_string(i + 1);
  }
  std::map<uint32_t, const CapcomCmdIR*> cmdAt;
  for (int t = 0; t < CapcomSeqIR::MAX_TRACKS; ++t) {
    const CapcomTrackIR* trk = ir.track(t);
    if (!trk) continue;
    for (const auto& c : trk->cmds) {
      cmdAt[c.origAbsOffset] = &c;
      const bool jumps = c.type == CapcomCmdType::Goto || c.type == CapcomCmdType::RepeatUntil ||
                         c.type == CapcomCmdType::RepeatBreak;
      if (jumps && c.destWord >= base && c.destWord < seqEnd && !label.count(c.destWord)) {
        label[c.destWord] = "L_" + hex4(c.destWord);
      }
    }
  }

  std::ostringstream o;
  o << "; Goof Troop Boop - Capcom SPC700 music (lossless ASM)\n";
  o << "; Reassemble with the same tool: Import > ASM.\n";
  o << "; Data-only dialect: db/.db, dw/.dw (big-endian), labels, .base, numeric aliases.\n";
  o << "; Aliases: !instrument_16 = #$01 then db $08, !instrument_16\n";
  o << "; Import maps resolved program values (1 above), not alias name suffixes (16).\n";
  o << "; External samples/drivers are not imported; review the target bank before importing.\n\n";
  o << ".base $" << hex4(base) << "\n\n";
  if (prio) o << "\t.db $" << hex2(at(base)) << "        ; song priority/type byte\n";
  o << "; channel pointer table (8 x 16-bit big-endian)\n";
  for (int i = 0; i < 8; ++i) o << "\t.dw " << label[ch[i]] << "\n";
  o << "\n";

  std::vector<uint8_t> run;
  auto flushRun = [&]() {
    if (run.empty()) return;
    o << "\t.db ";
    for (size_t k = 0; k < run.size(); ++k) {
      if (k) o << ", ";
      o << "$" << hex2(run[k]);
    }
    o << "\n";
    run.clear();
  };
  uint32_t a = bodyStart;
  while (a < seqEnd) {
    if (label.count(a)) {
      flushRun();
      o << label[a] << ":\n";
    }
    auto it = cmdAt.find(a);
    if (it != cmdAt.end()) {
      flushRun();
      const CapcomCmdIR* c = it->second;
      uint32_t n = c->sizeBytes ? c->sizeBytes : 1;
      o << "\t.db ";
      for (uint32_t k = 0; k < n && a + k < seqEnd; ++k) {
        if (k) o << ", ";
        o << "$" << hex2(at(a + k));
      }
      o << "    ; " << asmMnemonic(*c) << "\n";
      a += n;
    } else {
      run.push_back(at(a));
      ++a;
    }
  }
  flushRun();
  out = o.str();
  return true;
}

uint32_t parseNum(const std::string& tok) {
  std::string t = tok;
  if (!t.empty() && t[0] == '$') return static_cast<uint32_t>(std::strtoul(t.c_str() + 1, nullptr, 16));
  return static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10));
}

// Strict data-only dialect. Never evaluate expressions or execute assembler code.
std::string asmTrim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

bool asmNumber(std::string token, uint32_t& value) {
  if (!token.empty() && token[0] == '#') {
    token.erase(0, 1);
  }
  unsigned radix = 10;
  if (!token.empty() && token[0] == '$') {
    radix = 16;
    token.erase(0, 1);
  } else if (token.size() > 2 && token.substr(0, 2) == "0x") {
    radix = 16;
    token.erase(0, 2);
  }
  if (token.empty()) {
    return false;
  }
  value = 0;
  for (char c : token) {
    const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                           c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
    if (digit >= radix || value > (65535u - digit) / radix) {
      return false;
    }
    value = value * radix + digit;
  }
  return true;
}

bool asmSymbol(std::string name) {
  if (!name.empty() && name[0] == '!') {
    name.erase(0, 1);
  }
  if (name.empty() || (name[0] >= '0' && name[0] <= '9')) {
    return false;
  }
  return name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
         std::string::npos;
}

// Assemble .db/db, .dw/dw (big-endian), labels, .base and numeric aliases.
// bytesOut[0] corresponds to ARAM address `baseOut`.
bool assembleAsm(const std::string& text, uint32_t& baseOut, std::vector<uint8_t>& bytesOut,
                 std::string& err, std::map<uint32_t, unsigned>* lineOffsets = nullptr) {
  struct Line { std::string op; std::vector<std::string> args; std::string label; unsigned number; };
  std::vector<Line> lines;
  std::map<std::string, uint32_t> aliases;
  uint32_t base = 0x0D20;
  bool baseSet = false;
  unsigned number = 0;
  // CapcomToASM's exact upload wrapper is recognized as a file format, not
  // evaluated as ASM. Discard its four-byte ROM upload header; channel addresses
  // are ARAM-relative to Channels, as verified against local SPC sequence bytes.
  unsigned wrapper = 0;
  bool patchWrapper = false;
  std::string patchLabel;
  auto fail = [&](const std::string& reason) {
    err = "ASM line " + std::to_string(number) + ": " + reason;
    return false;
  };
  std::istringstream in(text);
  std::string raw;
  while (std::getline(in, raw)) {
    ++number;
    auto sc = raw.find(';');
    std::string line = asmTrim(raw.substr(0, sc));
    if (line.empty()) {
      continue;
    }
    std::string compact;
    for (char c : line) {
      if (c != ' ' && c != '\t') {
        compact.push_back(c);
      }
    }
    if (compact == "lorom" && !baseSet && lines.empty() && aliases.empty() && wrapper == 0) {
      wrapper = 1;
      continue;
    }
    // Recognize the bounded song-table patch envelope as metadata only. Never
    // execute org/dl, evaluate arbitrary expressions, or modify a ROM on import.
    if (wrapper == 1 && compact.rfind("org$848000+($", 0) == 0) {
      uint32_t slot;
      if (patchWrapper || compact.size() != 18 || compact.substr(15) != "*3)" ||
          !asmNumber("$" + compact.substr(13, 2), slot) || slot >= 0x30) {
        return fail("invalid song-table patch slot");
      }
      patchWrapper = true;
      wrapper = 100;
      continue;
    }
    if (wrapper == 100) {
      if (compact.size() <= 8 || compact.substr(0, 2) != "dl" ||
          compact.substr(compact.size() - 6) != "-$8000") {
        return fail("expected song-label relocation metadata");
      }
      patchLabel = compact.substr(2, compact.size() - 8);
      if (!asmSymbol(patchLabel) || patchLabel[0] == '!') {
        return fail("invalid song-label relocation metadata");
      }
      wrapper = 1;
      continue;
    }
    if (wrapper >= 101 && wrapper <= 104) {
      bool valid = false;
      switch (wrapper) {
        case 101:
          valid = compact.substr(0, 10) == "!ARAMAddr=" && asmNumber(compact.substr(10), base);
          baseSet = valid;
          break;
        case 102: valid = compact == "dwEndOfSong-SongStart"; break;
        case 103: valid = compact == "dw!ARAMAddr"; break;
        case 104: valid = compact == "SongStart:"; break;
      }
      if (!valid) {
        return fail("unsupported song patch upload header");
      }
      wrapper = wrapper == 104 ? 7 : wrapper + 1;
      continue;
    }
    if (wrapper > 0 && wrapper < 9) {
      bool valid = false;
      switch (wrapper) {
        case 1:
          valid = compact == "functionBigEndian(n)=(((n&$ff00)>>8)|((n&$00ff)<<8))";
          break;
        case 2: {
          uint32_t high, low;
          valid = compact.size() == 10 && compact.substr(0, 4) == "org$" &&
                  asmNumber("$" + compact.substr(4, 2), high) &&
                  asmNumber("$" + compact.substr(6), low);
          break;
        }
        case 3:
          if (patchWrapper) {
            if (compact != patchLabel + ":") {
              return fail("relocation label does not match upload label");
            }
            wrapper = 101;
            continue;
          }
          valid = compact.substr(0, 10) == "!ARAMAddr=" && asmNumber(compact.substr(10), base);
          baseSet = valid;
          break;
        case 4:
          if (compact == "dwEndOfSong-SongStart") {
            wrapper = 103;
            continue;
          }
          valid = compact == "SongStart:";
          break;
        case 5: valid = compact == "dwSongStart-EndOfSong"; break;
        case 6: valid = compact == "dw!ARAMAddr"; break;
        case 7: valid = compact == "Channels:"; break;
        case 8: valid = compact == "!ARAMC=!ARAMAddr-SongStart"; break;
      }
      if (!valid) {
        return fail("unsupported CapcomToASM wrapper; expected canonical upload header");
      }
      ++wrapper;
      continue;
    }
    if (wrapper >= 9 && wrapper < 17) {
      const std::string channel = "Channel0" + std::to_string(wrapper - 9);
      if (compact != "dwBigEndian(" + channel + "+!ARAMC)") {
        return fail("expected eight canonical BigEndian channel pointers");
      }
      lines.push_back({".dw", {channel}, "", number});
      ++wrapper;
      continue;
    }
    const auto equal = line.find('=');
    if (equal != std::string::npos) {
      const auto name = asmTrim(line.substr(0, equal));
      uint32_t value;
      if (!asmSymbol(name) || !asmNumber(asmTrim(line.substr(equal + 1)), value)) {
        return fail("expected numeric alias, e.g. !instrument_16 = #$01");
      }
      if (aliases.count(name) && aliases[name] != value) {
        return fail("conflicting alias: " + name);
      }
      aliases[name] = value;
      continue;
    }
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
      const auto name = asmTrim(line.substr(0, colon));
      if (!asmSymbol(name)) {
        return fail("invalid label: " + name);
      }
      lines.push_back({"label", {}, name, number});
      line = asmTrim(line.substr(colon + 1));
      if (line.empty()) {
        continue;
      }
    }
    std::istringstream ls(line);
    std::string op;
    ls >> op;
    std::string lop = op;
    std::transform(lop.begin(), lop.end(), lop.begin(), ::tolower);
    if (lop == "db" || lop == "dw") {
      lop = "." + lop;
    }
    std::string rest;
    std::getline(ls, rest);
    std::vector<std::string> args;
    std::string tok;
    std::istringstream as(rest);
    while (std::getline(as, tok, ',')) {
      tok = asmTrim(tok);
      if (tok.empty()) {
        return fail("empty operand");
      }
      args.push_back(tok);
    }
    if (args.empty() || (!asmTrim(rest).empty() && asmTrim(rest).back() == ',')) {
      return fail("missing operand");
    }
    if (lop == ".base") {
      if (baseSet || !lines.empty() || args.size() != 1 || !asmNumber(args[0], base)) {
        return fail(".base requires one 16-bit literal before all labels/data, once only");
      }
      baseSet = true;
    } else if (lop == ".db" || lop == ".dw") {
      lines.push_back({lop, args, "", number});
    } else {
      return fail("unsupported directive: " + op + "; use db/dw, labels, .base and numeric aliases");
    }
  }
  std::map<std::string, uint32_t> labels = aliases;
  if (wrapper && wrapper != 17) {
    return fail("incomplete CapcomToASM upload wrapper");
  }
  uint32_t off = 0;
  for (const auto& l : lines) {
    number = l.number;
    if (l.op == "label") {
      if (labels.count(l.label)) {
        return fail("duplicate label or alias collision: " + l.label);
      }
      labels[l.label] = base + off;
    } else {
      const size_t size = l.args.size() * (l.op == ".dw" ? 2 : 1);
      if (size > 65536u - base - off) {
        return fail("data exceeds ARAM");
      }
      off += static_cast<uint32_t>(size);
    }
  }
  std::vector<uint8_t> bytes;
  if (lineOffsets) {
    lineOffsets->clear();
  }
  for (const auto& l : lines) {
    number = l.number;
    if (lineOffsets && !l.args.empty()) {
      (*lineOffsets)[static_cast<uint32_t>(bytes.size())] = number;
    }
    for (const auto& a : l.args) {
      uint32_t v;
      if (labels.count(a)) {
        v = labels[a];
      } else if (!asmNumber(a, v)) {
        return fail("undefined alias/label or invalid literal: " + a);
      }
      if (v > (l.op == ".db" ? 255u : 65535u)) {
        return fail("operand out of range: " + a);
      }
      if (l.op == ".dw") {
        bytes.push_back(static_cast<uint8_t>(v >> 8));
      }
      bytes.push_back(static_cast<uint8_t>(v));
    }
  }
  if (bytes.empty()) {
    return fail("no sequence data");
  }
  baseOut = base;
  bytesOut = std::move(bytes);
  return true;
}

// Directory containing this engine executable: the bundled sidecar set
// (soundbank.spc, GTBoop-cli.exe, optional ffmpeg.exe) lives beside it.
std::filesystem::path exeDir() {
#ifdef _WIN32
  char buf[MAX_PATH]{};
  if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
    return std::filesystem::path(buf).parent_path();
  }
#endif
  return std::filesystem::current_path();
}

// Default sound bank (driver + samples) used to make sequence-only imports
// (ASM/MIDI/ROM/.bin) immediately playable, and as the default song. The
// Tauri shell sets GTB_SOUNDBANK to the bundled soundbank.spc; otherwise the
// copy beside the engine executable is used.
std::string soundBankPath() {
  if (const char* e = std::getenv("GTB_SOUNDBANK")) {
    if (*e) return e;
  }
  return (exeDir() / "soundbank.spc").string();
}

// Build a playable session from a raw sequence image (bytes[0] == ARAM addr
// `base`): inject it into a sound-bank SPC (the current song if it is an SPC,
// else the default bank) so it can be played and re-exported to any format.
bool loadSequenceImage(const std::vector<uint8_t>& seqBytes, uint32_t base,
                       const std::string& sourceName, std::string& err) {
  std::vector<uint8_t> container;
  uint32_t aramOff = kSpcAramFileOffset;
  if (g_session && g_session->isSpc && !g_session->originalBytes.empty()) {
    container = g_session->originalBytes;
    aramOff = g_session->aramFileOffset;
  } else {
    std::ifstream in(soundBankPath(), std::ios::binary);
    if (!in) { err = "sound bank not found: " + soundBankPath(); return false; }
    container.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (container.size() < aramOff + kAramSize ||
        std::memcmp(container.data(), kSpcSignature, sizeof(kSpcSignature) - 1) != 0) {
      err = "sound bank is not a valid SPC";
      return false;
    }
  }
  if (aramOff + base + seqBytes.size() > container.size()) {
    err = "sequence does not fit in ARAM";
    return false;
  }
  for (size_t i = 0; i < seqBytes.size(); ++i) {
    container[aramOff + base + i] = seqBytes[i];
  }

  std::vector<uint8_t> aram(kAramSize, 0);
  std::copy_n(container.begin() + aramOff, kAramSize, aram.begin());

  auto s = std::make_unique<Session>();
  s->originalBytes = container;
  s->isSpc = true;
  s->aramFileOffset = aramOff;
  s->base = base;
  s->sourcePath = sourceName;
  s->priorityInHeader = aram[base] == 0x00;
  s->tempAram = operationTempPath("import", ".aram");
  {
    std::ofstream out(s->tempAram, std::ios::binary);
    out.write(reinterpret_cast<const char*>(aram.data()), static_cast<std::streamsize>(aram.size()));
  }
  s->raw = new EditableRawFile(s->tempAram.string());
  if (s->raw->size() != kAramSize) { err = "staged ARAM wrong size"; return false; }
  s->seq = std::make_unique<CapcomSnesSeq>(s->raw, CAPCOMSNES_V1_BGM_IN_LIST, base, s->priorityInHeader, "engine");
  s->seq->parseTrackPointers();
  s->model = std::make_unique<CapcomPianoRollModel>(s->seq.get());
  if (!s->model->reload()) { err = "model reload failed after import"; return false; }
  applySongCapacityFloor(*s);
  g_session = std::move(s);
  return true;
}

// ASM-only preflight: all work is on private bytes. Neither review nor failure
// touches the current session, its undo stack, or its source/ROM association.
bool prepareAsmImport(std::vector<uint8_t>& bytes, uint32_t base, const json& mappings,
                      bool review, json& report, std::string& err) {
  if (base != kSongAramBase || bytes.size() > kSongAramCapacity || bytes.size() < 17) {
    err = "ASM must contain a complete Goof Troop sequence at .base $0D20, within $0D20..$3FFF";
    return false;
  }
  const bool priority = bytes[0] == 0;
  const size_t header = priority ? 1 : 0;
  for (size_t t = 0; t < 8; ++t) {
    const uint32_t pointer = (bytes[header + t * 2] << 8) | bytes[header + t * 2 + 1];
    if (pointer < base + header + 16 || pointer >= base + bytes.size()) {
      err = "ASM channel " + std::to_string(t + 1) + " pointer is outside the supplied sequence";
      return false;
    }
  }
  std::vector<uint8_t> bank;
  uint32_t aramOff = kSpcAramFileOffset;
  if (g_session && g_session->isSpc) {
    bank = g_session->originalBytes;
    aramOff = g_session->aramFileOffset;
    report["targetBank"] = g_session->sourcePath.string();
  } else {
    std::ifstream in(soundBankPath(), std::ios::binary);
    bank.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    report["targetBank"] = soundBankPath();
  }
  if (bank.size() < aramOff + kAramSize + 128 ||
      std::memcmp(bank.data(), kSpcSignature, sizeof(kSpcSignature) - 1) != 0) {
    err = "ASM target bank must be a valid SPC containing ARAM and DSP registers";
    return false;
  }
  VirtFile bankRaw(bank.data() + aramOff, kAramSize, "asm-bank");
  // Same non-executing signature as CapcomSnesScanner::ptnLoadInstrTableAddress.
  const uint8_t pattern[] = {0x8d, 0x06, 0xcf, 0xda, 0, 0x60, 0x98, 0, 0, 0x98, 0, 0};
  uint32_t table = 0;
  for (uint32_t i = 0; i + sizeof(pattern) <= kAramSize; ++i) {
    bool match = true;
    for (size_t k = 0; k < sizeof(pattern); ++k) {
      if (pattern[k] && bankRaw.readByte(i + k) != pattern[k]) {
        match = false;
      }
    }
    if (match) {
      const uint32_t found = bankRaw.readByte(i + 7) | (bankRaw.readByte(i + 10) << 8);
      if (table && table != found) {
        err = "ASM target bank has ambiguous instrument tables";
        return false;
      }
      table = found;
    }
  }
  if (!table) {
    err = "ASM target bank has no supported Capcom instrument table; open a Goof Troop SPC first";
    return false;
  }
  const uint32_t dir = bank[aramOff + kAramSize + 0x5d] << 8;
  std::map<unsigned, bool> available;
  report["targetPrograms"] = json::array();
  report["silentPrograms"] = json::array();
  for (unsigned program = 0; program <= 255; ++program) {
    const uint32_t entry = table + 6 * program;
    if (entry + 6 > kAramSize) {
      break;
    }
    bool blank = true;
    for (unsigned k = 0; k < 6; ++k) {
      const auto value = bankRaw.readByte(entry + k);
      if (value != 0 && value != 255) {
        blank = false;
      }
    }
    if (blank) {
      continue;
    }
    if (!CapcomSnesInstr::isValidHeader(&bankRaw, entry, dir, false)) {
      // The stock Goof Troop bank's $17 entry deliberately sets ADSR/GAIN
      // to zero. It is used to mute a part, not as a sampled instrument.
      // Qualify the exact location/header; do not admit arbitrary garbage
      // after the instrument table or accept zero envelopes in other banks.
      const uint8_t muteHeader[] = {2, 0, 0, 0, 0, 0};
      bool isMute = program == 0x17 && table == 0x505C && dir == 0x5000;
      for (unsigned k = 0; isMute && k < sizeof(muteHeader); ++k) {
        isMute = bankRaw.readByte(entry + k) == muteHeader[k];
      }
      if (isMute) {
        available[program] = true;
        report["targetPrograms"].push_back(program);
        report["silentPrograms"].push_back(program);
      }
      break;
    }
    if (CapcomSnesInstr::isValidHeader(&bankRaw, entry, dir, true)) {
      available[program] = true;
      report["targetPrograms"].push_back(program);
    }
  }
  if (!mappings.is_array()) {
    err = "programMap must be an array of {from,to} byte values";
    return false;
  }
  std::map<unsigned, unsigned> map;
  for (const auto& entry : mappings) {
    if (!entry.is_object() || !entry.contains("from") || !entry.contains("to") ||
        !entry["from"].is_number_integer() || !entry["to"].is_number_integer() ||
        entry["from"] < 0 || entry["from"] > 255 || entry["to"] < 0 || entry["to"] > 255) {
      err = "programMap requires integer from/to values in 0..255";
      return false;
    }
    const unsigned from = entry["from"].get<unsigned>();
    const unsigned to = entry["to"].get<unsigned>();
    if ((map.count(from) && map[from] != to) || !available.count(to)) {
      err = "conflicting map or unavailable target-bank program: " + std::to_string(from) +
            " -> " + std::to_string(to);
      return false;
    }
    map[from] = to;
  }
  // Zero outside the supplied image: never let a malformed track consume old song bytes.
  std::vector<uint8_t> aram(kAramSize, 0);
  std::copy(bytes.begin(), bytes.end(), aram.begin() + base);
  VirtFile raw(aram.data(), kAramSize, "asm-sequence");
  std::map<unsigned, unsigned> used;
  std::map<uint32_t, unsigned> patches;
  unsigned notes = 0;
  unsigned inactiveExternalJumps = 0;
  for (int t = 0; t < 8; ++t) {
    CapcomTrackTraversalResult traversal;
    const uint32_t start = raw.readShortBE(base + header + t * 2);
    if (!CapcomTrackTraversal::Traverse(&raw, start, &traversal, &err, true)) {
      err = "ASM channel " + std::to_string(t + 1) + ": " + err;
      return false;
    }
    bool hasProgram = false;
    for (size_t stepIndex = 0; stepIndex < traversal.steps.size(); ++stepIndex) {
      const auto& step = traversal.steps[stepIndex];
      const auto& cmd = step.cmd;
      if (cmd.origAbsOffset < base + header + 16 ||
          cmd.origAbsOffset + cmd.sizeBytes > base + bytes.size() || cmd.type == CapcomCmdType::Unknown) {
        err = "ASM contains a truncated/unsupported command or leaves the supplied sequence";
        return false;
      }
      if (cmd.type == CapcomCmdType::ProgramChange) {
        hasProgram = true;
        const unsigned program = cmd.params.at(0);  // traversal's cmd.program is the pre-command state
        ++used[program];
        const unsigned target = map.count(program) ? map[program] : program;
        patches[cmd.origAbsOffset + 1 - base] = target;
      }
      if (cmd.type == CapcomCmdType::Note) {
        ++notes;
        if (!hasProgram) {
          err = "ASM channel " + std::to_string(t + 1) +
                " has notes before a program command; add db $08, <program> at its start";
          return false;
        }
      }
      if (cmd.type == CapcomCmdType::Goto || cmd.type == CapcomCmdType::RepeatUntil ||
          cmd.type == CapcomCmdType::RepeatBreak) {
        if (cmd.destWord < base + header + 16 || cmd.destWord >= base + bytes.size()) {
          // An untaken repeat-break still has two operand bytes, but the
          // driver never dereferences them. Retain those bytes verbatim.
          // A taken branch is independently bounded by its next trace step.
          const bool fallsThrough = stepIndex + 1 < traversal.steps.size() &&
              traversal.steps[stepIndex + 1].cmd.origAbsOffset == cmd.origAbsOffset + cmd.sizeBytes;
          if (cmd.type != CapcomCmdType::RepeatBreak || !fallsThrough) {
            err = "ASM jump leaves the supplied sequence";
            return false;
          }
          ++inactiveExternalJumps;
        }
      }
    }
  }
  report["sourcePrograms"] = json::array();
  report["unmappedPrograms"] = json::array();
  for (const auto& [program, count] : used) {
    report["sourcePrograms"].push_back(program);
    const unsigned target = map.count(program) ? map[program] : program;
    if (!available.count(target)) {
      report["unmappedPrograms"].push_back(program);
    }
  }
  for (const auto& [from, to] : map) {
    if (!used.count(from)) {
      err = "mapped source program " + std::to_string(from) + " is not used (map resolved alias values)";
      return false;
    }
  }
  report["noteCommands"] = notes;
  report["inactiveExternalJumps"] = inactiveExternalJumps;
  report["warnings"] = json::array({"ASM imports sequence data only, not external samples or drivers. "
                                    "Valid programs do not guarantee audible output; audition after import."});
  if (inactiveExternalJumps) {
    report["warnings"].push_back("Untaken conditional branches contain external addresses. Their bytes are "
                                  "preserved. Structural edits are accepted only while those branches remain untaken.");
  }
  if (!notes) {
    err = "ASM has no note commands; refusing a silent import";
    return false;
  }
  if (!review && !report["unmappedPrograms"].empty()) {
    err = "ASM uses unavailable target-bank programs " + report["unmappedPrograms"].dump() +
          "; supply programMap or review the import in Studio";
    return false;
  }
  for (const auto& [offset, target] : patches) {
    bytes[offset] = static_cast<uint8_t>(target);
  }
  return true;
}

// ---- MIDI (lossless round-trip) -------------------------------------------
//
// The exported SMF carries two things: (1) playable note/tempo/program tracks
// for use in any DAW, and (2) a sequencer-specific meta blob holding the exact
// serialized Capcom bytes. Import reconstructs from the blob (byte-identical);
// the note tracks are ignored on our own round-trip.

void midiVarLen(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t buf[4];
  int n = 0;
  buf[n++] = v & 0x7F;
  while ((v >>= 7)) buf[n++] = (v & 0x7F) | 0x80;
  for (int i = n - 1; i >= 0; --i) out.push_back(buf[i]);
}
void be16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(v >> 8); o.push_back(v & 0xFF); }
void be32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(v >> 24); o.push_back((v >> 16) & 0xFF); o.push_back((v >> 8) & 0xFF); o.push_back(v & 0xFF);
}
void midiChunk(std::vector<uint8_t>& out, const char* id, const std::vector<uint8_t>& body) {
  out.insert(out.end(), id, id + 4);
  be32(out, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

bool exportMidi(std::vector<uint8_t>& out, std::string& err) {
  std::vector<uint8_t> image;
  uint32_t base;
  bool prio;
  CapcomSeqIR ir;
  if (!currentSeqBytes(image, base, prio, ir, err)) return false;

  const uint16_t division = 48;  // PPQN
  double bpm = 120.0;
  auto* m = g_session->model.get();
  for (int t = 0; t < m->trackCount(); ++t) {
    const auto* d = m->trackData(t);
    if (!d) continue;
    bool done = false;
    for (const auto& s : d->settings) {
      if (s.type == CapcomSettingType::Tempo) {
        const uint16_t tw = static_cast<uint16_t>((s.value1 << 8) | s.value2);
        if (tw) bpm = 60000000.0 / (48.0 * 125.0 * 64.0 * 2.0) * (tw / 256.0);
        done = true;
        break;
      }
    }
    if (done) break;
  }

  // Track 0: tempo + lossless blob.
  std::vector<uint8_t> t0;
  midiVarLen(t0, 0);
  const uint32_t usPerQuarter = static_cast<uint32_t>(60000000.0 / (bpm > 0 ? bpm : 120.0));
  t0.push_back(0xFF); t0.push_back(0x51); t0.push_back(0x03);
  t0.push_back((usPerQuarter >> 16) & 0xFF); t0.push_back((usPerQuarter >> 8) & 0xFF); t0.push_back(usPerQuarter & 0xFF);
  // sequencer-specific meta: "GTB1" + base(2 BE) + priority(1) + len(2 BE) + sequence bytes
  const uint16_t seqLen = static_cast<uint16_t>(image.size());
  std::vector<uint8_t> blob = {'G', 'T', 'B', '1', static_cast<uint8_t>((base >> 8) & 0xFF),
                               static_cast<uint8_t>(base & 0xFF), static_cast<uint8_t>(prio ? 1 : 0),
                               static_cast<uint8_t>((seqLen >> 8) & 0xFF), static_cast<uint8_t>(seqLen & 0xFF)};
  blob.insert(blob.end(), image.begin(), image.end());
  midiVarLen(t0, 0);
  t0.push_back(0xFF); t0.push_back(0x7F);
  midiVarLen(t0, static_cast<uint32_t>(blob.size()));
  t0.insert(t0.end(), blob.begin(), blob.end());
  midiVarLen(t0, 0); t0.push_back(0xFF); t0.push_back(0x2F); t0.push_back(0x00);

  std::vector<uint8_t> midi;
  std::vector<uint8_t> head;
  be16(head, 1); be16(head, static_cast<uint16_t>(1 + m->trackCount())); be16(head, division);
  midiChunk(midi, "MThd", head);
  midiChunk(midi, "MTrk", t0);

  // Note tracks (playable).
  for (int t = 0; t < m->trackCount(); ++t) {
    std::vector<uint8_t> trk;
    const auto* d = m->trackData(t);
    const uint8_t chan = static_cast<uint8_t>(t & 0x0F);
    struct Ev { uint32_t tick; uint8_t st, d1, d2; };
    std::vector<Ev> evs;
    int lastProg = -1;
    if (d) {
      for (const auto& n : d->notes) {
        if (n.isRest || n.midiKey < 0) continue;
        if (n.program != lastProg) {
          evs.push_back({n.startTick, static_cast<uint8_t>(0xC0 | chan), n.program, 0});
          lastProg = n.program;
        }
        const uint8_t key = static_cast<uint8_t>(std::clamp(n.midiKey, 0, 127));
        const uint32_t dur = n.durationTicks ? n.durationTicks : n.deltaTicks;
        evs.push_back({n.startTick, static_cast<uint8_t>(0x90 | chan), key, 100});
        evs.push_back({n.startTick + dur, static_cast<uint8_t>(0x80 | chan), key, 0});
      }
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.tick < b.tick; });
    uint32_t prev = 0;
    for (const auto& e : evs) {
      midiVarLen(trk, e.tick - prev);
      prev = e.tick;
      trk.push_back(e.st); trk.push_back(e.d1);
      if ((e.st & 0xF0) != 0xC0) trk.push_back(e.d2);
    }
    midiVarLen(trk, 0); trk.push_back(0xFF); trk.push_back(0x2F); trk.push_back(0x00);
    midiChunk(midi, "MTrk", trk);
  }
  out = std::move(midi);
  return true;
}

// Pull the lossless GTB blob out of a MIDI file.
bool importMidi(const std::vector<uint8_t>& midi, std::string& err) {
  // Scan for the "GTB1" signature inside a sequencer-specific meta event.
  for (size_t i = 0; i + 9 <= midi.size(); ++i) {
    if (midi[i] == 'G' && midi[i + 1] == 'T' && midi[i + 2] == 'B' && midi[i + 3] == '1') {
      const uint32_t base = static_cast<uint32_t>((midi[i + 4] << 8) | midi[i + 5]);
      const uint32_t len = static_cast<uint32_t>((midi[i + 7] << 8) | midi[i + 8]);
      if (i + 9 + len > midi.size()) { err = "truncated GTB blob"; return false; }
      const std::vector<uint8_t> seq(midi.begin() + i + 9, midi.begin() + i + 9 + len);
      return loadSequenceImage(seq, base, "imported.mid", err);
    }
  }
  err.clear();
  return false;  // no blob — caller falls back to the generic converter
}

// Generic (foreign) MIDI import: convert via GTBoop-cli midi2spc, which owns
// the full quantizer/program-map/pitch-comp pipeline, then open the result.
// `opts` (all optional): defaultProgram int, duration int, pitchComp bool,
// programMap [{from,to}], noDefaultMap bool.
bool importMidiGeneric(const std::string& midiPath, const json& opts, std::string& err) {
  std::string cli;
  if (const char* e = std::getenv("GTB_CLI"); e && *e) {
    cli = e;
  } else {
    cli = (exeDir() / "GTBoop-cli.exe").string();
  }
  std::error_code ec;
  if (!std::filesystem::exists(cli, ec)) {
    err = "GTBoop-cli not found (set GTB_CLI); cannot convert generic MIDI";
    return false;
  }
  const auto outSpc = operationTempPath("midi", ".spc");
  std::string args;
  if (opts.contains("defaultProgram")) {
    args += " --default-program " + std::to_string(opts["defaultProgram"].get<int>());
  }
  if (opts.contains("duration")) {
    args += " --duration " + std::to_string(opts["duration"].get<int>());
  }
  if (opts.value("noDefaultMap", false)) args += " --no-default-map";
  if (opts.contains("programMap")) {
    for (const auto& m : opts["programMap"]) {
      args += " --map-program " + std::to_string(m["from"].get<int>()) + ":" +
              std::to_string(m["to"].get<int>());
    }
  }
  // Redirect the CLI's console chatter away from our stdout (JSON protocol).
  std::string cmd = "\"\"" + cli + "\" midi2spc \"" + midiPath + "\" \"" + outSpc.string() +
                    "\" --template \"" + soundBankPath() + "\"" + args + " >nul 2>&1\"";
  if (runHidden(cmd) != 0) {
    std::filesystem::remove(outSpc, ec);
    err = "GTBoop-cli midi2spc failed (check the MIDI file)";
    return false;
  }
  const bool loaded = loadSong(outSpc.string(), 0x0D20, err);
  std::filesystem::remove(outSpc, ec);
  return loaded;
}

// ---- ROM (Goof Troop .smc/.sfc) ---------------------------------------------
//
// Song table: 3-byte entries at PC 0x20000 (SNES $84:8000, headerless). The
// game's loader (CODE_8098DC in the disassembly) decodes an entry as
//   bank   = $84 OR byte2            (an OR, not an add - see reachability)
//   offset = word AND $7FFF          (index from bank:$8000)
// so PC = (($84 OR byte2) AND $7F)*$8000 + (word AND $7FFF).
// CODE_80995C reads another block header after each upload: a song is
// [size u16LE][ARAM u16LE][payload][0 u16LE][0 u16LE], NOT just its payload.
// CODE_809928/809937 wrap Y at $8000 and increment the bank, including across
// banks which cannot themselves be encoded as a table entry's starting bank.

constexpr uint32_t kRomSongTablePc = 0x20000;
constexpr int kRomSongSlots = 0x30;
constexpr uint32_t kRomStockSize = 0x80000;     // 512 KB, no free space inside
constexpr uint32_t kRomMaximumSize = 0x400000; // standard LoROM, no ExLoROM guessing
constexpr uint8_t kRomBankBase = 0x84;

uint16_t rd16le(const std::vector<uint8_t>& d, uint32_t o) {
  return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}
void wr16le(std::vector<uint8_t>& d, uint32_t o, uint16_t v) {
  d[o] = v & 0xFF;
  d[o + 1] = (v >> 8) & 0xFF;
}

struct RomSlotInfo {
  uint32_t blobPc = 0;  // headerless PC of [size, loadPtr, seq]
  uint16_t seqSize = 0;
  uint16_t loadPtr = 0;
  bool valid = false;
};

// A start bank must contain all the bits forced by ORA #$84.
bool romPcReachable(uint32_t pc) {
  return pc < kRomMaximumSize && ((pc / 0x8000) & 0x04) != 0;
}

RomSlotInfo resolveRomSlot(const std::vector<uint8_t>& rom, uint32_t hdr, int slot) {
  RomSlotInfo out;
  if (slot < 0 || slot >= kRomSongSlots || hdr > rom.size()) {
    return out;
  }
  const uint32_t entry = hdr + kRomSongTablePc + static_cast<uint32_t>(slot) * 3;
  if (entry + 3 > rom.size()) return out;
  const uint16_t word = rd16le(rom, entry);
  const uint8_t byte2 = rom[entry + 2];
  const uint64_t pc = static_cast<uint64_t>((byte2 | kRomBankBase) & 0x7F) * 0x8000 + (word & 0x7FFF);
  const uint64_t filePos = hdr + pc;
  if (filePos + 4 >= rom.size()) return out;
  const uint16_t size = rd16le(rom, static_cast<uint32_t>(filePos));
  const uint16_t load = rd16le(rom, static_cast<uint32_t>(filePos) + 2);
  if (size == 0 || size > kSongAramCapacity || load != kSongAramBase || filePos + 8 + size > rom.size()) {
    return out;
  }
  // Support only the single-block song format, not driver/SFX/multi-block data.
  if (!std::all_of(rom.begin() + filePos + 4 + size, rom.begin() + filePos + 8 + size,
                   [](uint8_t v) { return v == 0; })) {
    return out;
  }
  if (pc < kRomSongTablePc + kRomSongSlots * 3 && kRomSongTablePc < pc + size + 8) {
    return out;
  }
  out.blobPc = static_cast<uint32_t>(pc);
  out.seqSize = size;
  out.loadPtr = load;
  out.valid = true;
  return out;
}

void writeRomSlotEntry(std::vector<uint8_t>& rom, uint32_t hdr, int slot, uint32_t blobPc) {
  const uint32_t entry = hdr + kRomSongTablePc + static_cast<uint32_t>(slot) * 3;
  wr16le(rom, entry, static_cast<uint16_t>(blobPc & 0x7FFF));
  rom[entry + 2] = static_cast<uint8_t>((blobPc / 0x8000) & ~kRomBankBase);
}

// A block this editor relocated a song into: sized for the largest song the
// ARAM window can hold and tagged at its end, so later exports of that slot can
// rewrite it in place instead of expanding the ROM again every time. Untagged
// space is never reclaimed (see allocateRomSpace).
constexpr char kOwnedBlockTag[8] = {'G', 'T', 'B', 'O', 'O', 'P', 'R', '1'};
constexpr uint32_t kOwnedBlockSpan = 8 + kSongAramCapacity;  // [size][load][seq][4-byte terminator]
constexpr uint32_t kOwnedBlockBytes = kOwnedBlockSpan + sizeof(kOwnedBlockTag);

bool isOwnedBlock(const std::vector<uint8_t>& rom, uint32_t hdr, uint32_t blobPc) {
  const uint64_t tag = static_cast<uint64_t>(hdr) + blobPc + kOwnedBlockSpan;
  return tag + sizeof(kOwnedBlockTag) <= rom.size() &&
         std::memcmp(rom.data() + tag, kOwnedBlockTag, sizeof(kOwnedBlockTag)) == 0;
}

// Existing expansion is opaque, even FF fill: GoofED reserves $94/$95/$96
// for tile16/tilemaps/graphics (GoofED/ASM/Required/EditorROMMap.txt).
// Only bytes appended by THIS export (or a block tagged by an earlier one) are
// owned. Never reclaim anything else.
uint32_t allocateRomSpace(std::vector<uint8_t>& rom, uint32_t hdr, uint32_t need) {
  const auto oldSize = static_cast<uint32_t>(rom.size() - hdr);
  if (oldSize >= kRomMaximumSize) {
    return 0;
  }
  const uint32_t newSize = oldSize * 2;
  uint32_t pc = oldSize;
  while (pc < newSize && !romPcReachable(pc)) {
    pc += 0x8000;
  }
  if (pc >= newSize || need > newSize - pc) {
    return 0;
  }
  rom.resize(hdr + newSize, 0xFF);
  return pc;
}

bool validateRomLayout(const std::vector<uint8_t>& rom, uint32_t hdr, std::string& err) {
  const size_t size = rom.size() - hdr;
  if (size < kRomStockSize || size > kRomMaximumSize || (size & (size - 1)) != 0) {
    err = "unsupported ROM size: require power-of-two 512 KB to 4 MB LoROM";
    return false;
  }
  if (rom[hdr + 0x7FD5] != 0x20 && rom[hdr + 0x7FD5] != 0x30) {
    err = "unsupported ROM mapping: require LoROM";
    return false;
  }
  // Entire U loader, PC $18DC-$19B1, matched to local ROM/disassembly.
  // Other loader revisions require qualification; do not silently guess.
  const unsigned char loader[] =
      "\xA6\x9A\xDA\xA2\xFF\x86\x9A\x84\x02\xC2\x30\x29\xFF\x00\x85\x00"
      "\x0A\x65\x00\xAA\xBF\x00\x80\x84\x29\xFF\x7F\xA8\xA9\x00\x80\x85"
      "\x10\xE2\x20\xBF\x02\x80\x84\x09\x84\x85\x12\x8A\xF0\x0E\xAF\x09"
      "\xFF\x7F\xCD\x42\x21\xD0\xFB\xA5\x02\x8D\x40\x21\xC2\x20\xA9\xAA"
      "\xBB\xCD\x40\x21\xE2\x20\xD0\xEF\xA9\xCC\x80\x34\xB7\x10\xC8\x10"
      "\x05\xA0\x00\x00\xE6\x12\xEB\xA9\x00\x80\x12\xEB\xB7\x10\xC8\x10"
      "\x05\xA0\x00\x00\xE6\x12\xEB\xCD\x40\x21\xD0\xFB\x1A\xC2\x20\x8D"
      "\x40\x21\xE2\x20\xCA\xD0\xE4\xCD\x40\x21\xD0\xFB\x69\x03\xF0\xFC"
      "\x48\xB7\x10\xEB\xC8\x10\x05\xA0\x00\x00\xE6\x12\xB7\x10\xEB\xAA"
      "\xC8\x10\x05\xA0\x00\x00\xE6\x12\xB7\x10\xEB\xC8\x10\x05\xA0\x00"
      "\x00\xE6\x12\xB7\x10\x8D\x43\x21\xC8\x10\x05\xA0\x00\x00\xE6\x12"
      "\xEB\x8D\x42\x21\xE0\x01\x00\xA9\x00\x2A\x8D\x41\x21\x69\x7F\x68"
      "\x8D\x40\x21\xCD\x40\x21\xD0\xFB\x70\x82\xE2\x30\xA9\x01\x8F\x09"
      "\xFF\x7F\x68\x85\x9A\x60";
  if (std::memcmp(loader, rom.data() + hdr + 0x18DC, sizeof(loader) - 1) != 0) {
    err = "unsupported ROM song loader: expected qualified Goof Troop U upload routine";
    return false;
  }
  return true;
}

bool writeFileAtomically(const std::string& path, const std::vector<uint8_t>& rom, std::string& err) {
  const std::filesystem::path target(path);
  std::filesystem::path staging;
  std::error_code ec;
  for (unsigned i = 0; i < 64; ++i) {
    staging = target;
    staging += ".gtb-write-" + std::to_string(_getpid()) + "-" + std::to_string(i);
    if (std::filesystem::create_directory(staging, ec)) {
      break;
    }
    staging.clear();
    if (ec && ec != std::errc::file_exists) {
      break;
    }
  }
  if (staging.empty()) {
    err = "cannot stage the file beside its destination";
    return false;
  }
  const auto temporary = staging / "image.tmp";
  bool written = false;
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    out.flush();
    written = out.good();
    out.close();
    written = written && !out.fail();
  }
  if (written) {
#ifdef _WIN32
    written = MoveFileExW(temporary.c_str(), target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, target, ec);
    written = !ec;
#endif
  }
  std::filesystem::remove(temporary, ec);
  std::filesystem::remove(staging, ec);
  if (!written) {
    err = "cannot atomically replace the file; destination was not changed";
  }
  return written;
}

void updateSnesChecksum(std::vector<uint8_t>& rom, uint32_t hdr) {
  const uint32_t comp = hdr + 0x7FDC, sum = hdr + 0x7FDE;
  if (sum + 1 >= rom.size()) return;
  // Standard convention: the four checksum bytes count as FF FF 00 00.
  // (Verified: reproduces the stock Goof Troop (U) checksum exactly.)
  rom[comp] = rom[comp + 1] = 0xFF;
  rom[sum] = rom[sum + 1] = 0x00;
  uint32_t s = 0;
  for (size_t i = hdr; i < rom.size(); ++i) s += rom[i];
  wr16le(rom, comp, static_cast<uint16_t>(~s & 0xFFFF));
  wr16le(rom, sum, static_cast<uint16_t>(s & 0xFFFF));
}

bool openRomSong(const std::string& path, int slot, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { err = "cannot open ROM"; return false; }
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const uint32_t hdr = (rom.size() % 0x8000) == 512 ? 512 : 0;
  if (!validateRomLayout(rom, hdr, err)) {
    return false;
  }
  auto info = resolveRomSlot(rom, hdr, slot);
  if (!info.valid) { err = "cannot resolve song slot " + std::to_string(slot); return false; }
  std::vector<uint8_t> seq(rom.begin() + hdr + info.blobPc + 4,
                           rom.begin() + hdr + info.blobPc + 4 + info.seqSize);
  if (!loadSequenceImage(seq, info.loadPtr, std::filesystem::path(path).filename().string() +
                                                " [slot " + std::to_string(slot) + "]", err)) {
    return false;
  }
  auto rc = std::make_unique<RomContext>();
  rc->rom = std::move(rom);
  rc->headerOffset = hdr;
  rc->slot = slot;
  rc->blobPc = info.blobPc;
  rc->capacity = info.seqSize;  // in-place rewrite limit; beyond it export relocates
  rc->origSeq = seq;
  rc->path = path;
  g_session->rom = std::move(rc);
  return true;
}

struct RomExportResult {
  bool relocated = false;
  uint32_t blobPc = 0;
  size_t romSize = 0;
};

// Write the current sequence into a ROM slot and save to `outPath`. Uses the
// ROM the song came from unless `romPath` overrides it.
bool exportRomSong(const std::string& romPath, int slot, const std::string& outPath, std::string& err,
                   RomExportResult* result = nullptr) {
  std::vector<uint8_t> image;
  uint32_t base;
  bool prio;
  CapcomSeqIR ir;
  if (!currentSeqBytes(image, base, prio, ir, err)) return false;

  std::vector<uint8_t> rom;
  uint32_t hdr = 0;
  if (!romPath.empty()) {
    std::ifstream in(romPath, std::ios::binary);
    if (!in) { err = "cannot open ROM"; return false; }
    rom.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    hdr = (rom.size() % 0x8000) == 512 ? 512 : 0;
  } else if (g_session->rom) {
    rom = g_session->rom->rom;
    hdr = g_session->rom->headerOffset;
    if (slot < 0) slot = g_session->rom->slot;
  } else {
    err = "no ROM: pass romPath or open the song from a ROM";
    return false;
  }
  if (slot < 0) { err = "slot required"; return false; }

  if (!validateRomLayout(rom, hdr, err)) {
    return false;
  }

  auto info = resolveRomSlot(rom, hdr, slot);
  if (!info.valid) { err = "cannot resolve song slot " + std::to_string(slot); return false; }
  if (base != kSongAramBase || prio || image.empty() || image.size() > kSongAramCapacity) {
    err = "sequence (" + std::to_string(image.size()) + " bytes) exceeds the ARAM song window (" +
          std::to_string(kSongAramCapacity) + " bytes)";
    return false;
  }
  // Copy on write for aliases/overlapping song blobs. A table entry does not
  // establish exclusive ownership of the bytes it references.
  bool shared = false;
  for (int s = 0; s < kRomSongSlots; ++s) {
    if (s == slot) {
      continue;
    }
    const auto other = resolveRomSlot(rom, hdr, s);
    if (other.valid && info.blobPc < other.blobPc + other.seqSize + 8 &&
        other.blobPc < info.blobPc + info.seqSize + 8) {
      shared = true;
    }
  }
  const bool unchanged = image.size() <= info.seqSize && base == info.loadPtr &&
      std::equal(image.begin(), image.end(), rom.begin() + hdr + info.blobPc + 4);
  // A block we tagged on an earlier export can grow in place up to the window.
  const bool owned = !shared && isOwnedBlock(rom, hdr, info.blobPc);
  const size_t inPlaceCapacity = owned ? kSongAramCapacity : info.seqSize;
  bool relocated = false;
  if (image.size() > inPlaceCapacity || (!unchanged && shared)) {
    const uint32_t pc = allocateRomSpace(rom, hdr, kOwnedBlockBytes);
    if (pc == 0) {
      err = "no safely owned space: existing expansion is reserved and ROM cannot expand further";
      return false;
    }
    // Claim the whole block: zero it (the terminator and any later growth
    // must read as zeros) and tag it so the next export reuses it.
    std::fill(rom.begin() + hdr + pc, rom.begin() + hdr + pc + kOwnedBlockSpan, 0);
    std::memcpy(rom.data() + hdr + pc + kOwnedBlockSpan, kOwnedBlockTag, sizeof(kOwnedBlockTag));
    writeRomSlotEntry(rom, hdr, slot, pc);
    relocated = true;
    info.blobPc = pc;
    info.seqSize = static_cast<uint16_t>(image.size());
    info.loadPtr = static_cast<uint16_t>(base);
  }
  // If the song is unchanged from what the selected template slot already
  // stores (possibly plus tail padding counted in the size field), leave the
  // slot byte-for-byte alone. This must also work when the UI passes the same
  // ROM explicitly instead of relying on the open-ROM session context.
  const auto slotBegin = rom.begin() + hdr + info.blobPc + 4;
  const bool unchangedPrefix = image.size() <= info.seqSize && base == info.loadPtr &&
                               std::equal(image.begin(), image.end(), slotBegin);
  if (!unchangedPrefix || relocated) {
    wr16le(rom, hdr + info.blobPc, static_cast<uint16_t>(image.size()));
    wr16le(rom, hdr + info.blobPc + 2, static_cast<uint16_t>(base));
    std::copy(image.begin(), image.end(), rom.begin() + hdr + info.blobPc + 4);
    // Include the zero-size/zero-entry terminating block, also when shrinking.
    // (When an owned block grew in place, the old size is the smaller one.)
    const size_t clearTo = 8 + std::max<size_t>(info.seqSize, image.size());
    std::fill(rom.begin() + hdr + info.blobPc + 4 + image.size(),
              rom.begin() + hdr + info.blobPc + clearTo, 0);
  }
  uint8_t sizeCode = 9;
  for (size_t size = kRomStockSize; size < rom.size() - hdr; size *= 2) {
    ++sizeCode;
  }
  rom[hdr + 0x7FD7] = sizeCode;
  updateSnesChecksum(rom, hdr);
  if (!writeFileAtomically(outPath, rom, err)) {
    return false;
  }
  if (result) {
    result->relocated = relocated;
    result->romSize = rom.size() - hdr;
    result->blobPc = info.blobPc;
  }
  return true;
}

// By value on purpose: const json::operator[] on a missing key is undefined
// behaviour (it can crash the engine and lose unsaved work). On a mutable copy
// a missing field reads as null, and converting null to a number or string
// throws json::type_error, which main() reports as a normal error response.
json handle(json req) {
  json resp;
  resp["id"] = req.value("id", 0);
  const std::string cmd = req.value("cmd", "");
  std::string err;

  auto need = [&](bool haveSong) -> bool {
    if (haveSong && (!g_session || !g_session->model)) {
      err = "no song open";
      return false;
    }
    return true;
  };
  auto model = [&]() -> CapcomPianoRollModel* { return g_session ? g_session->model.get() : nullptr; };

  // Compound note edits may erase, optimize, insert and restore a program.
  // Both standalone edits and nested group edits must preserve the whole
  // history on failure, and create only one undo entry on success.
  std::unique_ptr<CapcomPianoRollModel::EditTransaction> noteTransaction;
  if (model() && (cmd == "placeNote" || cmd == "resizeNote" || cmd == "moveNote" || cmd == "insertNote")) {
    noteTransaction = std::make_unique<CapcomPianoRollModel::EditTransaction>(*model());
  }

  bool ok = false;
  if (cmd == "open") {
    uint32_t base = 0x0D20;
    if (req.contains("base")) {
      base = static_cast<uint32_t>(std::strtoul(req["base"].get<std::string>().c_str(), nullptr, 16));
    }
    std::string path = req.value("path", "");
    if (path.empty()) path = soundBankPath();  // default song (bundled soundbank)
    ok = loadSong(path, base, err);
    if (ok && req.contains("path")) {
      g_session->userPath = path;
    }
  } else if (cmd == "state") {
    ok = need(true);
  } else if (cmd == "moveNotes" && need(true)) {
    CapcomPianoRollModel::EditTransaction transaction(*model());
    auto items = req.at("items");
    const int deltaTick = req.at("dTick");
    const int deltaPitch = req.at("dPitch");
    if (!items.is_array() || items.empty() || items.size() > 4096) {
      err = "moveNotes requires 1..4096 note references";
    } else {
      std::sort(items.begin(), items.end(), [deltaTick](const json& a, const json& b) {
        return deltaTick > 0 ? a.at("tick") > b.at("tick") : a.at("tick") < b.at("tick");
      });
      ok = true;
      for (const auto& item : items) {
        const int track = item.at("track");
        const int sourceTick = item.at("tick");
        const int sourcePitch = item.at("pitch");
        const int64_t targetTick = static_cast<int64_t>(sourceTick) + deltaTick;
        const int64_t targetPitch = static_cast<int64_t>(sourcePitch) + deltaPitch;
        const auto* data = model()->trackData(track);
        int note = -1;
        if (data) {
          for (size_t i = 0; i < data->notes.size(); ++i) {
            const auto& n = data->notes[i];
            if (!n.isRest && n.startTick == sourceTick && n.midiKey == sourcePitch) {
              note = static_cast<int>(i);
              break;
            }
          }
        }
        if (note < 0 || targetTick < 0 || targetTick > UINT32_MAX || targetPitch < 0 || targetPitch > 127) {
          ok = false;
          err = "Group move contains a missing note or out-of-range destination";
          break;
        }
        const auto moved = handle({{"cmd", "placeNote"}, {"track", track}, {"note", note},
                                   {"tick", targetTick}, {"pitch", targetPitch}});
        if (!moved.value("ok", false)) {
          ok = false;
          err = moved.value("error", "Group move failed");
          break;
        }
      }
      if (ok) ok = transaction.commit(&err);
    }
    if (!ok) {
      std::string rollbackError;
      if (!transaction.rollback(&rollbackError)) err += "; " + rollbackError;
    }
  } else if (cmd == "replaceLoop" && need(true)) {
    CapcomPianoRollModel::EditTransaction transaction(*model());
    ok = model()->removeLoop(req.at("track"), req.at("loop"), &err) &&
        model()->createLoop(req.at("track"), req.at("startTick"), req.at("endTick"),
                            req.value("slot", 0), req.value("count", 2), &err);
    if (ok) ok = transaction.commit(&err);
    if (!ok) {
      std::string rollbackError;
      if (!transaction.rollback(&rollbackError)) err += "; " + rollbackError;
    }
  } else if (cmd == "setNote" && need(true)) {
    ok = model()->applyEdit(req["track"], req["note"], req.value("pitch", 60), req.value("len", 12u),
                            req.value("rest", false), &err);
  } else if (cmd == "insertNote" && need(true)) {
    const int track = req.at("track").get<int>();
    const uint32_t tick = req.at("tick").get<uint32_t>();
    const int pitch = req.value("pitch", 60);
    const uint32_t len = req.value("len", 12u);
    // Past the end of the track - or past its loop point, where the parser's
    // replayed events no longer count as occupied - there is "no event"; append
    // there instead (for a looping track that extends the looped body).
    auto place = [&]() {
      err.clear();
      if (model()->InsertNoteAtTick(track, tick, pitch, len, nullptr, nullptr, &err)) {
        return true;
      }
      if (err.find("No event at this position") == std::string::npos) {
        return false;
      }
      err.clear();
      return model()->AppendNoteAtTick(track, tick, pitch, len, &err);
    };
    ok = place();
    if (!ok && err.find("bytes are allocated") != std::string::npos) {
      // over budget: reclaim bytes (merge rests, drop dead settings), retry once
      uint32_t b0 = 0, b1 = 0;
      std::string optErr;
      if (verifiedOptimize(&b0, &b1, &optErr) && b1 < b0) {
        ok = place();
      }
    }
    // "outside" places the note past the loop point AND ends the song's loop,
    // so the loop plays through once, then the appended note, then stops.
    if (ok && req.value("loopMode", std::string()) == "outside") {
      if (!model()->endSongLoops(&err)) ok = false;
    }
  } else if (cmd == "moveNote" && need(true)) {
    ok = model()->MoveNote(req["track"], req["note"], req["tick"], req["pitch"], nullptr, &err);
  } else if (cmd == "placeNote" && need(true)) {
    // Drop a note anywhere: try its own track first; if that overlaps another
    // note, relocate to any track with free space there, keeping the
    // instrument (a program change is written on the new track as needed).
    const int fromTrack = req["track"];
    const int noteIdx = req["note"];
    const uint32_t tick = req["tick"];
    const int pitch = req["pitch"];
    auto* m = model();
    const auto* src = m->trackData(fromTrack);
    if (!src || noteIdx < 0 || noteIdx >= static_cast<int>(src->notes.size())) {
      err = "invalid note";
    } else {
      const auto note = src->notes[static_cast<size_t>(noteIdx)];
      ok = m->MoveNote(fromTrack, noteIdx, tick, pitch, nullptr, &err);
      if (ok) {
        resp["movedTrack"] = fromTrack;
      } else {
        // Same-track move can fail several ways (overlap, a tied note still
        // sounding across the drop point, ...) — all mean "no room here", so
        // fall through to cross-track relocation on any failure.
        // find a track with room for [tick, tick+dur)
        const uint32_t dur = note.durationTicks ? note.durationTicks : note.deltaTicks;
        for (int t = 0; t < m->trackCount() && !ok; ++t) {
          if (t == fromTrack) continue;
          const auto* cand = m->trackData(t);
          if (!cand) continue;
          bool free_ = true;
          for (const auto& o : cand->notes) {
            if (o.isRest || o.midiKey < 0) continue;
            const uint32_t oEnd = o.startTick + (o.durationTicks ? o.durationTicks : o.deltaTicks);
            if (tick < oEnd && o.startTick < tick + dur) { free_ = false; break; }
          }
          if (!free_) continue;
          std::string e2;
          if (!m->eraseNotes({{fromTrack, noteIdx}}, &e2)) continue;
          uint32_t insertedTick = 0;
          bool placed = m->InsertNoteAtTick(t, tick, pitch, note.deltaTicks, &insertedTick, nullptr, &e2, &note);
          if (!placed && e2.find("No event at this position") != std::string::npos) {
            e2.clear();
            placed = m->AppendNoteAtTick(t, tick, pitch, note.deltaTicks, &e2, &note);
            insertedTick = tick;
          }
          if (placed) {
            // pin the instrument on the relocated note
            const auto* dst = m->trackData(t);
            if (dst) {
              for (size_t i = 0; i < dst->notes.size(); ++i) {
                if (!dst->notes[i].isRest && dst->notes[i].startTick == insertedTick) {
                  if (dst->notes[i].program != note.program) {
                    if (!m->setInstrument({{t, static_cast<int>(i)}}, note.program, &e2)) {
                      err = "Relocated note could not preserve its instrument: " + e2;
                      resp["ok"] = false;
                      resp["error"] = err;
                      return resp;  // transaction destructor restores the complete edit
                    }
                  }
                  resp["movedNote"] = i;
                  break;
                }
              }
            }
            resp["movedTrack"] = t;
            err.clear();
            ok = true;
          } else {
            std::string ue;
            m->undo(&ue);  // roll back the erase
          }
        }
        if (!ok) err = "no room on any track at that position (" + err + ")";
      }
    }
  } else if (cmd == "resizeNote" && need(true)) {
    // Non-ripple resize: erase + reinsert at the same tick with the new
    // length. InsertNoteAtTick pads with rests, so downstream notes keep
    // their start ticks; growing only works into rest space.
    const int track = req["track"];
    const int noteIdx = req["note"];
    const uint32_t newLen = req["len"];
    auto* m = model();
    const auto* data = m->trackData(track);
    if (!data || noteIdx < 0 || noteIdx >= static_cast<int>(data->notes.size())) {
      err = "invalid note";
    } else {
      const auto note = data->notes[static_cast<size_t>(noteIdx)];
      if (newLen == note.deltaTicks) {
        // Preserve the original encoding and articulation, including every
        // segment of a tie. A no-op must not consume the previous Undo.
        ok = true;
      } else if (m->eraseNotes({{track, noteIdx}}, &err)) {
        uint32_t insertedTick = 0;
        ok = m->InsertNoteAtTick(track, note.startTick, note.midiKey, newLen,
                                 &insertedTick, nullptr, &err);
        if (!ok && err.find("bytes are allocated") != std::string::npos) {
          uint32_t b0 = 0, b1 = 0;
          std::string optErr;
          if (verifiedOptimize(&b0, &b1, &optErr) && b1 < b0) {
            err.clear();
            ok = m->InsertNoteAtTick(track, note.startTick, note.midiKey, newLen,
                                     &insertedTick, nullptr, &err);
          }
        }
        if (!ok) {
          std::string ue;
          m->undo(&ue);
          err = "not enough room to grow the note (" + err + ")";
        }
      }
    }
  } else if (cmd == "setInstrument" && need(true)) {
    std::vector<std::pair<int, int>> refs;
    for (const auto& r : req.at("notes")) {  // a missing list is an error, not a silent no-op
      refs.emplace_back(r.at("track").get<int>(), r.at("note").get<int>());
    }
    ok = model()->setInstrument(refs, req.at("program").get<int>(), &err);
  } else if (cmd == "eraseNote" && need(true)) {
    std::vector<std::pair<int, int>> refs{{req["track"].get<int>(), req["note"].get<int>()}};
    ok = model()->eraseNotes(refs, &err);
  } else if (cmd == "addSetting" && need(true)) {
    ok = model()->addSetting(req["track"], req["tick"], settingTypeFromName(req.value("type", "volume")),
                             req.value("value", 0), req.value("value2", 0), &err);
  } else if (cmd == "debugRebuild" && need(true)) {
    CapcomSeqIR ir;
    if (!ir.parseFromSeq(g_session->seq.get(), g_session->raw)) {
      err = "parse failed";
    } else {
      std::vector<uint32_t> sizes;
      if (ir.debugRebuildSizes(g_session->raw, &sizes, &err)) {
        resp["totalWithHeader"] = sizes[0];
        json per = json::array();
        for (size_t i = 1; i < sizes.size(); ++i) per.push_back(sizes[i]);
        resp["perTrackCmdBytes"] = per;
        ok = true;
      }
    }
  } else if (cmd == "optimize" && need(true)) {
    CapcomPianoRollModel::EditTransaction transaction(*model());
    uint32_t beforeB = 0, afterB = 0;
    ok = verifiedOptimize(&beforeB, &afterB, &err);
    int mergedTracks = 0;
    if (ok && req.value("merge", false)) {
      // fold same-instrument tracks with non-overlapping notes together;
      // repeat until no candidate pair merges
      bool progress = true;
      while (progress && ok) {
        progress = false;
        auto* m = model();
        for (int src = m->trackCount() - 1; src > 0 && !progress && ok; --src) {
          const auto* sd = m->trackData(src);
          if (!sd || sd->notes.empty() || !sd->loops.empty()) continue;
          int prog = -1;
          bool oneProg = true, anyNote = false;
          for (const auto& n : sd->notes) {
            if (n.isRest || n.isLoopRepeat) continue;
            anyNote = true;
            if (prog < 0) prog = n.program;
            else if (prog != n.program) { oneProg = false; break; }
          }
          if (!anyNote || !oneProg) continue;
          for (int dst = 0; dst < src && !progress && ok; ++dst) {
            const auto* dd = m->trackData(dst);
            if (!dd || !dd->loops.empty()) continue;
            int dprog = -1;
            bool dOne = true, dAny = false, overlap = false;
            for (const auto& n : dd->notes) {
              if (n.isRest || n.isLoopRepeat) continue;
              dAny = true;
              if (dprog < 0) dprog = n.program;
              else if (dprog != n.program) { dOne = false; break; }
            }
            if (!dAny || !dOne || dprog != prog) continue;
            for (const auto& a : sd->notes) {
              if (a.isRest || a.isLoopRepeat) continue;
              for (const auto& b : dd->notes) {
                if (b.isRest || b.isLoopRepeat) continue;
                if (a.startTick < b.startTick + b.deltaTicks &&
                    b.startTick < a.startTick + a.deltaTicks) { overlap = true; break; }
              }
              if (overlap) break;
            }
            if (overlap) continue;
            std::string mergeErr;
            if (mergeTracksInto(src, dst, &mergeErr)) {
              ++mergedTracks;
              progress = true;
            } else {
              // A merge can fail after inserting several notes. Never keep
              // those partial edits or continue with invalidated track views.
              ok = false;
              err = mergeErr;
            }
          }
        }
      }
      if (ok && mergedTracks > 0) {
        ok = verifiedOptimize(nullptr, &afterB, &err);
      }
    }
    if (ok) {
      ok = transaction.commit(&err);
    }
    if (!ok) {
      std::string rollbackErr;
      if (!transaction.rollback(&rollbackErr)) {
        err += " (" + rollbackErr + ")";
      }
    }
    if (ok) {
      resp["bytesBefore"] = beforeB;
      resp["bytesAfter"] = afterB;
      resp["mergedTracks"] = mergedTracks;
    }
  } else if (cmd == "updateSetting" && need(true)) {
    ok = model()->updateSetting(req["track"], req["setting"],
                                settingTypeFromName(req.value("type", "volume")),
                                req.value("value", 0), req.value("value2", 0), &err);
  } else if (cmd == "removeSetting" && need(true)) {
    ok = model()->removeSetting(req["track"], req["setting"], &err);
  } else if (cmd == "createLoop" && need(true)) {
    ok = model()->createLoop(req["track"], req["startTick"], req["endTick"], req.value("slot", 0),
                             req.value("count", 2), &err);
  } else if (cmd == "updateLoopCount" && need(true)) {
    ok = model()->updateLoopRepeatCount(req["track"], req["loop"], req["count"], &err);
  } else if (cmd == "removeLoop" && need(true)) {
    ok = model()->removeLoop(req["track"], req["loop"], &err);
  } else if (cmd == "undo" && need(true)) {
    ok = model()->undo(&err);
  } else if (cmd == "redo" && need(true)) {
    ok = model()->redo(&err);
  } else if (cmd == "save" && need(true)) {
    ok = saveSong(req.value("path", g_session->userPath.string()), err);
  } else if (cmd == "saveSession" && need(true)) {
    // .gtb session: JSON wrapping the SPC bytes plus editor extras (ghost
    // notes etc.) that have no representation in the SPC itself
    std::vector<uint8_t> spc;
    if (currentSpc(spc, err)) {
      json doc;
      doc["format"] = "gtb-session";
      doc["version"] = 1;
      doc["spcBase64"] = base64Encode(spc);
      // carry the byte allocation: a reopened session must keep the room the
      // song had (the ARAM song window, not its current footprint)
      uint32_t used = 0, budget = 0;
      if (g_session->model->byteUsage(&used, &budget)) doc["allocation"] = budget;
      if (req.contains("extra")) doc["extra"] = req["extra"];
      // atomic: a failed write (disk full, locked file) must neither truncate
      // the existing session nor report success
      const std::string text = doc.dump();
      ok = writeFileAtomically(req.at("path").get<std::string>(),
                               std::vector<uint8_t>(text.begin(), text.end()), err);
    }
  } else if (cmd == "openSession") {
    std::ifstream in(std::filesystem::path(req["path"].get<std::string>()), std::ios::binary);
    if (!in) { err = "cannot read session file"; }
    else {
      json doc = json::parse(in, nullptr, false);
      if (doc.is_discarded() || doc.value("format", "") != "gtb-session") {
        err = "not a gtb session file";
      } else {
        std::vector<uint8_t> spc;
        if (!base64Decode(doc.value("spcBase64", ""), spc)) {
          err = "corrupt session payload";
        } else {
          auto tmp = operationTempPath("session", ".spc");
          std::ofstream out(tmp, std::ios::binary);
          out.write(reinterpret_cast<const char*>(spc.data()), static_cast<std::streamsize>(spc.size()));
          out.close();
          ok = loadSong(tmp.string(), 0x0D20, err);
          std::error_code cleanupError;
          std::filesystem::remove(tmp, cleanupError);
          if (ok && doc.contains("allocation") && g_session && g_session->model) {
            // never shrink below the ARAM capacity: older sessions carried the
            // 1388-byte new-song floor from before relocation existed
            g_session->model->setAllocationFloor(
                std::max(doc["allocation"].get<uint32_t>(), kSongAramCapacity));
          }
          if (ok && doc.contains("extra")) resp["extra"] = doc["extra"];
        }
      }
    }
  } else if (cmd == "render" && need(true)) {
    std::vector<uint8_t> spc;
    if (currentSpc(spc, err)) {
      int muteMask = 0;
      if (req.contains("mute")) {
        for (const auto& v : req["mute"]) muteMask |= (1 << v.get<int>());
      }
      std::vector<uint8_t> wav;
      if (renderSpcWav(spc, req.value("seconds", 30), muteMask, wav, err)) {
        // Optional explicit destination (WAV export); default is a temp file.
        const std::string dest = req.value("path", std::string());
        std::filesystem::path p;
        if (!dest.empty()) {
          p = dest;
          std::ofstream o(p, std::ios::binary | std::ios::trunc);
          if (!o) { err = "cannot write WAV file"; }
          else o.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        } else {
          p = renderToTemp(wav, "song");
        }
        if (err.empty()) {
          resp["wav"] = p.string();
          resp["durationMs"] = req.value("seconds", 30) * 1000;
          ok = true;
        }
      }
    }
  } else if (cmd == "setAllocationFloor" && need(true)) {
    // Diagnostic: pin the byte budget (0 = the song's own footprint). Lets the
    // test suite exercise over-budget rejection now that real songs get the
    // whole ARAM window.
    // (the IR only ever raises its allocation, so rebuild it from the raw)
    g_session->model->setAllocationFloor(req.value("bytes", 0u));
    ok = g_session->model->reload();
    if (!ok) err = "model reload failed";
  } else if (cmd == "openRom") {
    ok = openRomSong(req.value("path", std::string()), req.value("slot", 0x15), err);
  } else if (cmd == "exportRom" && need(true)) {
    const std::string outPath = req.value("path", std::string());
    if (outPath.empty()) { err = "path required"; }
    else {
      RomExportResult r;
      if (exportRomSong(req.value("rom", std::string()), req.value("slot", -1), outPath, err, &r)) {
        resp["path"] = outPath;
        resp["relocated"] = r.relocated;
        resp["blobPc"] = r.blobPc;
        resp["romSize"] = r.romSize;
        ok = true;
      }
    }
  } else if (cmd == "listRomSongs") {
    std::ifstream in(req.value("path", std::string()), std::ios::binary);
    if (!in) { err = "cannot open ROM"; }
    else {
      std::vector<uint8_t> rom;
      rom.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      const uint32_t hdr = (rom.size() % 0x8000) == 512 ? 512 : 0;
      json slotArr = json::array();
      const bool supported = validateRomLayout(rom, hdr, err);
      for (int s = 0; supported && s < kRomSongSlots; ++s) {
        auto info = resolveRomSlot(rom, hdr, s);
        if (info.valid && info.loadPtr == 0x0D20) {
          slotArr.push_back({{"slot", s}, {"size", info.seqSize}});
        }
      }
      resp["slots"] = slotArr;
      ok = supported;
    }
  } else if (cmd == "exportAsm" && need(true)) {
    std::string text;
    if (exportAsmText(text, err)) {
      const std::string path = req.value("path", std::string());
      if (!path.empty()) {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write ASM file"; }
        else { o << text; resp["path"] = path; ok = true; }
      } else {
        resp["asm"] = text;
        ok = true;
      }
    }
  } else if (cmd == "inspectAsmRepair") {
    std::string text = req.value("asm", std::string());
    if (text.empty()) {
      std::ifstream in(req.value("path", std::string()), std::ios::binary);
      if (!in) { err = "cannot read ASM file"; }
      else text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::ifstream source(req.value("sourceRom", std::string()), std::ios::binary | std::ios::ate);
    if (!source || source.tellg() <= 0 || source.tellg() > 8 * 1024 * 1024) {
      err = "Source ROM must be readable and at most 8 MiB";
    }
    if (err.empty()) {
      const auto size = static_cast<size_t>(source.tellg());
      std::vector<uint8_t> rom(size);
      source.seekg(0);
      source.read(reinterpret_cast<char*>(rom.data()), static_cast<std::streamsize>(size));
      uint32_t base = 0;
      std::vector<uint8_t> bytes, repaired;
      std::map<uint32_t, unsigned> lines;
      json repairReport, importReport;
      if (!source) {
        err = "Source ROM read failed";
      } else if (assembleAsm(text, base, bytes, err, &lines) &&
                 AsmSourceRepair::Propose(text, bytes, base, lines, rom, repaired, repairReport, err) &&
                 prepareAsmImport(repaired, base, json::array(), true, importReport, err)) {
        std::ostringstream proposed;
        proposed << ".base $0D20\n" << std::hex << std::uppercase << std::setfill('0');
        for (size_t offset = 0; offset < repaired.size(); ++offset) {
          proposed << (offset % 16 == 0 ? "db $" : ",$") << std::setw(2) << unsigned(repaired[offset]);
          if (offset % 16 == 15 || offset + 1 == repaired.size()) { proposed << '\n'; }
        }
        resp["asm"] = proposed.str();
        resp["asmReport"] = importReport;
        resp["repairReport"] = repairReport;
        ok = true; // A proposal only: the live song and undo stack are untouched.
      }
      if (!ok && !repairReport.is_null()) {
        repairReport["validated"] = false;
        resp["repairReport"] = repairReport;
      }
    }
  } else if (cmd == "importAsm" || cmd == "inspectAsm") {
    std::string text = req.value("asm", std::string());
    if (text.empty() && req.contains("path")) {
      std::ifstream in(req["path"].get<std::string>(), std::ios::binary);
      if (!in) { err = "cannot read ASM file"; }
      else text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (err.empty()) {
      uint32_t base;
      std::vector<uint8_t> bytes;
      json report;
      if (assembleAsm(text, base, bytes, err) &&
          prepareAsmImport(bytes, base, req.value("programMap", json::array()), cmd == "inspectAsm", report, err)) {
        resp["asmReport"] = report;
        if (cmd == "inspectAsm") {
          resp["asm"] = text;  // UI imports the reviewed text, not a potentially changed file.
          ok = true;
        } else {
          ok = loadSequenceImage(bytes, base, req.value("name", std::string("imported.asm")), err);
        }
      }
    }
  } else if (cmd == "exportMidi" && need(true)) {
    std::vector<uint8_t> midi;
    if (exportMidi(midi, err)) {
      const std::string path = req.value("path", std::string());
      if (path.empty()) { err = "path required"; }
      else {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write MIDI file"; }
        else {
          o.write(reinterpret_cast<const char*>(midi.data()), static_cast<std::streamsize>(midi.size()));
          resp["path"] = path;
          ok = true;
        }
      }
    }
  } else if (cmd == "importMidi") {
    const std::string mpath = req.value("path", std::string());
    std::ifstream in(mpath, std::ios::binary);
    if (!in) { err = "cannot read MIDI file"; }
    else {
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      ok = importMidi(bytes, err);
      if (!ok && err.empty()) {
        // No GTB1 blob: it's a foreign MIDI — run the full converter.
        ok = importMidiGeneric(mpath, req.value("options", json::object()), err);
      }
    }
  } else if (cmd == "renderMp3" && need(true)) {
    std::vector<uint8_t> spc;
    if (currentSpc(spc, err)) {
      int muteMask = 0;
      if (req.contains("mute")) {
        for (const auto& v : req["mute"]) muteMask |= (1 << v.get<int>());
      }
      std::vector<uint8_t> wav;
      if (renderSpcWav(spc, req.value("seconds", 30), muteMask, wav, err)) {
        const auto wp = renderToTemp(wav, "song");
        const std::string dest = req.value("path", std::string());
        const std::filesystem::path mp =
            dest.empty() ? operationTempPath("render", ".mp3")
                         : std::filesystem::path(dest);
        if (wavToMp3(wp, mp, req.value("bitrate", std::string("192k")), err)) {
          resp["mp3"] = mp.string();
          ok = true;
        }
      }
    }
  } else if (cmd == "new") {
    // Fresh 8-track template: track 1 gets tempo/volume/duration/program/
    // octave defaults, the rest are empty. ~30 bytes — fits any ROM slot.
    std::vector<uint8_t> seq;
    const uint32_t hdrLen = 16;
    std::vector<uint8_t> t0 = {0x05, 0x01, 0xC7,  // tempo
                               0x07, 0xC0,        // volume
                               0x06, 0xC0,        // duration rate
                               0x08, 0x01,        // program 1
                               0x09, 0x04,        // octave 4
                               0x17};             // end
    const uint32_t base = 0x0D20;
    const uint32_t t0Addr = base + hdrLen;
    // each empty track gets its OWN end byte so track offsets stay unique
    // (shared offsets make model->IR track mapping ambiguous)
    for (int i = 0; i < 8; ++i) {
      const uint16_t p = static_cast<uint16_t>(
          i == 0 ? t0Addr : t0Addr + t0.size() + (i - 1));
      seq.push_back((p >> 8) & 0xFF);
      seq.push_back(p & 0xFF);
    }
    seq.insert(seq.end(), t0.begin(), t0.end());
    for (int i = 0; i < 7; ++i) seq.push_back(0x17);
    ok = loadSequenceImage(seq, base, "untitled (new song)", err);
    // (loadSequenceImage applied the ARAM capacity floor, so a new song may
    // grow to the full $0D20..$4000 window rather than its tiny footprint.)
  } else if (cmd == "previewKey" && need(true)) {
    // Sound a key at a given MIDI pitch with a given instrument (piano keys).
    const int pitch = req.value("pitch", 60);
    const int program = req.value("program", 1);
    const uint8_t octave = static_cast<uint8_t>(std::clamp(pitch / 12, 0, 10));
    const uint8_t keyIndex = static_cast<uint8_t>(pitch % 12 + 1);
    std::vector<uint8_t> spc;
    if (previewNoteSpc(program, octave, false, 0xC0, keyIndex, spc, err)) {
      std::vector<uint8_t> wav;
      if (renderSpcWav(spc, 1, 0, wav, err)) {
        resp["wav"] = renderToTemp(wav, "note").string();
        ok = true;
      }
    }
  } else if (cmd == "renderNote" && need(true)) {
    // Preview a note from the current song by track/note index.
    const int tr = req["track"].get<int>();
    const int ni = req["note"].get<int>();
    const auto* data = model()->trackData(tr);
    if (!data || ni < 0 || ni >= static_cast<int>(data->notes.size())) {
      err = "invalid note";
    } else {
      const auto& n = data->notes[static_cast<size_t>(ni)];
      std::vector<uint8_t> spc;
      if (previewNoteSpc(n.program, n.octave, n.octaveUp, n.durationRate, n.statusByte, spc, err)) {
        std::vector<uint8_t> wav;
        if (renderSpcWav(spc, req.value("seconds", 1), 0, wav, err)) {
          const auto p = renderToTemp(wav, "note");
          resp["wav"] = p.string();
          ok = true;
        }
      }
    }
  } else if (cmd == "close") {
    g_session.reset();
    ok = true;
  } else {
    err = "unknown or invalid command: " + cmd;
  }

  if (noteTransaction) {
    if (ok) ok = noteTransaction->commit(&err);
    if (!ok) {
      std::string rollbackError;
      if (!noteTransaction->rollback(&rollbackError)) err += "; " + rollbackError;
    }
  }
  resp["ok"] = ok;
  if (!ok) {
    resp["error"] = err.empty() ? "failed" : err;
  } else if (cmd != "close" && cmd != "render" && cmd != "renderNote") {
    resp["state"] = stateJson();
  }
  return resp;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  static TestRoot testRoot;
  pRoot = &testRoot;

  std::ios::sync_with_stdio(true);
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    json resp;
    try {
      json req = json::parse(line);
      resp = handle(req);
    } catch (const std::exception& e) {
      resp = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
    }
    std::cout << resp.dump() << std::endl;
    std::cout.flush();
  }
  return 0;
}
