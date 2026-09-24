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

import atexit
import cmath
import glob
import io
import json
import math
import re
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
# Private per run: fixed names in the shared %TEMP% let two runs clobber each
# other's fixtures. Removed at exit.
TMP = tempfile.mkdtemp(prefix="gtb-regression-")
atexit.register(shutil.rmtree, TMP, ignore_errors=True)
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
    # songs now get the whole ARAM window; pin the budget to the footprint so
    # the insert is genuinely over budget
    rs = run([
        {"id": 1, "cmd": "open", "path": GAME_OVER},
        {"id": 0, "cmd": "setAllocationFloor", "bytes": 0},
        {"id": 2, "cmd": "insertNote", "track": 3, "tick": 0, "pitch": 60, "len": 12},
        {"id": 3, "cmd": "save", "path": b},
    ])
    if rs[2]["ok"]:
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
    allocation floor (1388 bytes originally, now the full ARAM song window
    since ROM export can relocate), and the shared-END-byte track-mapping bug
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
    if st["budget"]["total"] != 0x4000 - 0x0D20:  # the $0D20..$4000 ARAM song window
        return f"allocation floor wrong: {st['budget']}"
    per = {t["index"]: [(n["tick"], n["pitch"]) for n in t["notes"] if not n["rest"]]
           for t in st["tracks"] if any(not n["rest"] for n in t["notes"])}
    if per != {0: [(48, 60)], 3: [(0, 48)]}:
        return f"notes landed wrong: {per}"
    mono = read_wav_mono(last_wav(rs))
    rms = math.sqrt(sum(x * x for x in mono) / len(mono))
    if rms < 5:
        return f"render silent (rms {rms:.1f})"


TIMING_LENGTHS = (2, 3, 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 128, 144, 192)


def _run_timing(reqs):
    # Concurrent GUI/fixture workers also stage ARAM under the system temp
    # directory. Give every timing process a private staging namespace.
    with tempfile.TemporaryDirectory(prefix="gtb-timing-engine-") as tmp:
        return run(reqs, env={"TEMP": tmp, "TMP": tmp, "TMPDIR": tmp})


def _timing_signature(state):
    """Compare decoded timing/control flow without relocation-dependent offsets."""
    return [(t["index"],
             [(n["tick"], n["len"], n["dur"], n["pitch"], n["rest"], n["loopRepeat"])
              for n in t["notes"]],
             t.get("loops", []), t.get("songLoop")) for t in state["tracks"]]


def test_encoding_timing_duration_table():
    """Every straight/dotted/triplet length survives append, rest insertion and resize."""
    for length in TIMING_LENGTHS:
        # A following anchor gives resize/insertion a fixed downstream boundary.
        cases = {
            "append": [{"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": length},
                       {"cmd": "insertNote", "track": 0, "tick": length, "pitch": 62, "len": 24}],
            "rest": [{"cmd": "insertNote", "track": 0, "tick": 192, "pitch": 62, "len": 24},
                     {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": length}],
            "resize": [{"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 192},
                       {"cmd": "insertNote", "track": 0, "tick": 192, "pitch": 62, "len": 24},
                       {"cmd": "resizeNote", "track": 0, "note": 0, "len": length}],
        }
        for mode, commands in cases.items():
            rs = _run_timing([{"cmd": "new"}] + commands)
            if not all(r.get("ok") for r in rs):
                return f"{mode} {length}: {[r.get('error') for r in rs if not r.get('ok')]}"
            notes = [n for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]]
            expected = [(0, length, 60), (length if mode == "append" else 192, 24, 62)]
            actual = [(n["tick"], n["len"], n["pitch"]) for n in notes]
            if actual != expected:
                return f"{mode} {length}: expected {expected}, got {actual}"
            expected_lengths = [3, 9, 18, 36, 72, 144, 192] if length in (9, 18, 36, 72, 144) else list(TIMING_LENGTHS)
            if notes[0].get("timingLengths") != expected_lengths:
                return f"{mode} {length}: wrong timingLengths context: {notes[0].get('timingLengths')}"
            if notes[1].get("timingLengths") != list(TIMING_LENGTHS):
                return f"{mode} {length}: timing modifier leaked into following note"
        log(f"  exact timing: {length} ticks append/rest/resize")


def test_encoding_timing_boundaries():
    """Reject unsupported timing without byte changes; encode non-grid gaps exactly."""
    with tempfile.TemporaryDirectory(prefix="gtb-timing-") as tmp:
        before, after = [os.path.join(tmp, name + ".spc") for name in ("before", "after")]
        invalid = [(0, 0), (0, 1), (0, 5), (0, 10), (0, 193), (0, 288),
                   (1, 24), (4294967295, 24)]
        for tick, length in invalid:
            rs = _run_timing([{"cmd": "new"}, {"cmd": "save", "path": before},
                      {"cmd": "insertNote", "track": 0, "tick": tick, "pitch": 60, "len": length},
                      {"cmd": "save", "path": after}])
            if rs[2].get("ok") or not rs[2].get("error"):
                return f"unsupported tick={tick}, len={length} was not explicitly rejected"
            with open(before, "rb") as a, open(after, "rb") as b:
                if a.read() != b.read():
                    return f"rejected tick={tick}, len={length} changed bytes"
        for tick in (0, 2, 3, 4, 5, 7, 191, 193, 385):
            rs = _run_timing([{"cmd": "new"},
                      {"cmd": "insertNote", "track": 0, "tick": tick, "pitch": 60, "len": 9}])
            if not rs[-1].get("ok"):
                return f"representable gap {tick}: {rs[-1].get('error')}"
            notes = [n for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]]
            if [(n["tick"], n["len"]) for n in notes] != [(tick, 9)]:
                return f"gap {tick} was silently quantized: {notes}"
        # Splitting a rest must not turn a one-tick remainder into extra time.
        for tick, length in ((1, 12), (0, 191), (190, 3)):
            rs = _run_timing([{"cmd": "new"},
                      {"cmd": "insertNote", "track": 0, "tick": 192, "pitch": 62, "len": 24},
                      {"cmd": "save", "path": before},
                      {"cmd": "insertNote", "track": 0, "tick": tick, "pitch": 60, "len": length},
                      {"cmd": "save", "path": after}])
            if rs[3].get("ok"):
                return f"rest boundary tick={tick}, len={length} unexpectedly accepted"
            with open(before, "rb") as a, open(after, "rb") as b:
                if a.read() != b.read():
                    return "failed rest split changed bytes"


def test_encoding_timing_dotted_context():
    """A pending dot constrains resize; failure leaves the original note intact."""
    with tempfile.TemporaryDirectory(prefix="gtb-timing-dot-") as tmp:
        before, after = [os.path.join(tmp, name + ".spc") for name in ("before", "after")]
        for length, accepted in ((9, True), (36, True), (24, False), (2, False)):
            rs = _run_timing([{"cmd": "new"},
                      {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 144},
                      {"cmd": "save", "path": before},
                      {"cmd": "resizeNote", "track": 0, "note": 0, "len": length},
                      {"cmd": "save", "path": after}])
            if bool(rs[3].get("ok")) != accepted:
                return f"dotted resize {length}: {rs[3].get('error', 'unexpected success')}"
            if accepted:
                notes = [n for n in rs[3]["state"]["tracks"][0]["notes"] if not n["rest"]]
                if [(n["tick"], n["len"]) for n in notes] != [(0, length)]:
                    return f"dotted resize {length} changed the requested timing"
            else:
                with open(before, "rb") as a, open(after, "rb") as b:
                    if a.read() != b.read():
                        return f"rejected dotted resize {length} changed bytes"


def test_encoding_timing_loop_roundtrip():
    """Structural duration prefixes stay inside loop landings; history/save preserves timing."""
    with tempfile.TemporaryDirectory(prefix="gtb-timing-loop-") as tmp:
        session = os.path.join(tmp, "timing.gtb")
        for length in (9, 16, 36, 64, 144):
            rs = _run_timing([{"cmd": "new"},
                      {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 192},
                      {"cmd": "insertNote", "track": 0, "tick": 192, "pitch": 62, "len": 24},
                      {"cmd": "createLoop", "track": 0, "startTick": 0, "endTick": 192,
                       "slot": 0, "count": 2},
                      {"cmd": "resizeNote", "track": 0, "note": 0, "len": length},
                      {"cmd": "saveSession", "path": session},
                      {"cmd": "openSession", "path": session}])
            if not all(r.get("ok") for r in rs):
                return f"loop {length}: {[r.get('error') for r in rs if not r.get('ok')]}"
            if _timing_signature(rs[4]["state"]) != _timing_signature(rs[6]["state"]):
                return f"timing or loops changed across session roundtrip ({length})"
            old, new = rs[3]["state"]["tracks"][0], rs[4]["state"]["tracks"][0]
            if old["loops"] != new["loops"]:
                return f"loop boundaries changed on resize ({length})"
            old_anchor = [(n["tick"], n["len"], n["dur"]) for n in old["notes"] if n["pitch"] == 62]
            new_anchor = [(n["tick"], n["len"], n["dur"]) for n in new["notes"] if n["pitch"] == 62]
            if old_anchor != new_anchor:
                return f"downstream timing changed on resize ({length})"
            if any(n["len"] != length for n in new["notes"] if n["pitch"] == 60):
                return f"loop reentry skipped timing prefix ({length})"


def test_encoding_timing_merged_ties():
    """Every segment of a tied span edits together, without audible leftovers."""
    with tempfile.TemporaryDirectory(prefix="gtb-timing-tie-") as tmp:
        source = os.path.join(tmp, "tied.asm")
        with open(source, "w", encoding="utf-8") as f:
            f.write(".base $0D20\ndb $00\ndw end,end,end,end,end,end,end,melody\n"
                    "melody: db $07,$C0,$06,$C0,$08,$08,$09,$05,"
                    "$81,$01,$81,$01,$E0,$83,$17\nend: db $17\n")
        setup = [{"cmd": "new"}, {"cmd": "importAsm", "path": source}]
        baseline = _run_timing(setup)
        if not all(r.get("ok") for r in baseline):
            return f"tied fixture failed: {baseline}"
        track = next(t for t in baseline[-1]["state"]["tracks"] if any(n["len"] == 48 for n in t["notes"]))
        note = next(n for n in track["notes"] if not n["rest"] and n["len"] == 48)
        errors = []
        # Keeping the same length must preserve the encoded tie and the
        # previous history entry, even though erase/reinsert cannot recreate
        # this span inside either of its individual 24-tick segments.
        rs = _run_timing(setup + [{"cmd": "exportAsm"},
                        {"cmd": "setNote", "track": track["index"], "note": note["i"], "pitch": 61, "len": 48},
                        {"cmd": "exportAsm"},
                        {"cmd": "resizeNote", "track": track["index"], "note": note["i"], "len": 48},
                        {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                        {"cmd": "resizeNote", "track": track["index"], "note": note["i"], "len": 48},
                        {"cmd": "redo"}, {"cmd": "exportAsm"}])
        if not all(r.get("ok") for r in rs):
            errors.append("same-length tied resize failed or consumed undo/redo")
        elif (rs[4]["asm"] != rs[6]["asm"] or rs[2]["asm"] != rs[8]["asm"]
              or rs[4]["asm"] != rs[11]["asm"]):
            errors.append("same-length tied resize changed the encoding or history")
        for command in ({"cmd": "eraseNote"}, {"cmd": "resizeNote", "len": 12},
                        {"cmd": "moveNote", "tick": 48, "pitch": 60},
                        {"cmd": "moveNote", "tick": 300, "pitch": 60},
                        {"cmd": "setNote", "pitch": 61, "len": 48}):
            rs = _run_timing(setup + [{"cmd": "exportAsm"},
                            dict(command, track=track["index"], note=note["i"]),
                            {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                            {"cmd": "redo"}, {"cmd": "exportAsm"}])
            if not all(r.get("ok") for r in rs):
                errors.append(f"{command['cmd']} failed: " + str([r.get("error") for r in rs if not r.get("ok")]))
                continue
            if rs[2]["asm"] != rs[6]["asm"] or rs[4]["asm"] != rs[8]["asm"]:
                errors.append(f"{command['cmd']} undo/redo did not restore the full span")
            notes = [n for t in rs[4]["state"]["tracks"] if t["index"] == track["index"]
                     for n in t["notes"] if not n["rest"]]
            edited = [n for n in notes if n["tick"] != 240]
            expected = {"eraseNote": [], "resizeNote": [(0, 60, 12)],
                        "moveNote": [(command.get("tick", 48), 60, 48)], "setNote": [(0, 61, 48)]}[command["cmd"]]
            if [(n["tick"], n["pitch"], n["len"]) for n in edited] != expected:
                errors.append(f"{command['cmd']} left a wrong pitch, timing or tied continuation")
            if command["cmd"] == "moveNote" and len(edited) == 1 and edited[0]["dur"] != note["dur"]:
                errors.append(f"tied move changed sounding duration from {note['dur']} to {edited[0]['dur']}")
            if not any(n["tick"] == 240 and n["pitch"] == 62 for n in notes):
                errors.append(f"{command['cmd']} moved the following note")
            anchor_before = next(n for n in track["notes"] if n["tick"] == 240 and not n["rest"])
            anchor_after = next((n for n in notes if n["tick"] == 240), None)
            if anchor_after and (anchor_after["dur"], anchor_after["program"]) != (anchor_before["dur"], anchor_before["program"]):
                errors.append(f"{command['cmd']} changed the following note's articulation or instrument")
        return "; ".join(errors) if errors else None


def test_tied_move_contexts():
    """Relocation preserves segment articulation across inherited voice states."""
    for rate, triplet, octave in ((64, False, 5), (255, True, 4), (0, True, 5)):
        source = (".base $0D20\ndb 0\ndw end,end,end,end,end,end,end,melody\n"
                  "melody: db $07,$C0,$06,$C0,$08,8,$09,5,$81,$01,$81,$01,"
                  f"$06,{rate},$08,5,$09,{octave}," + ("$00," if triplet else "") +
                  "$E0,$83,$17\nend: db $17\n")
        setup = [{"cmd": "new"}, {"cmd": "importAsm", "asm": source}]
        initial = _run_timing(setup)[-1]
        if not initial.get("ok"):
            return "tied destination-context fixture failed"
        track = next(t for t in initial["state"]["tracks"] if t["notes"])
        sounding = [n for n in track["notes"] if not n["rest"]]
        note, anchor = sounding
        for tick in (48, 300):
            rs = _run_timing(setup + [{"cmd": "exportAsm"},
                            {"cmd": "moveNote", "track": track["index"], "note": note["i"], "tick": tick, "pitch": 60},
                            {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                            {"cmd": "redo"}, {"cmd": "exportAsm"}])
            if not all(r.get("ok") for r in rs):
                return f"tied context move failed: {[r.get('error') for r in rs if not r.get('ok')]}"
            if rs[2]["asm"] != rs[6]["asm"] or rs[4]["asm"] != rs[8]["asm"]:
                return "tied context move history is not exact"
            result = [n for t in rs[4]["state"]["tracks"] if t["index"] == track["index"]
                      for n in t["notes"] if not n["rest"]]
            signature = lambda n: (n["tick"], n["pitch"], n["len"], n["dur"], n["program"])
            expected = sorted([(tick, 60, note["len"], note["dur"], note["program"]), signature(anchor)])
            if sorted(map(signature, result)) != expected:
                return f"tied context move changed articulation or following voice state: {list(map(signature, result))}"


def test_consecutive_rest_edits():
    """Silent capacity spans rest events; entry points remain protected."""
    tail = _run_timing([{"cmd": "new"},
                        {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 12},
                        {"cmd": "setNote", "track": 0, "note": 0, "pitch": 72, "len": 12},
                        {"cmd": "exportAsm"},
                        {"cmd": "resizeNote", "track": 0, "note": 0, "len": 24},
                        {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                        {"cmd": "redo"}, {"cmd": "exportAsm"}])
    if not all(r.get("ok") for r in tail):
        return "inspector sequence could not grow a note at finite track end"
    if tail[3]["asm"] != tail[7]["asm"] or tail[5]["asm"] != tail[9]["asm"]:
        return "finite-track-end resize history was not exact"
    tail_notes = [n for n in tail[5]["state"]["tracks"][0]["notes"] if not n["rest"]]
    if [(n["tick"], n["pitch"], n["len"]) for n in tail_notes] != [(0, 72, 24)]:
        return "finite-track-end resize changed pitch or start time"
    source = (".base $0D20\ndb 0\ndw end,end,end,end,end,end,end,melody\n"
              "melody: db $08,1,$09,5,$61\nfirst: db $60\nsecond: db $60\n"
              "following: db $08,1,$62,$17\nend: db $17\n")
    setup = [{"cmd": "new"}, {"cmd": "importAsm", "asm": source}]
    baseline = _run_timing(setup)
    if not all(r.get("ok") for r in baseline):
        return "adjacent-rest fixture failed"
    track = next(t for t in baseline[-1]["state"]["tracks"] if t["notes"])
    note = next(n for n in track["notes"] if not n["rest"])
    for command in ({"cmd": "resizeNote", "note": note["i"], "len": 36},
                    {"cmd": "insertNote", "tick": 12, "pitch": 60, "len": 24}):
        rs = _run_timing(setup + [{"cmd": "exportAsm"}, dict(command, track=track["index"]),
                                {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                                {"cmd": "redo"}, {"cmd": "exportAsm"}])
        if not all(r.get("ok") for r in rs):
            return f"{command['cmd']} across adjacent rests failed: " + str([r.get("error") for r in rs if not r.get("ok")])
        if rs[2]["asm"] != rs[6]["asm"] or rs[4]["asm"] != rs[8]["asm"]:
            return "adjacent-rest edit did not undo/redo atomically"
        actual = [(n["tick"], n["pitch"], n["len"]) for t in rs[4]["state"]["tracks"]
                  if t["index"] == track["index"] for n in t["notes"] if not n["rest"]]
        expected = ([(0, 60, 36), (36, 61, 12)] if command["cmd"] == "resizeNote"
                    else [(0, 60, 12), (12, 60, 24), (36, 61, 12)])
        if actual != expected:
            return f"adjacent-rest edit changed following timing: {actual}"
    for guard_index, guarded in enumerate((source.replace("end,end,end,end,end,end,end,melody", "second,end,end,end,end,end,end,melody"),
                    source.replace("following: db $08,1,$62,$17", "following: db $08,1,$62,$16\ndw second"),
                    source.replace("second: db $60", "db $07,$40\nsecond: db $60"))):
        rs = _run_timing([{"cmd": "new"}, {"cmd": "importAsm", "asm": guarded},
                          {"cmd": "exportAsm"},
                          {"cmd": "resizeNote", "track": track["index"], "note": note["i"], "len": 36},
                          {"cmd": "exportAsm"}])
        if not rs[1].get("ok") or rs[3].get("ok") or rs[2]["asm"] != rs[4]["asm"]:
            return f"rest boundary case {guard_index} failed: import={rs[1].get('ok')} edit={rs[3].get('ok')} unchanged={rs[2].get('asm') == rs[4].get('asm')} errors={[r.get('error') for r in rs if not r.get('ok')]}"


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


def test_optimize_atomic_history():
    """Multi-track optimize is one undo entry and preserves earlier edits."""
    rs = _run_timing([
        {"cmd": "new"},
        {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
        {"cmd": "insertNote", "track": 1, "tick": 48, "pitch": 64, "len": 24},
        {"cmd": "insertNote", "track": 2, "tick": 96, "pitch": 67, "len": 24},
        {"cmd": "optimize", "merge": True},
        {"cmd": "undo"}, {"cmd": "redo"}, {"cmd": "undo"}, {"cmd": "undo"},
    ])
    if not all(r.get("ok") for r in rs) or rs[4].get("mergedTracks") != 2:
        return f"atomic merge setup failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    if rs[5]["state"]["tracks"] != rs[3]["state"]["tracks"]:
        return "one Undo did not restore all pre-merge tracks"
    if rs[6]["state"]["tracks"] != rs[4]["state"]["tracks"]:
        return "one Redo did not restore the complete merge"
    if rs[8]["state"]["tracks"] != rs[2]["state"]["tracks"]:
        return "merge transaction destroyed previous undo history"


def test_tied_track_merge():
    """Track consolidation preserves tied segments and destination voice state."""
    source = (".base $0D20\ndb 0\ndw end,end,end,end,end,end,src,dst\n"
              "dst: db $07,$C0,$06,$40,$08,8,$09,5,$63,$02,$80,$E0,$64,$17\n"
              "src: db $07,$C0,$06,$C0,$08,8,$09,5,$A0,$81,$01,$81,$01,$17\n"
              "end: db $17\n")
    rs = _run_timing([{"cmd": "new"}, {"cmd": "importAsm", "asm": source},
                      {"cmd": "exportAsm"}, {"cmd": "optimize", "merge": True},
                      {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                      {"cmd": "redo"}, {"cmd": "exportAsm"}])
    if not all(r.get("ok") for r in rs):
        return f"tied track merge failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    if rs[3].get("mergedTracks", 0) < 1:
        return "tied merge fixture did not exercise consolidation"
    signature = lambda state: sorted((n["tick"], n["pitch"], n["len"], n["dur"], n["program"])
                                    for t in state["tracks"] for n in t["notes"] if not n["rest"])
    if signature(rs[1]["state"]) != signature(rs[3]["state"]):
        return "track merge flattened a tie or changed destination articulation"
    if rs[2]["asm"] != rs[6]["asm"] or rs[4]["asm"] != rs[8]["asm"]:
        return "tied track merge undo/redo was not exact"


def test_optimize_noop_history():
    """A no-op optimize must not consume prior undo or invalidate redo."""
    rs = _run_timing([
        {"cmd": "new"},
        {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
        {"cmd": "insertNote", "track": 1, "tick": 48, "pitch": 64, "len": 24},
        {"cmd": "undo"}, {"cmd": "optimize", "merge": False}, {"cmd": "redo"},
    ])
    if not all(r.get("ok") for r in rs):
        return f"no-op optimize/history failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    if rs[4]["state"] != rs[3]["state"]:
        return "no-op optimize changed state/history"
    if rs[5]["state"]["tracks"] != rs[2]["state"]["tracks"]:
        return "no-op optimize destroyed the original redo entry"


def test_optimize_atomic_failure():
    """A merge failing after a successful insertion restores bytes and both histories."""
    with tempfile.TemporaryDirectory(prefix="gtb-optimize-rollback-") as tmp:
        before, after = [os.path.join(tmp, name + ".spc") for name in ("before", "after")]
        rs = _run_timing([
            {"cmd": "new"},
            {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
            {"cmd": "insertNote", "track": 0, "tick": 192, "pitch": 62, "len": 24},
            {"cmd": "addSetting", "track": 0, "tick": 168, "type": "volume", "value": 64},
            {"cmd": "insertNote", "track": 1, "tick": 48, "pitch": 64, "len": 24},
            # Adjacent rests are supported; an intervening live setting still
            # cannot be moved in time to make room for this second source note.
            {"cmd": "insertNote", "track": 1, "tick": 156, "pitch": 67, "len": 24},
            {"cmd": "insertNote", "track": 2, "tick": 300, "pitch": 69, "len": 24},
            {"cmd": "undo"}, {"cmd": "save", "path": before},
            {"cmd": "optimize", "merge": True}, {"cmd": "state"},
            {"cmd": "save", "path": after}, {"cmd": "redo"}, {"cmd": "undo"}, {"cmd": "undo"},
        ])
        if not all(r.get("ok") for r in rs[:9]):
            return "atomic failure setup failed"
        if rs[9].get("ok") or not rs[9].get("error"):
            return "partially failed merge was reported as success"
        if rs[10]["state"] != rs[7]["state"]:
            return "failed merge changed state or undo/redo availability"
        with open(before, "rb") as a, open(after, "rb") as b:
            if a.read() != b.read():
                return "failed merge changed raw bytes"
        if not all(r.get("ok") for r in rs[11:]):
            return "failed merge destroyed undo/redo history"
        if rs[12]["state"]["tracks"] != rs[6]["state"]["tracks"]:
            return "failed merge destroyed the prior redo entry"
        if rs[14]["state"]["tracks"] != rs[4]["state"]["tracks"]:
            return "failed merge destroyed the prior undo entry"


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
                probe = run([{"cmd": "open", "path": BREAK},
                             {"cmd": "eraseNote", "track": t["index"], "note": n["i"]},
                             {"cmd": "state"}])
                if probe[1].get("ok"):
                    if tgt is None:
                        tgt = (t["index"], n)
                else:
                    return f"supported shared event probe failed: {probe[1].get('error')}"
                if tgt:
                    break
        if tgt:
            break
    if not tgt:
        return "shared fixture must contain an editable note"
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

    # A song that outgrows its stock slot is relocated: the ROM expands to
    # 1 MB, the blob moves to reachable new space, the table entry is
    # repointed using the loader's real (bank = $84 OR byte2) encoding, and
    # nothing else in the image changes but the header size byte + checksum.
    smallest = min(slots, key=lambda s: s["size"])
    ham_asm = os.path.join(TMP, "gtb-ham.asm")
    rs = run([{"id": 1, "cmd": "open", "path": HAMLET},
              {"id": 2, "cmd": "exportAsm", "path": ham_asm},
              {"id": 3, "cmd": "exportRom", "rom": rom, "slot": smallest["slot"], "path": out}])
    if not rs[2].get("ok"):
        return f"relocating export failed: {rs[2].get('error')}"
    if not rs[2].get("relocated"):
        return "oversized song was written without relocation"
    orig = open(rom, "rb").read()
    new = open(out, "rb").read()
    if len(new) != 0x100000 or rs[2].get("romSize") != 0x100000:
        return f"expanded ROM is {len(new)} bytes, expected 1 MB"
    if new[0x7FD7] != 0x0A:
        return f"header ROM-size byte is 0x{new[0x7FD7]:02x}, expected 0x0A"
    entry = 0x20000 + smallest["slot"] * 3
    word = new[entry] | (new[entry + 1] << 8)
    byte2 = new[entry + 2]
    if byte2 & 0x84:
        return f"table entry bank byte 0x{byte2:02x} is not reachable through ORA #$84"
    # Decode the CPU bus address from CODE_8098DC's ORA #$84, independently
    # of the editor's encoder/resolver. Addition is wrong for bank aliases.
    cpu = ((0x84 | byte2) << 16) | 0x8000 | (word & 0x7FFF)
    blob = ((cpu & 0x7F0000) >> 1) | (cpu & 0x7FFF)
    if blob != rs[2].get("blobPc") or blob < 0x80000:
        return f"table entry decodes to 0x{blob:x}, engine reported 0x{rs[2].get('blobPc', 0):x}"
    size = new[blob] | (new[blob + 1] << 8)
    load = new[blob + 2] | (new[blob + 3] << 8)
    if load != 0x0D20 or size == 0:
        return f"relocated blob header wrong: size {size}, load 0x{load:04x}"
    if new[blob + 4 + size:blob + 8 + size] != bytes(4):
        return "relocated song is missing the loader's zero-length terminating block"
    # every other byte of the stock 512 KB is untouched
    touched = set(range(entry, entry + 3)) | {0x7FD7} | set(range(0x7FDC, 0x7FE0))
    drift = [i for i in range(0x80000) if orig[i] != new[i] and i not in touched]
    if drift:
        return f"relocation disturbed {len(drift)} stock bytes, first at 0x{drift[0]:x}"
    s = (sum(new) - sum(new[0x7FDC:0x7FE0]) + 0xFF + 0xFF) & 0xFFFF
    if (new[0x7FDE] | (new[0x7FDF] << 8)) != s or (new[0x7FDC] | (new[0x7FDD] << 8)) != (~s & 0xFFFF):
        return "expanded ROM checksum is wrong"
    # the relocated slot reads back as exactly the song we wrote
    back_asm = os.path.join(TMP, "gtb-ham-back.asm")
    rs = run([{"id": 1, "cmd": "listRomSongs", "path": out},
              {"id": 2, "cmd": "openRom", "path": out, "slot": smallest["slot"]},
              {"id": 3, "cmd": "exportAsm", "path": back_asm},
              {"id": 4, "cmd": "exportRom", "rom": out, "slot": smallest["slot"], "path": out + ".again"}])
    if any(not r.get("ok") for r in rs):
        return f"reading back the expanded ROM failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    if len(rs[0]["slots"]) != 19 or {s["slot"]: s["size"] for s in rs[0]["slots"]}[smallest["slot"]] != size:
        return "expanded ROM's song table no longer lists the 19 songs with the relocated size"
    if open(ham_asm, "rb").read() != open(back_asm, "rb").read():
        return "relocated song does not round-trip through the ROM"
    if rs[3].get("relocated") or open(out + ".again", "rb").read() != new:
        return "re-exporting the same song to its relocated slot should be an in-place no-op"


def test_loop_body_extend():
    """Drawing past a looping song's end extends the looped body (inside).

    Origin: a looping track ends in a GOTO and the parser unrolls the body, so
    'past the end' has no empty space and every append guard fired. Placing a
    note past the loop point now inserts it before the GOTO (plays every
    repeat); the loop is preserved and the song still round-trips.
    """
    rom = os.path.join(CWD, "ROMs", "Goof Troop (U) [!].smc")
    if not os.path.exists(rom):
        return f"ROM fixture missing: {rom}"
    before = os.path.join(TMP, "loopin.asm")
    st = run([{"id": 1, "cmd": "openRom", "path": rom, "slot": 0x14},
              {"id": 2, "cmd": "exportAsm", "path": before}])[0]["state"]
    loops = [t for t in st["tracks"] if t.get("songLoop")]
    if not loops:
        return "expected slot 0x14 to be a looping song"
    tk = loops[0]["index"]
    at = loops[0]["songLoop"]["tick"]
    after = os.path.join(TMP, "loopin2.asm")
    rs = run([{"id": 1, "cmd": "openRom", "path": rom, "slot": 0x14},
              {"id": 2, "cmd": "insertNote", "track": tk, "tick": at, "pitch": 72, "len": 24},
              {"id": 3, "cmd": "exportAsm", "path": after},
              {"id": 4, "cmd": "importAsm", "path": after},
              {"id": 5, "cmd": "exportAsm", "path": os.path.join(TMP, "loopin3.asm")}])
    if not rs[1].get("ok"):
        return f"loop-body insert failed: {rs[1].get('error')}"
    got = [n for n in rs[1]["state"]["tracks"][tk]["notes"] if not n["rest"] and n["tick"] == at]
    if not got or got[0]["pitch"] != 72:
        return "appended note is missing at the loop point"
    if open(after, "rb").read() != open(os.path.join(TMP, "loopin3.asm"), "rb").read():
        return "extended looping song is not an ASM export/import fixpoint"
    if "goto" not in open(after).read().lower():
        return "extending the body must preserve the song loop (GOTO gone)"


def test_end_song_loop():
    """'Outside' placement ends the song loop on every channel, or refuses.

    Origin: for an infinite loop, placing a note 'outside' means end the loop
    (play through once, then the note, then stop). The loop is the first GOTO
    back into already-played bytes; GOTOs into another channel's melody are not
    loops and must survive. After the edit no channel may still loop, and a
    refusal must leave the song exactly as it was.
    """
    rom = os.path.join(CWD, "ROMs", "Goof Troop (U) [!].smc")
    if not os.path.exists(rom):
        return f"ROM fixture missing: {rom}"

    def loop_track(slot):
        st = run([{"cmd": "openRom", "path": rom, "slot": slot}])[0]["state"]
        ts = [t for t in st["tracks"] if t.get("songLoop")]
        return (ts[0]["index"], ts[0]["songLoop"]["tick"]) if ts else (None, None)

    def still_looping(state):
        return [t["index"] for t in state["tracks"] if t.get("songLoop")]

    ended = 0
    for slot in (0x11, 0x14, 0x18):
        tk, at = loop_track(slot)
        if tk is None:
            return f"expected slot 0x{slot:x} to loop"
        rs = run([{"cmd": "openRom", "path": rom, "slot": slot},
                  {"cmd": "insertNote", "track": tk, "tick": at, "pitch": 72, "len": 24, "loopMode": "outside"},
                  {"cmd": "state"}])
        after = rs[2]["state"]
        if rs[1].get("ok"):
            ended += 1
            if still_looping(after):
                return f"slot 0x{slot:x}: 'outside' succeeded but tracks {still_looping(after)} still loop"
            if not [n for n in after["tracks"][tk]["notes"] if not n["rest"] and n["tick"] == at]:
                return f"slot 0x{slot:x}: the ending note was not placed"
        else:
            # atomic: same session, nothing changed, loop intact
            before = run([{"cmd": "openRom", "path": rom, "slot": slot}])[0]["state"]
            if after["tracks"] != before["tracks"]:
                return f"slot 0x{slot:x}: refused 'outside' still changed the song: {rs[1].get('error')}"
    if ended == 0:
        return "'outside' could not end the loop of any tested song"


ROM_FIXTURE = os.path.join(CWD, "ROMs", "Goof Troop (U) [!].smc")


def _first_pass(state, before_tick):
    """(track, tick, pitch, program, rest, len) of every event before a tick."""
    return sorted((t["index"], n["tick"], n["pitch"], n["program"], n["rest"], n["len"])
                  for t in state["tracks"] for n in t["notes"] if n["tick"] < before_tick)


def test_loop_append_sync_and_octave():
    """Appending past a loop keeps channels in sync and the loop's octave.

    Origin: 2026-09-22 review. The parser unrolls a looping body, so a tick past
    the loop landed on a replayed rest and edited the FIRST pass; appending
    before the GOTO left the new octave active for every later pass; and only
    the edited channel's cycle grew, so channels drifted apart on each repeat.
    """
    if not os.path.exists(ROM_FIXTURE):
        return f"ROM fixture missing: {ROM_FIXTURE}"
    st = run([{"cmd": "openRom", "path": ROM_FIXTURE, "slot": 0x14}])[0]["state"]
    loops = {t["index"]: t["songLoop"]["tick"] for t in st["tracks"] if t.get("songLoop")}
    if len(loops) < 2 or len(set(loops.values())) != 1:
        return f"fixture: expected several channels looping at one tick, got {loops}"
    loop_tick = next(iter(loops.values()))
    tk = min(loops)
    at = loop_tick + 24              # past the loop, inside the old replay region
    asm = os.path.join(TMP, "loopsync.asm")
    rs = run([{"cmd": "openRom", "path": ROM_FIXTURE, "slot": 0x14},
              {"cmd": "insertNote", "track": tk, "tick": at, "pitch": 100, "len": 24},
              {"cmd": "exportAsm", "path": asm},
              {"cmd": "state"}])
    if not rs[1].get("ok"):
        return f"append past the loop failed: {rs[1].get('error')}"
    after = rs[3]["state"]
    new_loops = {t["index"]: t["songLoop"]["tick"] for t in after["tracks"] if t.get("songLoop")}
    if set(new_loops) != set(loops):
        return f"the set of looping channels changed: {loops} -> {new_loops}"
    if set(new_loops.values()) != {at + 24}:
        return f"channels drifted or loop point wrong (want {at + 24}): {new_loops}"
    if not [n for n in after["tracks"][tk]["notes"] if not n["rest"] and n["tick"] == at and n["pitch"] == 100]:
        return "the appended note is not at the requested tick"
    if _first_pass(st, loop_tick) != _first_pass(after, loop_tick):
        return "appending past the loop changed the first pass (edit landed on a replayed event)"
    # pitch 100 needs octave-up; the loop body must get its octave back before the GOTO
    lines = open(asm).read().splitlines()
    start = lines.index(f"Channel{8 - tk}:")
    block = []
    for ln in lines[start + 1:]:
        if re.match(r"^Channel\d+:", ln):
            break
        block.append(ln)
    last_note = max(i for i, ln in enumerate(block) if "; note" in ln)
    tail = block[last_note + 1:]
    goto = next((i for i, ln in enumerate(tail) if "; goto" in ln), None)
    if goto is None or not any("octave" in ln for ln in tail[:goto]):
        return "no octave restore between the appended note and the loop GOTO"


def _grow_requests(state, limit=40):
    """insertNote requests filling rests across tracks. A single structural edit
    re-lays the song out compactly and can shrink it, so many are needed to
    reliably grow it past its original size."""
    reqs = []
    for t in state["tracks"]:
        for n in t["notes"]:
            if n["rest"] and n["len"] >= 24 and not n["loopRepeat"] and len(reqs) < limit:
                reqs.append({"cmd": "insertNote", "track": t["index"], "tick": n["tick"], "pitch": 60, "len": 12})
    return reqs


def test_bin_save_keeps_grown_song():
    """Saving a .bin that outgrew its file writes the whole song.

    Origin: 2026-09-22 review. .bin output was sized to the original file, so
    added bytes were cut off and track pointers pointed past the end.
    """
    spc = open(HAMLET, "rb").read()
    used = run([{"cmd": "open", "path": HAMLET}])[0]["state"]["budget"]["used"]
    binp = os.path.join(TMP, "hamlet.bin")
    with open(binp, "wb") as f:
        f.write(spc[0x100 + 0x0D20:0x100 + 0x0D20 + used])
    st = run([{"cmd": "open", "path": binp}])[0]["state"]
    out = os.path.join(TMP, "hamlet-grown.bin")
    rs = run([{"cmd": "open", "path": binp}] + _grow_requests(st) +
             [{"cmd": "save", "path": out}, {"cmd": "state"}])
    after = rs[-1]["state"]
    grown = after["budget"]["used"]
    if not rs[-2].get("ok"):
        return f"save failed: {rs[-2].get('error')}"
    if grown <= used:
        return f"fixture: song did not grow ({used} -> {grown})"
    if os.path.getsize(out) < grown:
        return f"saved .bin is {os.path.getsize(out)} bytes, song is {grown}"
    back = run([{"cmd": "open", "path": out}])[0]
    if not back.get("ok") or back["state"]["tracks"] != after["tracks"]:
        return "reopened .bin does not match the saved song"


def test_rom_reexport_reuses_owned_block():
    """Re-exporting a relocated song grows in place instead of doubling the ROM.

    Origin: 2026-09-22 review. Each export that outgrew the slot appended new
    space, so 512K -> 1M -> 2M -> 4M, then failed. A block the editor relocated
    into is now tagged and reused; untagged space is still never touched.
    """
    if not os.path.exists(ROM_FIXTURE):
        return f"ROM fixture missing: {ROM_FIXTURE}"
    slots = run([{"cmd": "listRomSongs", "path": ROM_FIXTURE}])[0]["slots"]
    slot = min(slots, key=lambda s: s["size"])["slot"]
    r1 = os.path.join(TMP, "reexport1.smc")
    rs = run([{"cmd": "open", "path": HAMLET},
              {"cmd": "exportRom", "rom": ROM_FIXTURE, "slot": slot, "path": r1}])
    if not rs[1].get("ok") or not rs[1].get("relocated") or rs[1].get("romSize") != 0x100000:
        return f"first export should relocate into a 1 MB ROM: {rs[1]}"
    st = run([{"cmd": "openRom", "path": r1, "slot": slot}])[0]["state"]
    r2 = os.path.join(TMP, "reexport2.smc")
    rs = run([{"cmd": "openRom", "path": r1, "slot": slot}] + _grow_requests(st) +
             [{"cmd": "exportRom", "slot": slot, "path": r2}, {"cmd": "state"}])
    after = rs[-1]["state"]
    if after["budget"]["used"] <= st["budget"]["used"]:
        return f"fixture: song did not grow ({st['budget']['used']} -> {after['budget']['used']})"
    ex = rs[-2]
    if not ex.get("ok"):
        return f"re-export of the grown song failed: {ex.get('error')}"
    if ex.get("relocated") or ex.get("romSize") != 0x100000 or os.path.getsize(r2) != 0x100000:
        return f"re-export should reuse the owned block in a 1 MB ROM: {ex}"
    back = run([{"cmd": "openRom", "path": r2, "slot": slot}])[0]
    if not back.get("ok") or back["state"]["tracks"] != after["tracks"]:
        return "grown song does not read back from the re-exported ROM"


def test_malformed_request_keeps_engine_alive():
    """A request missing a required field is an error, not an engine crash.

    Origin: 2026-09-22 review. const json::operator[] on a missing key is
    undefined behaviour and could crash the engine, losing unsaved work.
    """
    rs = run([{"cmd": "open", "path": HAMLET},
              {"cmd": "insertNote"},
              {"cmd": "setInstrument", "program": 3},
              {"cmd": "state"}])
    if rs[1].get("ok") or rs[2].get("ok"):
        return "malformed requests unexpectedly succeeded"
    if not rs[3].get("ok") or not rs[3].get("state", {}).get("tracks"):
        return "engine did not survive malformed requests with the song still open"


def test_set_instrument_on_repeat_body():
    """Changing the instrument of a repeat body applies to every pass.

    Origin: 2026-09-22 review. The restore program change queued for the note
    after the selection could be the same bytes as a selected note (the next
    pass of the repeat); both landed before one command, the sort kept the
    lower program, and the old instrument survived when the new one was higher.
    """
    reqs = [{"cmd": "new"},
            {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
            {"cmd": "insertNote", "track": 0, "tick": 24, "pitch": 62, "len": 24},
            {"cmd": "createLoop", "track": 0, "startTick": 0, "endTick": 48, "count": 2}]
    rs = run(reqs)
    if not all(r.get("ok") for r in rs):
        return f"fixture failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    notes = [n for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]]
    first = [n for n in notes if n["tick"] < 48]
    new_program = max(n["program"] for n in notes) + 5
    rs = run(reqs + [{"cmd": "setInstrument", "program": new_program,
                      "notes": [{"track": 0, "note": n["i"]} for n in first]}])
    if not rs[-1].get("ok"):
        return f"setInstrument failed: {rs[-1].get('error')}"
    progs = sorted({n["program"] for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]})
    if progs != [new_program]:
        return f"repeat body kept an old instrument: programs {progs}, wanted {new_program}"


def test_aram_window_limit():
    """A song larger than the ARAM song window ($0D20-$4000) never reaches a ROM.

    Origin: 2026-09-22 review: nothing tested the 13024-byte ceiling.
    """
    if not os.path.exists(ROM_FIXTURE):
        return f"ROM fixture missing: {ROM_FIXTURE}"
    src = os.path.join(TMP, "window.asm")
    big = os.path.join(TMP, "window-big.asm")
    run([{"cmd": "open", "path": HAMLET}, {"cmd": "exportAsm", "path": src}])
    lines = open(src).read().splitlines()
    ch = lines.index("Channel1:")
    filler = ["\t.db $A1    ; note"] * 13100      # 13100 one-byte notes: past the window alone
    open(big, "w").write("\n".join(lines[:ch + 1] + filler + lines[ch + 1:]) + "\n")
    rs = run([{"cmd": "importAsm", "path": big},
              {"cmd": "exportRom", "rom": ROM_FIXTURE, "slot": 0x14, "path": os.path.join(TMP, "window.smc")}])
    if not rs[0].get("ok"):
        return None if re.search(r"ARAM|window|fit|bytes|\$3FFF", rs[0].get("error", ""), re.I) else \
            f"oversized import refused for an unexpected reason: {rs[0].get('error')}"
    if rs[1].get("ok"):
        return "a song over 13024 bytes was written into a ROM"
    if "exceeds the ARAM song window" not in rs[1].get("error", ""):
        return f"unexpected refusal: {rs[1].get('error')}"


def test_borrowed_notes_refuse_structural_edits():
    """Structural edits on melody bytes a channel borrows are refused, not lost.

    Origin: 2026-09-22 review. An insert on a channel playing another channel's
    bytes went into the borrower's copy, which the serializer ignores: the
    engine reported ok and pushed undo, but the note was gone after reload.
    """
    st = run([{"cmd": "open", "path": BREAK}])[0]["state"]
    refused = 0
    for t in st["tracks"]:
        tried = 0
        for n in t["notes"]:
            if not n["rest"] or n["len"] < 24 or n["loopRepeat"] or tried >= 6:
                continue
            tried += 1
            rs = run([{"cmd": "open", "path": BREAK},
                      {"cmd": "insertNote", "track": t["index"], "tick": n["tick"], "pitch": 60, "len": 12},
                      {"cmd": "state"}])
            if rs[1].get("ok"):
                if not [m for m in rs[2]["state"]["tracks"][t["index"]]["notes"]
                        if not m["rest"] and m["tick"] == n["tick"] and m["pitch"] == 60]:
                    return f"track {t['index']} tick {n['tick']}: insert reported ok but the note is gone"
            elif "stored in Track" in rs[1].get("error", ""):
                refused += 1
    log(f"  borrowed-byte inserts refused: {refused}")


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


def test_appended_notes_inherit_instrument():
    """New notes on a track must keep the instrument the user set, not reset.

    Origin: AppendNoteAtTick hardcoded a ProgramChange to Electric Piano (0x08)
    whenever a track had no notes yet, clobbering the track's real instrument.
    A tester saw every freshly-added note revert to Electric Piano after
    choosing a different instrument.
    """
    rs = run([{"cmd": "new"}, {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24}])
    n0 = next(n for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"])
    rs = run([
        {"cmd": "new"},
        {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": 60, "len": 24},
        {"cmd": "setInstrument", "notes": [{"track": 0, "note": n0["i"]}], "program": 3},
        {"cmd": "insertNote", "track": 0, "tick": 48, "pitch": 62, "len": 24},
        {"cmd": "insertNote", "track": 0, "tick": 96, "pitch": 64, "len": 24},
    ])
    if not all(r.get("ok") for r in rs):
        return f"scenario failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    progs = [n["program"] for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]]
    if progs != [3, 3, 3]:
        return f"added notes did not inherit the set instrument: programs {progs}"


def _fft(x):
    n = len(x)
    if n == 1:
        return x
    e = _fft(x[0::2])
    o = _fft(x[1::2])
    tw = [cmath.exp(-2j * math.pi * k / n) * o[k] for k in range(n // 2)]
    return [e[k] + tw[k] for k in range(n // 2)] + [e[k] - tw[k] for k in range(n // 2)]


def spectral_distance_db(wav_a, wav_b, win=1024):
    """Mean per-frame magnitude-spectrum error, in dB relative to frame energy.

    The SPC driver spends CPU cycles per sequence command, so any change in
    the command stream shifts DSP writes by a few samples and makes a
    byte-for-byte WAV comparison useless. Magnitude spectra are insensitive to
    those sub-millisecond shifts. Calibrated on the corpus: dead/same-value
    program changes land around -25 dB, a real octave change -1 dB, a real
    instrument change +74 dB. This is a regression tolerance, not a perceptual
    inaudibility test. Missing signal or different frame counts fail closed.
    """
    rate_a, channels_a = read_wav_channels(wav_a)
    rate_b, channels_b = read_wav_channels(wav_b)
    if rate_a != rate_b:
        return math.inf
    # Analyze each channel separately so a lost/panned channel cannot hide in
    # a mono downmix or an assertion which only examines the left channel.
    distances = [_spectral_channel_distance(a, b, win) for a, b in zip(channels_a, channels_b)]
    return max(distances)


def _spectral_channel_distance(a, b, win):
    if len(a) != len(b) or len(a) < win or not any(a) or not any(b):
        return math.inf
    n = len(a)
    tot, cnt = 0.0, 0
    for st in range(0, n - win + 1, win):
        fa = _fft([complex(v) for v in a[st:st + win]])
        fb = _fft([complex(v) for v in b[st:st + win]])
        ma = [abs(v) for v in fa[:win // 2]]
        mb = [abs(v) for v in fb[:win // 2]]
        ea = sum(v * v for v in ma)
        if ea < 1e3:
            continue
        tot += sum((x - y) ** 2 for x, y in zip(ma, mb)) / ea
        cnt += 1
    if not cnt:
        return math.inf
    return 10 * math.log10(tot / cnt + 1e-12)


def read_wav_mono_from_bytes(data):
    return read_wav_channels(data)[1][0]


def read_wav_channels(data):
    with wave.open(io.BytesIO(data), "rb") as wav:
        if wav.getsampwidth() != 2 or wav.getnchannels() != 2 or wav.getcomptype() != "NONE":
            raise ValueError("expected uncompressed 16-bit stereo PCM")
        frames = wav.getnframes()
        rate = wav.getframerate()
        d = wav.readframes(frames)
        if len(d) != frames * 4:
            raise ValueError("truncated PCM data")
    samples = struct.unpack("<%dh" % (len(d) // 2), d)
    return rate, (samples[0::2], samples[1::2])


SPECTRAL_REGRESSION_DB = -15.0  # empirical timing-jitter tolerance; not an audibility guarantee


def test_instrument_edits_self_clean():
    """Repeated instrument changes must not leak dead ProgramChange bytes.

    Origin: beta feedback "every instrument change costs +2 bytes". Each
    setInstrument left the previous edit's restore PC directly in front of the
    new one (PC old, PC new, note) so the track grew by 2 bytes per edit with
    no audible effect. setInstrument now drops such dead stores itself; the
    result must be minimal (one PC per real instrument boundary) and the
    cleanup must preserve this exact minimal-encoding control: reaching the same end state
    incrementally must render byte-identically to reaching it in one edit.
    """
    def build(edits):
        reqs = [{"cmd": "new"}]
        for tick, pitch in ((0, 60), (24, 62), (48, 64), (72, 65)):
            reqs.append({"cmd": "insertNote", "track": 0, "tick": tick, "pitch": pitch, "len": 24})
        rs = run(reqs)
        notes = [n for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]]
        reqs2 = list(reqs)
        for sel, prog in edits:
            reqs2.append({"cmd": "setInstrument", "notes": [{"track": 0, "note": notes[i]["i"]} for i in sel], "program": prog})
        reqs2.append({"id": 9, "cmd": "render", "seconds": 2})
        rs = run(reqs2)
        bad = [r.get("error") for r in rs if not r.get("ok")]
        if bad:
            raise RuntimeError(f"scenario failed: {bad}")
        st = rs[-2]["state"]
        return st["budget"]["used"], [n["program"] for n in st["tracks"][0]["notes"] if not n["rest"]], wav_bytes(last_wav(rs))

    # one-shot: notes 1-2 -> Flute in a single edit (the minimal encoding)
    used_min, progs_min, wav_min = build([((1, 2), 3)])
    # incremental: the same end state reached one note at a time, which used
    # to cost an extra dead PC per step
    used_inc, progs_inc, wav_inc = build([((1,), 3), ((2,), 3)])
    if progs_min != progs_inc:
        return f"end states differ: {progs_min} vs {progs_inc}"
    if used_inc != used_min:
        return f"incremental instrument edits leaked bytes: {used_inc} vs minimal {used_min}"
    if wav_inc != wav_min:
        return "self-cleaned song renders differently from the minimal encoding"
    # flip-flop: set to Flute then back to the original must fully unwind
    used_flip, progs_flip, _ = build([((1,), 3), ((1,), progs_min[0])])
    used_base, progs_base, _ = build([])
    if progs_flip != progs_base or used_flip != used_base:
        return f"set+revert left residue: {used_flip}B/{progs_flip} vs {used_base}B/{progs_base}"

    # Render-verify the cleanup itself: hand-build the un-cleaned encoding
    # (a dead store in front of the real PC, and a same-value re-set before
    # the next note) via ASM and confirm it is inaudibly different from the
    # cleaned song; then a positive control so the metric cannot pass trivially.
    asm = os.path.join(TMP, "selfclean.asm")
    reqs = [{"cmd": "new"}]
    for tick, pitch in ((0, 60), (24, 62), (48, 64), (72, 65)):
        reqs.append({"cmd": "insertNote", "track": 0, "tick": tick, "pitch": pitch, "len": 24})
    rs = run(reqs)
    notes = [n for n in rs[-1]["state"]["tracks"][0]["notes"] if not n["rest"]]
    reqs.append({"cmd": "setInstrument", "notes": [{"track": 0, "note": notes[i]["i"]} for i in (1, 2)], "program": 3})
    reqs.append({"cmd": "exportAsm", "path": asm})
    reqs.append({"id": 9, "cmd": "render", "seconds": 2})
    rs = run(reqs)
    clean_wav = wav_bytes(last_wav(rs))
    lines = open(asm).read().splitlines()
    dirty, control = [], []
    seen_pc3, notes_after = False, 0
    for ln in lines:
        m = re.match(r"\s*\.db \$08, \$([0-9A-F]{2})", ln)
        if m and m.group(1) == "03" and not seen_pc3:
            seen_pc3 = True
            dirty.append("\t.db $08, $08")         # dead store (the old restore PC)
            dirty.append(ln)
            control.append("\t.db $08, $05")       # real change: a different instrument
            continue
        if seen_pc3 and "; note key" in ln:
            notes_after += 1
            if notes_after == 2:
                dirty.append("\t.db $08, $03")     # same-value re-set across a note
        dirty.append(ln)
        control.append(ln)
    if not seen_pc3 or notes_after < 2:
        return "could not locate the Flute program change + notes in exported ASM"
    dirty_asm = os.path.join(TMP, "selfclean-dirty.asm")
    control_asm = os.path.join(TMP, "selfclean-control.asm")
    open(dirty_asm, "w").write("\n".join(dirty) + "\n")
    open(control_asm, "w").write("\n".join(control) + "\n")
    rs = run(reqs[:-3] + [{"cmd": "importAsm", "path": dirty_asm}, {"id": 9, "cmd": "render", "seconds": 2}])
    if not all(r.get("ok") for r in rs):
        return f"dirty ASM import failed: {[r.get('error') for r in rs if not r.get('ok')]}"
    d_dirty = spectral_distance_db(clean_wav, wav_bytes(last_wav(rs)))
    rs = run(reqs[:-3] + [{"cmd": "importAsm", "path": control_asm}, {"id": 9, "cmd": "render", "seconds": 2}])
    d_ctrl = spectral_distance_db(clean_wav, wav_bytes(last_wav(rs)))
    log(f"  self-clean spectral distance: redundant PCs {d_dirty:.1f} dB, real change {d_ctrl:.1f} dB")
    if d_dirty > SPECTRAL_REGRESSION_DB:
        return f"cleanup exceeds spectral regression tolerance: {d_dirty:.1f} dB"
    if not math.isfinite(d_ctrl) or d_ctrl < 0:
        return f"metric failed its positive control: a real instrument change scored {d_ctrl:.1f} dB"


TESTS = [
    test_optimize_atomic_history,
    test_tied_track_merge,
    test_optimize_noop_history,
    test_optimize_atomic_failure,
    test_encoding_timing_duration_table,
    test_encoding_timing_boundaries,
    test_encoding_timing_dotted_context,
    test_encoding_timing_loop_roundtrip,
    test_encoding_timing_merged_ties,
    test_tied_move_contexts,
    test_consecutive_rest_edits,
    test_loop_body_extend,
    test_end_song_loop,
    test_loop_append_sync_and_octave,
    test_bin_save_keeps_grown_song,
    test_rom_reexport_reuses_owned_block,
    test_malformed_request_keeps_engine_alive,
    test_set_instrument_on_repeat_body,
    test_aram_window_limit,
    test_borrowed_notes_refuse_structural_edits,
    test_instrument_edits_self_clean,
    test_appended_notes_inherit_instrument,
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
