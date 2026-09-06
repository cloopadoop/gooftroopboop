// Headless model edit-test harness (plan M2.4 / M3 verification).
//
// Drives the *real* CapcomPianoRollModel edit operations (the same code the
// GUI calls: resize/repitch, insert, move, set-instrument, loops, settings,
// undo/redo) against a stock song loaded into an EditableRawFile, and after
// every edit re-parses the backing bytes to assert the sequence is never
// corrupted. Console only: no window, no Qt Widgets, no desktop interaction.
//
// Usage: gtb-model-test <file.spc|file.bin> [--ops N] [--seed S] [--base hex]

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "RawFile.h"
#include "Root.h"
#include "formats/CapcomSnes/CapcomSnesSeq.h"
#include "formats/CapcomSnes/CapcomTrackTraversal.h"
#include "workarea/CapcomPianoRollModel.h"

namespace {

// Minimal headless root so the model's logging (pRoot->log via LogManager) and
// any UI callbacks resolve to no-ops instead of dereferencing a null pRoot.
class TestRoot final : public GTBRoot {
 public:
  void UI_setRootPtr(GTBRoot** theRoot) override { *theRoot = this; }
  std::string UI_getSaveFilePath(const std::string&, const std::string&) override { return {}; }
  std::string UI_getSaveDirPath(const std::string&) override { return {}; }
};

constexpr uint32_t kAramSize = 0x10000;
constexpr uint32_t kSpcAramFileOffset = 0x100;
constexpr char kSpcSignature[] = "SNES-SPC700 Sound File Data";

// Load the 64KB ARAM image from an .spc snapshot or a raw .bin song image.
bool LoadAram(const std::filesystem::path& path, uint32_t base, std::vector<uint8_t>& aram,
              std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot open " + path.string();
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  aram.assign(kAramSize, 0);
  const bool isSpc = bytes.size() >= kSpcAramFileOffset + kAramSize &&
                     std::memcmp(bytes.data(), kSpcSignature, sizeof(kSpcSignature) - 1) == 0;
  if (isSpc) {
    std::copy_n(bytes.begin() + kSpcAramFileOffset, kAramSize, aram.begin());
    return true;
  }
  if (base + bytes.size() > kAramSize) {
    err = "image does not fit in ARAM";
    return false;
  }
  std::copy(bytes.begin(), bytes.end(), aram.begin() + base);
  return true;
}

// Re-parse the backing raw and assert every track traverses to a clean end.
bool VerifyRaw(RawFile* raw, uint32_t base, bool priority, std::string& reason) {
  const uint32_t headerOffset = base + (priority ? 1 : 0);
  for (int t = 0; t < 8; ++t) {
    const uint16_t ptr = raw->readShortBE(headerOffset + static_cast<uint32_t>(t) * 2);
    if (ptr == 0) {
      continue;
    }
    if (ptr < base || ptr >= kAramSize) {
      reason = "track " + std::to_string(t) + " pointer out of range";
      return false;
    }
    CapcomTrackTraversalResult tr;
    std::string terr;
    if (!CapcomTrackTraversal::Traverse(raw, ptr, &tr, &terr)) {
      reason = "track " + std::to_string(t) + " traversal failed: " + terr;
      return false;
    }
    if (tr.steps.empty() || tr.steps.size() >= 19999) {
      reason = "track " + std::to_string(t) + " degenerate/runaway";
      return false;
    }
  }
  return true;
}

struct Stats {
  int applied = 0, refused = 0;
  int resize = 0, insert = 0, move = 0, instr = 0, erase = 0, setting = 0, loop = 0, undo = 0;
};

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);

  static TestRoot testRoot;
  pRoot = &testRoot;

  std::filesystem::path inputPath;
  uint32_t base = 0x0D20;
  int ops = 300;
  unsigned seed = 1;
  bool undoCheck = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--ops" && i + 1 < argc) {
      ops = std::atoi(argv[++i]);
    } else if (a == "--seed" && i + 1 < argc) {
      seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if (a == "--base" && i + 1 < argc) {
      base = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
    } else if (a == "--undocheck") {
      undoCheck = true;
    } else if (!a.empty() && a[0] != '-') {
      inputPath = a;
    }
  }
  if (inputPath.empty()) {
    std::fprintf(stderr, "Usage: gtb-model-test <file.spc|file.bin> [--ops N] [--seed S] [--base hex]\n");
    return 2;
  }

  std::vector<uint8_t> aram;
  std::string err;
  if (!LoadAram(inputPath, base, aram, err)) {
    std::fprintf(stderr, "FAIL %s: %s\n", inputPath.filename().string().c_str(), err.c_str());
    return 2;
  }

  // EditableRawFile loads from disk, so stage the ARAM to a temp file.
  const std::filesystem::path tmp =
      std::filesystem::temp_directory_path() /
      ("gtb-model-test-" + std::to_string(seed) + "-" + inputPath.stem().string() + ".aram");
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(reinterpret_cast<const char*>(aram.data()), static_cast<std::streamsize>(aram.size()));
  }

  auto* raw = new EditableRawFile(tmp.string());
  if (raw->size() != kAramSize) {
    std::fprintf(stderr, "FAIL: staged ARAM size %zu\n", raw->size());
    return 2;
  }

  // Stock GT songs have no leading type byte (track 0's pointer high byte is
  // 0x0D-0x11); a 0x00 there marks the custom priority-byte variant (I2I.bin).
  const bool priorityInHeader = aram[base] == 0x00;

  auto* seq = new CapcomSnesSeq(raw, CAPCOMSNES_V1_BGM_IN_LIST, base, priorityInHeader, "test");
  seq->parseTrackPointers();

  CapcomPianoRollModel model(seq);
  if (!model.reload()) {
    std::fprintf(stderr, "FAIL: model.reload()\n");
    return 1;
  }
  if (!model.canWrite()) {
    std::fprintf(stderr, "FAIL: model not writable\n");
    return 1;
  }

  std::mt19937 rng(seed);
  Stats st;
  const std::string name = inputPath.filename().string();

  auto snapshot = [&]() {
    std::vector<uint8_t> s(kAramSize);
    for (uint32_t i = 0; i < kAramSize; ++i) {
      s[i] = raw->readByte(i);
    }
    return s;
  };

  // Undo-correctness: every applied edit, when undone, must restore the exact
  // pre-edit bytes (plan M3.6). Exercises each edit type on real notes.
  if (undoCheck) {
    int checks = 0;
    for (int t = 0; t < model.trackCount(); ++t) {
      const auto* data = model.trackData(t);
      if (!data || data->notes.empty()) {
        continue;
      }
      for (int variant = 0; variant < 5; ++variant) {
        const int noteCount = static_cast<int>(model.trackData(t)->notes.size());
        if (noteCount == 0) {
          break;
        }
        const int ni = static_cast<int>(rng() % noteCount);
        const auto& note = model.trackData(t)->notes[ni];
        auto before = snapshot();
        std::string e;
        bool ok = false;
        switch (variant) {
          case 0: ok = model.applyEdit(t, ni, note.isRest ? 60 : note.midiKey, 24, false, &e); break;
          case 1: ok = model.InsertNoteAtTick(t, note.startTick + note.deltaTicks / 2, 62, 12,
                                              nullptr, nullptr, &e); break;
          case 2: {
            std::vector<std::pair<int,int>> r{{t, ni}};
            ok = model.setInstrument(r, static_cast<uint8_t>(3), &e);
            break;
          }
          case 3: {
            std::vector<std::pair<int,int>> r{{t, ni}};
            ok = model.eraseNotes(r, &e);
            break;
          }
          default: ok = model.addSetting(t, note.startTick, CapcomSettingType::Volume, 64, 0, &e); break;
        }
        if (!ok) {
          continue;  // refused edit changes nothing; nothing to undo
        }
        if (!model.undo(&e)) {
          std::printf("FAIL %s: undo returned false after variant %d (%s)\n", name.c_str(), variant,
                      e.c_str());
          return 1;
        }
        auto after = snapshot();
        if (after != before) {
          size_t firstDiff = 0;
          while (firstDiff < after.size() && after[firstDiff] == before[firstDiff]) {
            ++firstDiff;
          }
          std::printf("FAIL %s: undo did not restore bytes (variant %d, first diff at 0x%zx)\n",
                      name.c_str(), variant, firstDiff);
          return 1;
        }
        ++checks;
      }
    }
    std::printf("PASS %s: undo restored pre-edit bytes for %d edits\n", name.c_str(), checks);
    return 0;
  }

  auto verify = [&](const char* what, int step) -> bool {
    std::string reason;
    if (!VerifyRaw(raw, base, priorityInHeader, reason)) {
      std::printf("FAIL %s (seed %u, step %d, %s): %s\n", name.c_str(), seed, step, what, reason.c_str());
      return false;
    }
    return true;
  };

  for (int step = 0; step < ops; ++step) {
    const int tc = model.trackCount();
    if (tc == 0) {
      break;
    }
    const int t = static_cast<int>(rng() % tc);
    const auto* data = model.trackData(t);
    if (!data || data->notes.empty()) {
      continue;
    }
    const int noteCount = static_cast<int>(data->notes.size());
    const int ni = static_cast<int>(rng() % noteCount);
    const auto& note = data->notes[ni];
    const int op = rng() % 8;
    std::string e;
    bool ok = false;

    switch (op) {
      case 0: {  // resize / repitch (applyEdit)
        const int midi = note.isRest ? 60 : std::clamp(note.midiKey + (int(rng() % 5) - 2), 0, 127);
        const uint32_t len = 3u << (rng() % 6);  // 3..96 ticks (valid note lengths)
        ok = model.applyEdit(t, static_cast<size_t>(ni), midi, len, /*makeRest=*/false, &e);
        st.resize++;
        break;
      }
      case 1: {  // insert at a tick inside this note/rest
        const uint32_t tick = note.startTick + (note.deltaTicks / 2);
        const int midi = std::clamp(60 + (int(rng() % 13) - 6), 0, 127);
        ok = model.InsertNoteAtTick(t, tick, midi, 12, nullptr, nullptr, &e);
        st.insert++;
        break;
      }
      case 2: {  // move note (pitch/time)
        if (note.isRest) {
          continue;
        }
        const int midi = std::clamp(note.midiKey + (int(rng() % 7) - 3), 0, 127);
        const uint32_t tick = note.startTick;  // same tick, repitch via move
        ok = model.MoveNote(t, static_cast<size_t>(ni), tick, midi, nullptr, &e);
        st.move++;
        break;
      }
      case 3: {  // set instrument on a single note
        if (note.isRest) {
          continue;
        }
        const uint8_t prog = static_cast<uint8_t>(rng() % 22);
        std::vector<std::pair<int, int>> refs{{t, ni}};
        ok = model.setInstrument(refs, prog, &e);
        st.instr++;
        break;
      }
      case 4: {  // erase a note
        std::vector<std::pair<int, int>> refs{{t, ni}};
        ok = model.eraseNotes(refs, &e);
        st.erase++;
        break;
      }
      case 5: {  // add a setting (volume) at this note's tick
        const uint16_t val = static_cast<uint16_t>(rng() % 128);
        ok = model.addSetting(t, note.startTick, CapcomSettingType::Volume, val, 0, &e);
        st.setting++;
        break;
      }
      case 6: {  // loop: create / edit repeat count / remove
        if (!data->loops.empty()) {
          const size_t li = rng() % data->loops.size();
          if (rng() % 2 == 0) {
            ok = model.updateLoopRepeatCount(t, li, static_cast<uint8_t>(1 + rng() % 8), &e);
          } else {
            ok = model.removeLoop(t, li, &e);
          }
        } else {
          // Create a loop spanning a portion of the track.
          const uint32_t span = note.startTick + note.deltaTicks;
          const uint32_t startTick = note.startTick;
          const uint32_t endTick = startTick + std::max<uint32_t>(span, 24);
          const uint8_t slot = static_cast<uint8_t>(rng() % 4);
          ok = model.createLoop(t, startTick, endTick, slot, static_cast<uint8_t>(2 + rng() % 4), &e);
        }
        st.loop++;
        break;
      }
      default: {  // undo (if available), else redo
        if (model.canUndo()) {
          ok = model.undo(&e);
        } else if (model.canRedo()) {
          ok = model.redo(&e);
        } else {
          continue;
        }
        st.undo++;
        break;
      }
    }

    if (ok) {
      st.applied++;
    } else {
      st.refused++;  // a clean refusal (e.g. can't fit) is acceptable
    }

    if (!verify("edit", step)) {
      return 1;
    }
  }

  std::printf(
      "PASS %s (seed %u): %d applied / %d refused  [resize %d, insert %d, move %d, instr %d, "
      "erase %d, setting %d, loop %d, undo %d], no corruption\n",
      name.c_str(), seed, st.applied, st.refused, st.resize, st.insert, st.move, st.instr, st.erase,
      st.setting, st.loop, st.undo);

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  return 0;
}
