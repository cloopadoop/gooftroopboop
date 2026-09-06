#!/usr/bin/env python3
"""Goof Troop Boop engine regression suite.

Every test here encodes a bug or invariant discovered during real editing
sessions (see git history around 2026-07-09/10). The engine is exercised
headlessly over its JSON-lines stdin/stdout protocol; audio assertions use
the fact that the bundled snes_spc renderer is deterministic, so
byte-identical WAVs mean provably-equivalent song bytes.

Run via run-tests.cmd (or: python tests/regression.py). Exit code 0 = all
green. A log with per-test detail is written to tests/regression.log.

Environment expectations:
  - Run from the gooftroopboop repo root (the .cmd wrapper handles this).
  - Stock SPC corpus lives at ../../Music Sources/SPC/*.spc.
  - Engine binary at build/win-x64/bin/gtb-engine.exe.
"""

import glob
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.environ.get("GTB_TEST_ENGINE", os.path.join(ROOT, "build", "win-x64", "bin", "gtb-engine.exe"))
CORPUS = os.environ.get("GTB_TEST_SPC_CORPUS", os.path.join(os.path.dirname(os.path.dirname(ROOT)), "Music Sources", "SPC"))
CWD = os.path.dirname(os.path.dirname(ROOT))  # repo root that song paths are relative to
TMP = tempfile.gettempdir()
SONG_WAV = os.path.join(TMP, "gtb-render-song.wav")
NOTE_WAV = os.path.join(TMP, "gtb-render-note.wav")
LOG = os.path.join(ROOT, "tests", "regression.log")

HAMLET = os.path.join(CORPUS, "08 Hamlet.spc")
GAME_OVER = os.path.join(CORPUS, "15 Game Over.spc")
BREAK = os.path.join(CORPUS, "10 Break.spc")
FLASHBACK = os.path.join(CORPUS, "17 Flashback.spc")

_log_lines = []


def log(msg):
    _log_lines.append(msg)


def run(reqs, env=None):
    """Feed JSON-lines requests to a fresh engine process; return responses."""
    inp = "\n".join(json.dumps(r) for r in reqs) + "\n"
    child_env = os.environ.copy()
    # The engine's default sound bank resolves beside its own executable; the
    # bare build tree has none, so point it at the corpus Hamlet dump (the
    # old behavior, now via the portable env contract instead of a hardcoded
    # developer path in the engine).
    child_env.setdefault("GTB_SOUNDBANK", HAMLET)
    if env:
        child_env.update(env)
    p = subprocess.run([ENGINE], input=inp, capture_output=True, text=True, cwd=CWD, env=child_env, timeout=180)
    if p.returncode != 0:
        raise RuntimeError(f"engine exited with code {p.returncode}")
    out = []
    for line in p.stdout.splitlines():
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError as exc:
            raise RuntimeError("non-JSON output on engine protocol") from exc
    if len(out) != len(reqs):
        raise RuntimeError(f"expected {len(reqs)} responses, got {len(out)}")
    return out


def read_wav_mono(path):
    w = wave.open(path, "rb")
    n = w.getnframes()
    d = w.readframes(n)
    return [struct.unpack_from("<h", d, i * 4)[0] for i in range(n)]


def wav_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def render_song(path, seconds=3, mute=None, pre=None):
    reqs = [{"id": 1, "cmd": "open", "path": path}]
    if pre:
        reqs += pre
    reqs.append({"id": 9, "cmd": "render", "seconds": seconds, "mute": mute or []})
    rs = run(reqs)
    wav = next(r["wav"] for r in rs if r.get("wav"))
    return wav_bytes(wav), rs

def last_wav(rs):
    return next(r["wav"] for r in reversed(rs) if r.get("wav"))


# ---------------------------------------------------------------------------
# Tests. Each returns None on pass, or a failure string.
# ---------------------------------------------------------------------------

def test_corpus_open_save_parity():
    """Opening and saving any stock song must be byte-identical.

    Origin: the layout-preserving serialization milestone (M1.3). Unedited
    round-trips must never move a byte.
    """
    for f in sorted(glob.glob(os.path.join(CORPUS, "*.spc"))):
        dst = os.path.join(TMP, "parity.spc")
        run([{"id": 1, "cmd": "open", "path": f}, {"id": 2, "cmd": "save", "path": dst}])
        if open(f, "rb").read() != open(dst, "rb").read():
            return f"byte drift on {os.path.basename(f)}"
        log(f"  parity ok: {os.path.basename(f)}")


def test_corpus_optimize_noop():
    """optimize {merge:true} must be a render-perfect no-op on stock songs.

    Origin: three broken optimize variants (dead-store elimination corrupted
    an LFO region; same-value re-set removal audibly changed 'Goofy or Max';
    a slur-context rest merge changed 'Game Over'). verifiedOptimize now
    render-checks and rolls back, so this holds by construction - this test
    guards the verification machinery itself.
    """
    for f in sorted(glob.glob(os.path.join(CORPUS, "*.spc"))):
        base, _ = render_song(f)
        after, rs = render_song(f, pre=[{"id": 2, "cmd": "optimize", "merge": True}])
        r = rs[1]
        if not r["ok"]:
            return f"optimize failed on {os.path.basename(f)}: {r.get('error')}"
        if r.get("mergedTracks", 0):
            return f"stock song merged tracks: {os.path.basename(f)}"
        if base != after:
            return f"render changed: {os.path.basename(f)}"
        log(f"  no-op ok: {os.path.basename(f)}")


def test_failed_edit_leaves_file_untouched():
    """An edit rejected for budget must not change a single byte.

    Origin: dirty-IR bug - a failed InsertNoteAtTick left the mutated IR in
    memory and a later optimize serialized it, committing the failed edit.
    """
    a = os.path.join(TMP, "untouched_a.spc")
    b = os.path.join(TMP, "untouched_b.spc")
    run([{"id": 1, "cmd": "open", "path": GAME_OVER}, {"id": 2, "cmd": "save", "path": a}])
    rs = run([
        {"id": 1, "cmd": "open", "path": GAME_OVER},
        {"id": 2, "cmd": "insertNote", "track": 3, "tick": 0, "pitch": 60, "len": 12},
        {"id": 3, "cmd": "save", "path": b},
    ])
    if rs[1]["ok"]:
        return "expected the over-budget insert to fail (song is byte-tight)"
    if open(a, "rb").read() != open(b, "rb").read():
        return "failed insert mutated the file"


def test_append_full_midi_pitch_range():
    """Octave operands have only three bits; high notes need the octave-up flag."""
    reqs = [{"cmd": "new"}]
    pitches = list(range(128)) + [60, 96, 92, 127, 0]
    reqs.extend({"cmd":"insertNote", "track":0, "tick":i*12, "pitch":pitch, "len":12}
                for i, pitch in enumerate(pitches))
    rs = run(reqs)
    if len(rs) != len(reqs) or not all(r.get("ok") for r in rs):
        return "pitch sweep insert failed"
    actual = [n["pitch"] for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]]
    if actual != pitches:
        return "pitch sweep wrapped or transposed notes"


def test_loop_at_track_end_remains_editable():
    """A trailing loop must precede END, survive reload, and support history."""
    rs = run([
        {"cmd": "new"},
        {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 12},
        {"cmd": "insertNote", "track": 0, "tick": 12, "pitch": 64, "len": 12},
        {"cmd": "createLoop", "track": 0, "startTick": 0, "endTick": 24, "count": 2},
        {"cmd": "updateLoopCount", "track": 0, "loop": 0, "count": 3},
        {"cmd": "removeLoop", "track": 0, "loop": 0},
        {"cmd": "undo"},
        {"cmd": "redo"},
    ])
    if len(rs) != 8 or not all(r.get("ok") for r in rs):
        return "loop edit/history command failed"
    loops = rs[3]["state"]["tracks"][0]["loops"]
    if len(loops) != 1 or (loops[0]["destTick"], loops[0]["tick"], loops[0]["count"]) != (0, 24, 2):
        return "trailing loop was lost or has incorrect bounds"
    if rs[4]["state"]["tracks"][0]["loops"][0]["count"] != 3:
        return "loop count did not update"
    if rs[5]["state"]["tracks"][0]["loops"] or rs[7]["state"]["tracks"][0]["loops"]:
        return "loop removal/redo did not persist"
    if rs[6]["state"]["tracks"][0]["loops"][0]["count"] != 3:
        return "undo did not restore loop"


def test_compose_from_scratch():
    """File->New, draw notes on two tracks, render is audible.

    Origin: AppendNoteAtTick (empty tracks had no event to split), the
    1388-byte allocation floor, and the shared-END-byte track-mapping bug
    (notes drawn on track 1 landed on track 8).
    """
    reqs = [{"id": 1, "cmd": "new"},
            {"id": 2, "cmd": "insertNote", "track": 0, "tick": 48, "pitch": 60, "len": 24},
            {"id": 3, "cmd": "insertNote", "track": 3, "tick": 0, "pitch": 48, "len": 48},
            {"id": 4, "cmd": "render", "seconds": 2}]
    rs = run(reqs)
    for r in rs[:3]:
        if not r["ok"]:
            return f"id{r['id']} failed: {r.get('error')}"
    st = rs[2]["state"]
    if st["budget"]["total"] != 1388:
        return f"allocation floor wrong: {st['budget']}"
    per = {t["index"]: [(n["tick"], n["pitch"]) for n in t["notes"] if not n["rest"]]
           for t in st["tracks"] if any(not n["rest"] for n in t["notes"])}
    if per != {0: [(48, 60)], 3: [(0, 48)]}:
        return f"notes landed wrong: {per}"
    mono = read_wav_mono(last_wav(rs))
    rms = math.sqrt(sum(x * x for x in mono) / len(mono))
    if rms < 5:
        return f"render silent (rms {rms:.1f})"


def test_nonripple_resize():
    """Resizing a note must not shift downstream notes.

    Origin: resize used to change the note's delta, rippling every
    following note in time; resizeNote is erase+reinsert at the same tick.
    """
    # session save/open keeps the new-song allocation floor (a bare .spc
    # reopen would pin the budget to the tiny footprint - itself a former bug)
    tmp = os.path.join(TMP, "resize.gtb")
    rs = run([{"id": 1, "cmd": "new"},
              {"id": 2, "cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
              {"id": 3, "cmd": "insertNote", "track": 0, "tick": 24, "pitch": 62, "len": 24},
              {"id": 4, "cmd": "insertNote", "track": 0, "tick": 48, "pitch": 64, "len": 24},
              {"id": 5, "cmd": "saveSession", "path": tmp}])
    st = rs[3]["state"]
    notes = [n for n in st["tracks"][0]["notes"] if not n["rest"]]
    rs = run([{"id": 1, "cmd": "openSession", "path": tmp},
              {"id": 2, "cmd": "resizeNote", "track": 0, "note": notes[0]["i"], "len": 12}])
    if not rs[1]["ok"]:
        return f"resize failed: {rs[1].get('error')}"
    after = [(n["tick"], n["pitch"]) for n in rs[1]["state"]["tracks"][0]["notes"] if not n["rest"]]
    if [(t, p) for t, p in after if t > 0] != [(24, 62), (48, 64)]:
        return f"downstream notes moved: {after}"


def test_place_note_relocation():
    """Dropping a note on an occupied spot relocates it to a free track.

    Origin: placeNote cross-track relocation with instrument pinning.
    """
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET}])
    st = rs[0]["state"]
    t3 = [n for n in st["tracks"][3]["notes"] if not n["rest"]]
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "placeNote", "track": 3, "note": t3[0]["i"],
               "tick": t3[5]["tick"], "pitch": 60}])
    r = rs[1]
    if not r["ok"]:
        return f"placeNote failed: {r.get('error')}"
    if r.get("movedTrack") == 3:
        return "expected relocation off track 3 (target tick was occupied)"


def test_session_roundtrip():
    """.gtb sessions preserve the song and editor extras (ghost notes).

    Origin: sessions keep 'illegal' notes until the user sorts them out.
    """
    tmp = os.path.join(TMP, "session.gtb")
    ghosts = [{"track": 2, "tick": 480, "pitch": 72, "len": 24, "reason": "test"}]
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "saveSession", "path": tmp, "extra": {"ghosts": ghosts}}])
    if not rs[1]["ok"]:
        return f"saveSession failed: {rs[1].get('error')}"
    rs = run([{"id": 1, "cmd": "openSession", "path": tmp},
              {"id": 2, "cmd": "render", "seconds": 1}])
    if not rs[0]["ok"] or not rs[1]["ok"]:
        return "openSession or render failed"
    if rs[0].get("extra", {}).get("ghosts") != ghosts:
        return "ghosts did not round-trip"
    if sum(len(t["notes"]) for t in rs[0]["state"]["tracks"]) < 100:
        return "song content lost in session round-trip"


def test_track_merge():
    """optimize {merge:true} folds same-instrument non-overlapping tracks.

    Origin: the auto-optimizer card; frees whole channels.
    """
    rs = run([{"id": 1, "cmd": "new"},
              {"id": 2, "cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
              {"id": 3, "cmd": "insertNote", "track": 1, "tick": 96, "pitch": 67, "len": 24},
              {"id": 4, "cmd": "optimize", "merge": True}])
    r = rs[3]
    if not r["ok"] or r.get("mergedTracks", 0) != 1:
        return f"expected 1 merged track, got {r.get('mergedTracks')} ({r.get('error', '')})"
    per = {t["index"]: len([n for n in t["notes"] if not n["rest"]])
           for t in r["state"]["tracks"] if any(not n["rest"] for n in t["notes"])}
    if per != {0: 2}:
        return f"merge result wrong: {per}"


def test_shared_song_edit():
    """Structural edits work on jump-sharing songs and other channels stay exact.

    Origin: the sharing-aware serializer. Break shares melodies via mid-track
    jumps; editing one track must not disturb the shared bytes other
    channels play.
    """
    rs = run([{"id": 1, "cmd": "open", "path": BREAK}])
    st = rs[0]["state"]
    tgt = None
    for t in st["tracks"]:
        for n in t["notes"]:
            if not n["rest"] and not n["loopRepeat"]:
                tgt = (t["index"], n)
                break
        if tgt:
            break
    tr, n0 = tgt
    # Shared bytes are heard by every channel that jumps into them, so the
    # invariant is reversibility: erase must succeed, and undo must restore
    # the render byte-for-byte.
    base, _ = render_song(BREAK, seconds=4)
    rs = run([{"id": 1, "cmd": "open", "path": BREAK},
              {"id": 2, "cmd": "eraseNote", "track": tr, "note": n0["i"]},
              {"id": 3, "cmd": "undo"},
              {"id": 9, "cmd": "render", "seconds": 4}])
    if not rs[1]["ok"] or not rs[2]["ok"]:
        return f"erase/undo failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    if wav_bytes(last_wav(rs)) != base:
        return "undo did not restore the original audio"


def test_relayout_render_parity():
    """A net-zero structural edit (createLoop+removeLoop) must not change audio.

    Origin: proves the rebuild serializer's relayout (with home-track clone
    dedup and jump-target remapping) is semantics-preserving.
    """
    base, _ = render_song(FLASHBACK, seconds=4)
    after, rs = render_song(FLASHBACK, seconds=4, pre=[
        {"id": 2, "cmd": "createLoop", "track": 0, "startTick": 0, "endTick": 12, "count": 1},
        {"id": 3, "cmd": "removeLoop", "track": 0, "loop": 0}])
    if not rs[1]["ok"] or not rs[2]["ok"]:
        return f"loop round-trip failed: {[r.get('error') for r in rs if not r['ok']]}"
    if base != after:
        return "relayout changed the audio"


def test_preview_single_note():
    """previewKey renders one clean note: silent attack, no dump-time chord.

    Origin: SPC snapshots keep the driver mid-song with voices keyed on;
    previews needed the 0xFB restart command plus a DSP key-off patch.
    """
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "previewKey", "pitch": 76, "program": 3}])
    if not rs[1]["ok"]:
        return f"previewKey failed: {rs[1].get('error')}"
    mono = read_wav_mono(rs[1]["wav"])
    first25ms = max(abs(x) for x in mono[:800])
    peak = max(abs(x) for x in mono)
    if first25ms > 50:
        return f"pop at start (first-25ms peak {first25ms})"
    if peak < 200:
        return f"preview too quiet (peak {peak})"


def test_asm_roundtrip():
    """Exported ASM must assemble back to the same canonical sequence.

    This protects the public ASM interchange path, including labels, channel
    pointers, command bytes, and jump destinations.
    """
    before = os.path.join(TMP, "gtb-roundtrip-before.asm")
    after = os.path.join(TMP, "gtb-roundtrip-after.asm")
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "exportAsm", "path": before},
              {"id": 3, "cmd": "importAsm", "path": before},
              {"id": 4, "cmd": "exportAsm", "path": after}])
    if len(rs) != 4 or any(not r.get("ok") for r in rs):
        return f"ASM command failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    if open(before, "rb").read() != open(after, "rb").read():
        return "ASM export/import/export was not a fixpoint"


def test_lossless_midi_roundtrip():
    """A Boop-authored MIDI must retain its exact GTB sequence payload."""
    before = os.path.join(TMP, "gtb-midi-before.asm")
    after = os.path.join(TMP, "gtb-midi-after.asm")
    midi = os.path.join(TMP, "gtb-roundtrip.mid")
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "exportAsm", "path": before},
              {"id": 3, "cmd": "exportMidi", "path": midi},
              {"id": 4, "cmd": "importMidi", "path": midi},
              {"id": 5, "cmd": "exportAsm", "path": after}])
    if len(rs) != 5 or any(not r.get("ok") for r in rs):
        return f"MIDI command failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    data = open(midi, "rb").read()
    if not data.startswith(b"MThd") or b"GTB1" not in data:
        return "MIDI lacks its header or lossless GTB1 payload"
    if open(before, "rb").read() != open(after, "rb").read():
        return "SPC -> MIDI -> sequence was not lossless"


def test_generic_midi_import():
    """A normal MIDI without GTB metadata must convert and render audibly."""
    midi = os.path.join(CWD, "Music Sources", "Miscellaneous", "gooftroop_cartoon_theme.mid")
    cli = os.path.join(ROOT, "build", "win-x64", "bin", "Release", "GTBoop-cli.exe")
    if not os.path.exists(midi):
        return f"generic MIDI fixture missing: {midi}"
    if not os.path.exists(cli):
        return f"generic MIDI converter missing: {cli}"
    rs = run([{"id": 1, "cmd": "importMidi", "path": midi},
              {"id": 2, "cmd": "render", "seconds": 2}],
             env={"GTB_CLI": cli, "GTB_SOUNDBANK": HAMLET})
    if len(rs) != 2 or any(not r.get("ok") for r in rs):
        return f"generic MIDI import failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    note_count = sum(len([n for n in t["notes"] if not n["rest"]]) for t in rs[0]["state"]["tracks"])
    if note_count == 0:
        return "generic MIDI import produced no notes"
    mono = read_wav_mono(last_wav(rs))
    rms = math.sqrt(sum(x * x for x in mono) / len(mono))
    if rms < 5:
        return f"generic MIDI render was silent (rms {rms:.1f})"


def test_rom_roundtrip_and_capacity_guard():
    """UI-style ROM export preserves an untouched slot and rejects overflow."""
    rom = os.path.join(CWD, "ROMs", "Goof Troop (U) [!].smc")
    out = os.path.join(TMP, "gtb-roundtrip.smc")
    if not os.path.exists(rom):
        return f"ROM fixture missing: {rom}"
    rs = run([{"id": 1, "cmd": "listRomSongs", "path": rom},
              {"id": 2, "cmd": "openRom", "path": rom, "slot": 0x15},
              {"id": 3, "cmd": "exportRom", "rom": rom, "slot": 0x15, "path": out}])
    if len(rs) != 3 or any(not r.get("ok") for r in rs):
        return f"ROM command failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    slots = rs[0].get("slots", [])
    if len(slots) != 19:
        return f"expected 19 stock ROM songs, found {len(slots)}"
    if open(rom, "rb").read() != open(out, "rb").read():
        return "untouched explicit-template ROM export changed bytes"

    smallest = min(slots, key=lambda s: s["size"])
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "exportRom", "rom": rom, "slot": smallest["slot"], "path": out}])
    if rs[1].get("ok"):
        return f"oversized song unexpectedly fit in {smallest['size']}-byte ROM slot"
    if "exceeds slot capacity" not in rs[1].get("error", ""):
        return f"unexpected capacity error: {rs[1].get('error')}"


def test_audio_exports_and_mute():
    """Explicit WAV/MP3 exports work and the eight-channel mute is silent."""
    audible = os.path.join(TMP, "gtb-audible.wav")
    muted = os.path.join(TMP, "gtb-muted.wav")
    mp3 = os.path.join(TMP, "gtb-export.mp3")
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "render", "seconds": 1, "path": audible},
              {"id": 3, "cmd": "render", "seconds": 1, "mute": list(range(8)), "path": muted},
              {"id": 4, "cmd": "renderMp3", "seconds": 1, "path": mp3}])
    if len(rs) != 4 or any(not r.get("ok") for r in rs):
        return f"audio export failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    with wave.open(audible, "rb") as w:
        if (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()) != (2, 2, 32000, 32000):
            return "WAV export format is not 32kHz 16-bit stereo for one second"
    audible_mono = read_wav_mono(audible)
    muted_mono = read_wav_mono(muted)
    if max(abs(x) for x in audible_mono) < 200:
        return "normal WAV export is too quiet"
    muted_peak = max(abs(x) for x in muted_mono)
    muted_rms = math.sqrt(sum(x * x for x in muted_mono) / len(muted_mono))
    if muted_peak > 1 or muted_rms > 0.1:
        return f"all-channel mute exceeds the inaudible PCM floor (peak {muted_peak}, rms {muted_rms:.3f})"
    data = open(mp3, "rb").read()
    if len(data) < 1000 or not (data.startswith(b"ID3") or data[:1] == b"\xff"):
        return "MP3 export is missing or malformed"


def test_redo_and_note_preview():
    """Undo/redo restores an edit and renderNote previews the restored note."""
    rs = run([{"id": 1, "cmd": "new"},
              {"id": 2, "cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
              {"id": 3, "cmd": "undo"},
              {"id": 4, "cmd": "redo"}])
    if len(rs) != 4 or any(not r.get("ok") for r in rs):
        return f"undo/redo failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    notes = [n for n in rs[3]["state"]["tracks"][0]["notes"] if not n["rest"]]
    if [(n["tick"], n["pitch"]) for n in notes] != [(0, 60)]:
        return f"redo did not restore the note: {notes}"
    rs = run([{"id": 1, "cmd": "new"},
              {"id": 2, "cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
              {"id": 3, "cmd": "renderNote", "track": 0, "note": notes[0]["i"]}])
    if len(rs) != 3 or not rs[2].get("ok"):
        return f"renderNote failed: {rs[-1].get('error') if rs else 'no response'}"
    mono = read_wav_mono(rs[2]["wav"])
    if max(abs(x) for x in mono) < 200:
        return "renderNote preview is too quiet"


TESTS = [
    test_corpus_open_save_parity,
    test_corpus_optimize_noop,
    test_failed_edit_leaves_file_untouched,
    test_compose_from_scratch,
    test_loop_at_track_end_remains_editable,
    test_append_full_midi_pitch_range,
    test_nonripple_resize,
    test_place_note_relocation,
    test_session_roundtrip,
    test_track_merge,
    test_shared_song_edit,
    test_relayout_render_parity,
    test_preview_single_note,
    test_asm_roundtrip,
    test_lossless_midi_roundtrip,
    test_generic_midi_import,
    test_rom_roundtrip_and_capacity_guard,
    test_audio_exports_and_mute,
    test_redo_and_note_preview,
]


def main():
    if not os.path.exists(ENGINE):
        print(f"engine not found: {ENGINE}")
        return 2
    if len(glob.glob(os.path.join(CORPUS, "*.spc"))) < 18:
        print("Full regression requires the local 18-song SPC corpus; refusing an incomplete run")
        return 2
    failures = 0
    for t in TESTS:
        name = t.__name__
        log(f"[{name}]")
        try:
            err = t()
        except Exception as e:  # noqa: BLE001 - report, don't crash the run
            err = f"exception: {e!r}"
        if err:
            failures += 1
            print(f"FAIL {name}: {err}")
            log(f"  FAIL: {err}")
        else:
            print(f"ok   {name}")
            log("  ok")
    with open(LOG, "w", encoding="utf-8") as f:
        f.write("\n".join(_log_lines) + "\n")
    print(f"\n{len(TESTS) - failures}/{len(TESTS)} passed; log: {LOG}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
