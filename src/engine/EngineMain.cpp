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
#include "Root.h"
#include "formats/CapcomSnes/CapcomSnesSeq.h"
#include "workarea/CapcomPianoRollModel.h"

#include "Snes_Spc.h"
#include "Spc_Filter.h"

using nlohmann::json;

namespace {

constexpr uint32_t kAramSize = 0x10000;
constexpr uint32_t kSpcAramFileOffset = 0x100;
constexpr char kSpcSignature[] = "SNES-SPC700 Sound File Data";

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

  s->tempAram = std::filesystem::temp_directory_path() /
                ("gtb-engine-" + std::filesystem::path(path).stem().string() + ".aram");
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

  json tracks = json::array();
  for (int t = 0; t < m.trackCount(); ++t) {
    const auto* data = m.trackData(t);
    json tj;
    tj["index"] = t;
    std::string instr;
    int noteCount = 0;
    json notes = json::array();
    if (data) {
      // Find the trailing Goto (song loop) FIRST: the traversal unrolls it
      // one pass, so every event repeats past the goto tick - clip those so
      // the UI shows a single rendition.
      uint32_t clipTick = 0xFFFFFFFF;
      {
        std::string irErr;
        const auto* trk = m.irTrack(t, &irErr);
        if (trk) {
          for (const auto& c : trk->cmds) {
            if (c.type == CapcomCmdType::Goto && c.destCmdIndex >= 0 &&
                c.destTrackIndex == trk->trackIndex &&
                c.destCmdIndex < static_cast<int>(trk->cmds.size())) {
              tj["songLoop"] = {{"tick", c.tick},
                                {"destTick", trk->cmds[static_cast<size_t>(c.destCmdIndex)].tick}};
              clipTick = c.tick;
              break;
            }
          }
        }
      }
      for (size_t ni = 0; ni < data->notes.size(); ++ni) {
        const auto& n = data->notes[ni];
        if (n.startTick >= clipTick) continue;
        notes.push_back({{"i", ni},
                         {"tick", n.startTick},
                         {"len", n.deltaTicks},
                         {"dur", n.durationTicks},
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
  return out;
}

// Save the edited ARAM back into the original container and write to `path`.
bool saveSong(const std::string& path, std::string& err) {
  if (!g_session || !g_session->raw) {
    err = "no song open";
    return false;
  }
  std::vector<uint8_t> outBytes = g_session->originalBytes;
  if (g_session->isSpc) {
    for (uint32_t i = 0; i < kAramSize; ++i) {
      outBytes[g_session->aramFileOffset + i] = g_session->raw->readByte(i);
    }
  } else {
    // .bin: write the song image region starting at base.
    for (size_t i = 0; i < outBytes.size(); ++i) {
      outBytes[i] = g_session->raw->readByte(g_session->base + i);
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = "cannot open output";
    return false;
  }
  out.write(reinterpret_cast<const char*>(outBytes.data()),
            static_cast<std::streamsize>(outBytes.size()));
  return true;
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
  const int kVerifySeconds = 15;
  std::vector<uint8_t> spcBefore;
  if (!currentSpc(spcBefore, *err)) return false;
  uint32_t before = 0, after = 0;
  if (!g_session->model->optimizeSequence(&before, &after, err)) return false;
  if (b0) *b0 = before;
  if (b1) *b1 = after;
  if (after >= before) return true;  // nothing changed
  std::vector<uint8_t> wavA, wavB, spcAfter;
  std::string rerr;
  if (!renderSpcWav(spcBefore, kVerifySeconds, 0, wavA, rerr) ||
      !currentSpc(spcAfter, rerr) ||
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
  struct SrcNote { uint32_t tick; int key; uint32_t len; uint8_t program; };
  std::vector<SrcNote> moves;
  for (const auto& n : sd->notes) {
    if (n.isRest || n.isLoopRepeat || n.midiKey < 0) continue;
    moves.push_back({n.startTick, n.midiKey, n.deltaTicks, n.program});
  }
  std::sort(moves.begin(), moves.end(),
            [](const SrcNote& a, const SrcNote& b) { return a.tick < b.tick; });
  for (const auto& mv : moves) {
    uint32_t insertedTick = 0;
    std::string e;
    bool placed = m->InsertNoteAtTick(dst, mv.tick, mv.key, mv.len, &insertedTick, nullptr, &e);
    if (!placed && e.find("No event at this position") != std::string::npos) {
      e.clear();
      placed = m->AppendNoteAtTick(dst, mv.tick, mv.key, mv.len, &e);
      insertedTick = mv.tick;
    }
    if (!placed && e.find("bytes are allocated") != std::string::npos) {
      uint32_t b0 = 0, b1 = 0;
      std::string oe;
      if (verifiedOptimize(&b0, &b1, &oe) && b1 < b0) {
        e.clear();
        placed = m->InsertNoteAtTick(dst, mv.tick, mv.key, mv.len, &insertedTick, nullptr, &e);
        if (!placed && e.find("No event at this position") != std::string::npos) {
          e.clear();
          placed = m->AppendNoteAtTick(dst, mv.tick, mv.key, mv.len, &e);
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
            m->setInstrument({{dst, static_cast<int>(i)}}, mv.program, &ie);
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
std::string findFfmpeg() {
  if (const char* e = std::getenv("GTB_FFMPEG")) {
    if (*e) return e;
  }
  std::error_code ec;
  auto exeDir = std::filesystem::current_path(ec);
  for (const char* name : {"ffmpeg.exe", "ffmpeg"}) {
    auto cand = exeDir / name;
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
  o << "; Reassemble with the same tool: Import > ASM.\n\n";
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

// Assemble the .db/.dw/label/.base dialect back to a raw sequence image.
// bytesOut[0] corresponds to ARAM address `baseOut`.
bool assembleAsm(const std::string& text, uint32_t& baseOut, std::vector<uint8_t>& bytesOut,
                 std::string& err) {
  struct Line { std::string op; std::vector<std::string> args; std::string label; };
  std::vector<Line> lines;
  uint32_t base = 0x0D20;
  bool baseSet = false;

  std::istringstream in(text);
  std::string raw;
  while (std::getline(in, raw)) {
    auto sc = raw.find(';');
    if (sc != std::string::npos) raw = raw.substr(0, sc);
    // trim
    auto b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) continue;
    auto e = raw.find_last_not_of(" \t\r\n");
    std::string line = raw.substr(b, e - b + 1);
    if (line.empty()) continue;

    if (!line.empty() && line.back() == ':') {
      lines.push_back({"label", {}, line.substr(0, line.size() - 1)});
      continue;
    }
    std::istringstream ls(line);
    std::string op;
    ls >> op;
    std::string lop = op;
    std::transform(lop.begin(), lop.end(), lop.begin(), ::tolower);
    std::string rest;
    std::getline(ls, rest);
    std::vector<std::string> args;
    std::string tok;
    std::istringstream as(rest);
    while (std::getline(as, tok, ',')) {
      auto tb = tok.find_first_not_of(" \t");
      if (tb == std::string::npos) continue;
      auto te = tok.find_last_not_of(" \t");
      args.push_back(tok.substr(tb, te - tb + 1));
    }
    if (lop == ".base") {
      base = parseNum(args.empty() ? "0" : args[0]);
      baseSet = true;
    } else if (lop == ".db" || lop == ".dw") {
      lines.push_back({lop, args, ""});
    }
  }
  (void)baseSet;

  // Pass 1: assign label addresses (bytes: .db = N, .dw = 2 each arg).
  std::map<std::string, uint32_t> labels;
  uint32_t off = 0;
  for (const auto& l : lines) {
    if (l.op == "label") { labels[l.label] = base + off; }
    else if (l.op == ".db") off += static_cast<uint32_t>(l.args.size());
    else if (l.op == ".dw") off += static_cast<uint32_t>(l.args.size()) * 2;
  }
  // Pass 2: emit bytes.
  std::vector<uint8_t> bytes;
  for (const auto& l : lines) {
    if (l.op == ".db") {
      for (const auto& a : l.args) bytes.push_back(static_cast<uint8_t>(parseNum(a) & 0xFF));
    } else if (l.op == ".dw") {
      for (const auto& a : l.args) {
        uint32_t v;
        if (!a.empty() && (a[0] == '$' || (a[0] >= '0' && a[0] <= '9'))) v = parseNum(a);
        else {
          auto it = labels.find(a);
          if (it == labels.end()) { err = "undefined label: " + a; return false; }
          v = it->second;
        }
        bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));  // big-endian
        bytes.push_back(static_cast<uint8_t>(v & 0xFF));
      }
    }
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
  s->tempAram = std::filesystem::temp_directory_path() / "gtb-engine-import.aram";
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
  g_session = std::move(s);
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
  const auto outSpc = std::filesystem::temp_directory_path() / "gtb-import-midi.spc";
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
    err = "GTBoop-cli midi2spc failed (check the MIDI file)";
    return false;
  }
  return loadSong(outSpc.string(), 0x0D20, err);
}

// ---- ROM (Goof Troop .smc/.sfc) ---------------------------------------------
//
// Song table: 3-byte LE pointer entries at PC 0x20000 (headerless). Blob at
// base+pointer (base is 0x18000 or 0x20000 depending on slot) laid out as
// [seqSize u16LE][aramLoadPtr u16LE][sequence bytes]. Mirrors
// Tools/Build-GoofTroop-TestRom.ps1.

constexpr uint32_t kRomSongTablePc = 0x20000;
constexpr int kRomSongSlots = 0x30;
constexpr std::array<uint32_t, 2> kRomPtrBases = {0x18000u, 0x20000u};

uint16_t rd16le(const std::vector<uint8_t>& d, uint32_t o) {
  return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}
void wr16le(std::vector<uint8_t>& d, uint32_t o, uint16_t v) {
  d[o] = v & 0xFF;
  d[o + 1] = (v >> 8) & 0xFF;
}

struct RomSlotInfo {
  uint32_t blobPc = 0;
  uint16_t seqSize = 0;
  uint16_t loadPtr = 0;
  bool valid = false;
};

// Resolve slot -> blob location using the same base-candidate heuristic as the
// PowerShell pipeline (prefer entries whose loadPtr is a sane ARAM address).
RomSlotInfo resolveRomSlot(const std::vector<uint8_t>& rom, uint32_t hdr, int slot) {
  RomSlotInfo out;
  const uint32_t entry = hdr + kRomSongTablePc + static_cast<uint32_t>(slot) * 3;
  if (entry + 3 > rom.size()) return out;
  const uint32_t ptr = rom[entry] | (rom[entry + 1] << 8) | (rom[entry + 2] << 16);
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t base : kRomPtrBases) {
      const uint64_t pc = static_cast<uint64_t>(hdr) + base + ptr;
      if (pc + 4 >= rom.size()) continue;
      const uint16_t size = rd16le(rom, static_cast<uint32_t>(pc));
      const uint16_t load = rd16le(rom, static_cast<uint32_t>(pc) + 2);
      const bool saneSize = size > 0 && size < 0x8000 && pc + 4 + size <= rom.size();
      const bool saneLoad = load == 0x0D20;
      if (saneSize && (pass == 1 || saneLoad)) {
        out.blobPc = static_cast<uint32_t>(pc);
        out.seqSize = size;
        out.loadPtr = load;
        out.valid = true;
        return out;
      }
    }
  }
  return out;
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
  auto info = resolveRomSlot(rom, hdr, slot);
  if (!info.valid) { err = "cannot resolve song slot " + std::to_string(slot); return false; }
  std::vector<uint8_t> seq(rom.begin() + info.blobPc + 4, rom.begin() + info.blobPc + 4 + info.seqSize);
  if (!loadSequenceImage(seq, info.loadPtr, std::filesystem::path(path).filename().string() +
                                                " [slot " + std::to_string(slot) + "]", err)) {
    return false;
  }
  auto rc = std::make_unique<RomContext>();
  rc->rom = std::move(rom);
  rc->headerOffset = hdr;
  rc->slot = slot;
  rc->blobPc = info.blobPc;
  rc->capacity = info.seqSize;  // in-place rewrite may not grow past the original blob
  rc->origSeq = seq;
  rc->path = path;
  g_session->rom = std::move(rc);
  return true;
}

// Write the current sequence into a ROM slot and save to `outPath`. Uses the
// ROM the song came from unless `romPath` overrides it.
bool exportRomSong(const std::string& romPath, int slot, const std::string& outPath, std::string& err) {
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

  auto info = resolveRomSlot(rom, hdr, slot);
  if (!info.valid) { err = "cannot resolve song slot " + std::to_string(slot); return false; }
  if (image.size() > info.seqSize) {
    err = "sequence (" + std::to_string(image.size()) + " bytes) exceeds slot capacity (" +
          std::to_string(info.seqSize) + "); choose a larger slot";
    return false;
  }
  // If the song is unchanged from what the selected template slot already
  // stores (possibly plus tail padding counted in the size field), leave the
  // slot byte-for-byte alone. This must also work when the UI passes the same
  // ROM explicitly instead of relying on the open-ROM session context.
  const auto slotBegin = rom.begin() + info.blobPc + 4;
  const bool unchangedPrefix = image.size() <= info.seqSize && base == info.loadPtr &&
                               std::equal(image.begin(), image.end(), slotBegin);
  if (!unchangedPrefix) {
    wr16le(rom, info.blobPc, static_cast<uint16_t>(image.size()));
    wr16le(rom, info.blobPc + 2, static_cast<uint16_t>(base));
    std::copy(image.begin(), image.end(), rom.begin() + info.blobPc + 4);
    // Zero the slot's tail so stale bytes from the previous song don't leak.
    std::fill(rom.begin() + info.blobPc + 4 + image.size(),
              rom.begin() + info.blobPc + 4 + info.seqSize, 0);
  }
  updateSnesChecksum(rom, hdr);

  std::ofstream o(outPath, std::ios::binary | std::ios::trunc);
  if (!o) { err = "cannot write ROM"; return false; }
  o.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
  return true;
}

json handle(const json& req) {
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

  bool ok = false;
  if (cmd == "open") {
    uint32_t base = 0x0D20;
    if (req.contains("base")) {
      base = static_cast<uint32_t>(std::strtoul(req["base"].get<std::string>().c_str(), nullptr, 16));
    }
    std::string path = req.value("path", "");
    if (path.empty()) path = soundBankPath();  // default song (bundled soundbank)
    ok = loadSong(path, base, err);
  } else if (cmd == "state") {
    ok = need(true);
  } else if (cmd == "setNote" && need(true)) {
    ok = model()->applyEdit(req["track"], req["note"], req.value("pitch", 60), req.value("len", 12u),
                            req.value("rest", false), &err);
  } else if (cmd == "insertNote" && need(true)) {
    ok = model()->InsertNoteAtTick(req["track"], req["tick"], req.value("pitch", 60),
                                   req.value("len", 12u), nullptr, nullptr, &err);
    if (!ok && err.find("No event at this position") != std::string::npos) {
      // past the track's end (or the track is empty): append instead
      err.clear();
      ok = model()->AppendNoteAtTick(req["track"], req["tick"], req.value("pitch", 60),
                                     req.value("len", 12u), &err);
    }
    if (!ok && err.find("bytes are allocated") != std::string::npos) {
      // over budget: reclaim bytes (merge rests, drop dead settings), retry once
      uint32_t b0 = 0, b1 = 0;
      std::string optErr;
      if (verifiedOptimize(&b0, &b1, &optErr) && b1 < b0) {
        err.clear();
        ok = model()->InsertNoteAtTick(req["track"], req["tick"], req.value("pitch", 60),
                                       req.value("len", 12u), nullptr, nullptr, &err);
        if (!ok && err.find("No event at this position") != std::string::npos) {
          err.clear();
          ok = model()->AppendNoteAtTick(req["track"], req["tick"], req.value("pitch", 60),
                                         req.value("len", 12u), &err);
        }
      }
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
          bool placed = m->InsertNoteAtTick(t, tick, pitch, note.deltaTicks, &insertedTick, nullptr, &e2);
          if (!placed && e2.find("No event at this position") != std::string::npos) {
            e2.clear();
            placed = m->AppendNoteAtTick(t, tick, pitch, note.deltaTicks, &e2);
            insertedTick = tick;
          }
          if (placed) {
            // pin the instrument on the relocated note
            const auto* dst = m->trackData(t);
            if (dst) {
              for (size_t i = 0; i < dst->notes.size(); ++i) {
                if (!dst->notes[i].isRest && dst->notes[i].startTick == insertedTick) {
                  if (dst->notes[i].program != note.program) {
                    m->setInstrument({{t, static_cast<int>(i)}}, note.program, &e2);
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
      if (m->eraseNotes({{track, noteIdx}}, &err)) {
        uint32_t insertedTick = 0;
        ok = m->InsertNoteAtTick(track, note.startTick, note.midiKey, newLen,
                                 &insertedTick, nullptr, &err);
        if (!ok && err.find("bytes are allocated") != std::string::npos) {
          uint32_t b0 = 0, b1 = 0;
          std::string optErr;
          if (m->optimizeSequence(&b0, &b1, &optErr) && b1 < b0) {
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
    for (const auto& r : req["notes"]) {
      refs.emplace_back(r["track"].get<int>(), r["note"].get<int>());
    }
    ok = model()->setInstrument(refs, req["program"].get<int>(), &err);
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
    uint32_t beforeB = 0, afterB = 0;
    ok = verifiedOptimize(&beforeB, &afterB, &err);
    int mergedTracks = 0;
    if (ok && req.value("merge", false)) {
      // fold same-instrument tracks with non-overlapping notes together;
      // repeat until no candidate pair merges
      bool progress = true;
      while (progress) {
        progress = false;
        auto* m = model();
        for (int src = m->trackCount() - 1; src > 0 && !progress; --src) {
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
          for (int dst = 0; dst < src && !progress; ++dst) {
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
            }
          }
        }
      }
      if (mergedTracks > 0) {
        verifiedOptimize(nullptr, &afterB, &err);
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
    ok = saveSong(req.value("path", g_session->sourcePath.string()), err);
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
      // song had (a new song's 1388-byte floor, not its current footprint)
      uint32_t used = 0, budget = 0;
      if (g_session->model->byteUsage(&used, &budget)) doc["allocation"] = budget;
      if (req.contains("extra")) doc["extra"] = req["extra"];
      std::ofstream out(std::filesystem::path(req["path"].get<std::string>()), std::ios::binary);
      if (out) { out << doc.dump(); ok = true; }
      else err = "cannot write session file";
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
          auto tmp = std::filesystem::temp_directory_path() / "gtb-session-open.spc";
          std::ofstream out(tmp, std::ios::binary);
          out.write(reinterpret_cast<const char*>(spc.data()), static_cast<std::streamsize>(spc.size()));
          out.close();
          ok = loadSong(tmp.string(), 0x0D20, err);
          if (ok && doc.contains("allocation") && g_session && g_session->model) {
            g_session->model->setAllocationFloor(doc["allocation"].get<uint32_t>());
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
  } else if (cmd == "openRom") {
    ok = openRomSong(req.value("path", std::string()), req.value("slot", 0x15), err);
  } else if (cmd == "exportRom" && need(true)) {
    const std::string outPath = req.value("path", std::string());
    if (outPath.empty()) { err = "path required"; }
    else if (exportRomSong(req.value("rom", std::string()), req.value("slot", -1), outPath, err)) {
      resp["path"] = outPath;
      ok = true;
    }
  } else if (cmd == "listRomSongs") {
    std::ifstream in(req.value("path", std::string()), std::ios::binary);
    if (!in) { err = "cannot open ROM"; }
    else {
      std::vector<uint8_t> rom;
      rom.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      const uint32_t hdr = (rom.size() % 0x8000) == 512 ? 512 : 0;
      json slotArr = json::array();
      for (int s = 0; s < kRomSongSlots; ++s) {
        auto info = resolveRomSlot(rom, hdr, s);
        if (info.valid && info.loadPtr == 0x0D20) {
          slotArr.push_back({{"slot", s}, {"size", info.seqSize}});
        }
      }
      resp["slots"] = slotArr;
      ok = true;
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
  } else if (cmd == "importAsm") {
    std::string text = req.value("asm", std::string());
    if (text.empty() && req.contains("path")) {
      std::ifstream in(req["path"].get<std::string>(), std::ios::binary);
      if (!in) { err = "cannot read ASM file"; }
      else text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (err.empty()) {
      uint32_t base;
      std::vector<uint8_t> bytes;
      if (assembleAsm(text, base, bytes, err) &&
          loadSequenceImage(bytes, base, req.value("name", std::string("imported.asm")), err)) {
        ok = true;
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
            dest.empty() ? std::filesystem::temp_directory_path() / "gtb-render-song.mp3"
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
    // A new song may grow into the space the game reserves for its largest
    // stock song (1388 bytes, ROM slot 20) instead of its own tiny footprint.
    if (ok && g_session && g_session->model) {
      g_session->model->setAllocationFloor(1388);
    }
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
