#!/usr/bin/env python3
"""ASM format, instrument mapping, and transactional import regression tests.

python tests/asm_regression.py --parser-only
  Compiles ONLY the actual data parser in a temporary harness, not the engine.
python tests/asm_regression.py --engine
  Uses GTB_TEST_ENGINE (or build/win-x64/bin/gtb-engine.exe).
Logs: tests/asm-parser-regression.log and tests/asm-engine-regression.log.
ASM input is always data, never executed.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / "tests/asm-regression.log"
RESULTS = []


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    RESULTS.append("PASS " + message)


def fixture(body="db $08, !instrument_16, $61, $08, $08, $62, $08, !instrument_16, $63, $17"):
    return ("; synthetic extraction-style aliases\n"
            ".base $0D20\n!instrument_16 = #$01\n!instrument_16 = #$01\n"
            "db $00\ndw melody, end, end, end, end, end, end, end\n"
            "melody:\n" + body + "\nend: db $17\n")


def wrapped_fixture():
    return ("lorom\nfunction BigEndian(n) = (((n&$ff00)>>8)|((n&$00ff)<<8))\n"
            "org $84E983\n!ARAMAddr = $0D20\nSongStart:\ndw SongStart-EndOfSong\ndw !ARAMAddr\n"
            "Channels:\n!ARAMC = !ARAMAddr-SongStart\n" +
            "\n".join(f"dw BigEndian(Channel0{i}+!ARAMC)" for i in range(8)) +
            "\n!instrument_16 = #$01\nChannel00:\ndb 8,!instrument_16,$61,$17\n" +
            "\n".join(f"Channel0{i}:\ndb $17" for i in range(1, 8)) + "\nEndOfSong:\n")


def corpus_files():
    music = Path(os.environ.get("GTB_TEST_CORPUS", ROOT.parent.parent / "Music Sources"))
    files = sorted((music / "ASM").glob("*.asm")) + [music / "Miscellaneous/ToTheSouth.asm"]
    if len(files) < 35 or not all(file.is_file() for file in files):
        raise RuntimeError("ASM qualification requires the complete local extraction corpus (GTB_TEST_CORPUS)")
    return files


def patched_fixture():
    return wrapped_fixture().replace(
        "lorom\n", "lorom\norg $848000+($14*3)\ndl melodyblob-$8000\n"
    ).replace(
        "org $84E983\n!ARAMAddr = $0D20\nSongStart:\ndw SongStart-EndOfSong\ndw !ARAMAddr",
        "org $9CC000\nmelodyblob:\n!ARAMAddr = $0D20\ndw EndOfSong-SongStart\ndw !ARAMAddr\nSongStart:"
    )


def parser_tests():
    source = (ROOT / "src/engine/EngineMain.cpp").read_text(encoding="utf-8")
    parser = source[source.index("std::string asmTrim("):source.index("// Directory containing this engine")]
    harness = '''#include <algorithm>
#include <cstdint>
#include <cctype>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"
''' + parser + '''
int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto request = nlohmann::json::parse(line);
    uint32_t base = 123;
    std::vector<uint8_t> bytes{99};
    std::string error;
    const bool ok = assembleAsm(request.get<std::string>(), base, bytes, error);
    std::cout << nlohmann::json({{"ok",ok},{"base",base},{"bytes",bytes},{"error",error}}).dump() << "\\n";
  }
}
'''
    cases = [
        ("song patch envelope", patched_fixture(), True),
        ("song patch directives need no dots", patched_fixture().replace("db ", "DB "), True),
        ("song patch wrong slot", patched_fixture().replace("($14*3)", "($30*3)"), False),
        ("song patch wrong table", patched_fixture().replace("$848000+", "$858000+"), False),
        ("song patch label mismatch", patched_fixture().replace("melodyblob:", "other:"), False),
        ("song patch arbitrary expression", patched_fixture().replace("-$8000", "+$8000"), False),
        ("song patch includes rejected", patched_fixture() + '\nincsrc "file.asm"', False),
        ("song patch missing header", patched_fixture().replace("dw EndOfSong-SongStart\n", ""), False),
        ("song patch missing channel", patched_fixture().replace("dw BigEndian(Channel07+!ARAMC)\n", ""), False),
        ("extraction aliases and repeated declarations", fixture(), True),
        ("forward numeric alias", "db !later\n!later = #$2a", True),
        ("decimal hex immediate literals", "db 1, #2, $03, #$04, 0x05", True),
        ("case-insensitive directives and inline labels", ".base $100\nData: DB $01\nDW Data", True),
        ("undefined alias", "db !missing", False),
        ("conflicting alias", "!x = #$01\n!x = #$02\ndb !x", False),
        ("alias expression", "!x = 1+2\ndb !x", False),
        ("alias chain unsupported", "!x = !y\n!y=2\ndb !x", False),
        ("unsupported instruction", "mov a, #$01", False),
        ("include is never executed", 'incsrc "missing.asm"', False),
        ("unknown macro", "%instrument(1)", False),
        ("byte overflow", "db $100", False),
        ("negative byte", "db -1", False),
        ("trailing garbage", "db $01garbage", False),
        ("duplicate label", "x: db 1\nx: db 2", False),
        ("alias label collision", "x=1\nx: db 2", False),
        ("undefined word", "dw Missing", False),
        ("word overflow", "dw $10000", False),
        ("empty operand", "db 1,,2", False),
        ("trailing comma", "db 1,", False),
        ("empty directive", "db", False),
        ("late base", "db 1\n.base $2000", False),
        ("repeated base", ".base $2000\n.base $2000\ndb 1", False),
        ("ARAM overflow", ".base $ffff\ndw $1234", False),
        ("empty input", "; no data", False),
        ("case-sensitive aliases", "!X=1\ndb !x", False),
        ("unknown number prefix", "db %0101", False),
        ("canonical extraction wrapper", wrapped_fixture(), True),
        ("altered wrapper function", wrapped_fixture().replace("n&$ff00", "n&$f000"), False),
        ("wrapper injected expression", wrapped_fixture().replace("Channel00+!ARAMC", "Channel00+!ARAMC+1"), False),
        ("truncated wrapper", "lorom\n", False),
        ("positive-length extraction wrapper", patched_fixture().replace(
            "org $848000+($14*3)\ndl melodyblob-$8000\n", "").replace("melodyblob:\n", ""), True),
    ]
    synthetic_count = len(cases)
    for path in corpus_files():
        check(path.is_file(), "local corpus exists: " + path.name)
        # Old 0x extracts contain oversized/non-song data and are expected to fail
        # either byte bounds here or song-region validation in the engine.
        cases.append(("local corpus " + path.name, path.read_text(encoding="utf-8-sig"),
                      None if path.name.startswith("0x") else True))
    with tempfile.TemporaryDirectory(prefix="gtb-asm-parser-") as tmp:
        tmp = Path(tmp)
        (tmp / "parser.cpp").write_text(harness, encoding="utf-8")
        vcvars = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
        command = f'call "{vcvars}" >nul && cl /nologo /EHsc /std:c++20 /I"{ROOT / "lib"}" parser.cpp /Fe:parser.exe'
        # cmd.exe needs its own command-line quoting, not list2cmdline's C-runtime escaping.
        built = subprocess.run('cmd /d /c ' + command, cwd=tmp, capture_output=True, text=True, timeout=120)
        RESULTS.append(built.stdout + built.stderr)
        check(built.returncode == 0, "actual ASM parser compiles in isolation")
        run = subprocess.run([str(tmp / "parser.exe")], input="\n".join(json.dumps(c[1]) for c in cases),
                             capture_output=True, text=True, timeout=20)
        check(run.returncode == 0, "parser harness exits successfully")
        responses = [json.loads(line) for line in run.stdout.splitlines()]
        check(len(responses) == len(cases), "one response per parser fixture")
        for (name, _, expected), response in zip(cases, responses):
            if expected is not None:
                check(response["ok"] == expected, name + ": " + response["error"])
            else:
                RESULTS.append(f"CORPUS {name}: ok={response['ok']} base={response['base']} "
                               f"bytes={len(response['bytes'])} error={response['error']}")
            if not response["ok"]:
                check(response["base"] == 123 and response["bytes"] == [99], name + " preserves output on error")
                check("ASM line " in response["error"], name + " has line diagnostic")
        original = responses[9:]
        check(original[0]["bytes"][18] == 1 and original[0]["bytes"][24] == 1,
              "repeated alias uses emit program 1, not suffix 16 or zero")
        check(original[1]["bytes"] == [42], "forward alias resolves")
        check(original[2]["bytes"] == [1, 2, 3, 4, 5], "literal values preserved")
        check(original[3]["bytes"] == [1, 1, 0], "word output is big-endian")
        check(original[27]["bytes"][:2] == [0x0d, 0x30],
              "extraction header strips upload words without adding four-byte address bias")
        check(responses[0]["bytes"] == original[27]["bytes"], "patch metadata does not change sequence bytes")
        check(synthetic_count == 41, "41 synthetic dialect cases exercised")


def engine_tests():
    engine = Path(os.environ.get("GTB_TEST_ENGINE", ROOT / "build/win-x64/bin/gtb-engine.exe"))
    bank = Path(os.environ.get("GTB_SOUNDBANK", ROOT.parent.parent / "Music Sources/SPC/08 Hamlet.spc"))
    check(engine.is_file() and bank.is_file(), "rebuilt engine and target bank exist")

    def run(requests, bank_path=bank):
        env = dict(os.environ, GTB_SOUNDBANK=str(bank_path))
        result = subprocess.run([str(engine)], input="\n".join(json.dumps(r) for r in requests) + "\n",
                                capture_output=True, text=True, cwd=ROOT, env=env, timeout=120)
        check(result.returncode == 0, "engine process exits successfully")
        responses = [json.loads(line) for line in result.stdout.splitlines()]
        check(len(responses) == len(requests), "one response per engine request")
        return responses

    def rejection(name, request):
        r = run([{"cmd": "open", "path": str(bank)}, {"cmd": "exportAsm"}, request, {"cmd": "exportAsm"}])
        check(r[0]["ok"] and r[1]["ok"], name + " baseline opens")
        check(not r[2]["ok"], name + " rejected")
        check(r[3]["ok"] and r[1]["asm"] == r[3]["asm"], name + " preserves song bytes")
        check(r[1]["state"] == r[3]["state"], name + " preserves session/undo state")

    group_source = fixture("db $08,1,$61,$60,$62,$60,$60,$60,$17")
    group_state = run([{"cmd": "importAsm", "asm": group_source}])[0]["state"]
    group_track = next(t for t in group_state["tracks"] if any(not n["rest"] for n in t["notes"]))
    items = [{"track": group_track["index"], "tick": n["tick"], "pitch": n["pitch"]}
             for n in group_track["notes"] if not n["rest"]]
    for operation in (
        {"cmd": "moveNotes", "items": items, "dTick": 12, "dPitch": 1},
        {"cmd": "moveNotes", "items": items, "dTick": 0, "dPitch": 1},
    ):
        replies = run([{"cmd": "importAsm", "asm": group_source}, {"cmd": "exportAsm"}, operation,
                       {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                       {"cmd": "redo"}, {"cmd": "exportAsm"}])
        check(all(r["ok"] for r in replies), "group move succeeds as one transaction")
        check(replies[1]["asm"] != replies[3]["asm"] and replies[1]["asm"] == replies[5]["asm"] and
              replies[3]["asm"] == replies[7]["asm"], "one undo/redo restores complete group")
    bad_items = [*items, {"track": group_track["index"], "tick": 9999, "pitch": 60}]
    failed_group = run([{"cmd": "importAsm", "asm": group_source}, {"cmd": "exportAsm"},
                        {"cmd": "moveNotes", "items": bad_items, "dTick": 0, "dPitch": 1},
                        {"cmd": "exportAsm"}])
    check(not failed_group[2]["ok"] and failed_group[1]["asm"] == failed_group[3]["asm"] and
          failed_group[1]["state"] == failed_group[3]["state"], "late group failure restores bytes and history")

    loop_setup = [{"cmd": "importAsm", "asm": group_source},
                  {"cmd": "createLoop", "track": group_track["index"], "startTick": 0,
                   "endTick": 24, "slot": 0, "count": 2}]
    replacement = {"cmd": "replaceLoop", "track": group_track["index"], "loop": 0,
                   "startTick": 0, "endTick": 48, "slot": 0, "count": 3}
    replaced = run([*loop_setup, {"cmd": "exportAsm"}, replacement, {"cmd": "exportAsm"},
                    {"cmd": "undo"}, {"cmd": "exportAsm"}, {"cmd": "redo"}, {"cmd": "exportAsm"}])
    check(all(r["ok"] for r in replaced), "loop replacement succeeds")
    check(replaced[2]["asm"] != replaced[4]["asm"] and replaced[2]["asm"] == replaced[6]["asm"] and
          replaced[4]["asm"] == replaced[8]["asm"], "loop replacement has one undo/redo")
    failed_loop = run([*loop_setup, {"cmd": "exportAsm"}, dict(replacement, endTick=0), {"cmd": "exportAsm"}])
    check(not failed_loop[3]["ok"] and failed_loop[2]["asm"] == failed_loop[4]["asm"] and
          failed_loop[2]["state"] == failed_loop[4]["state"], "invalid replacement restores original loop and history")

    # Independent driver evidence: the V1 dispatch table points $1E/$1F
    # at RET, after the dispatcher has already fetched one operand.
    bank_bytes = bank.read_bytes()
    shared_tail = (".base $0D20\ndw a,b,b,b,b,b,b,b\n"
                   "a: db $08,1,$61,$16\ndw shared\n"
                   "b: db $17\nshared: db $08,1,$62,$17\n")
    loaded = run([{"cmd": "importAsm", "asm": shared_tail}])[0]
    selected_track = next(t for t in loaded["state"]["tracks"] if t["notes"])
    selected = selected_track["notes"][0]["i"]
    shared = run([{"cmd": "importAsm", "asm": shared_tail}, {"cmd": "exportAsm"},
                  {"cmd": "setInstrument", "notes": [{"track": selected_track["index"], "note": selected}],
                   "program": 2},
                  {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                  {"cmd": "redo"}, {"cmd": "exportAsm"}])
    check(all(item["ok"] for item in shared), "structural edit retains a foreign-only shared tail: " +
          str([(i, item.get("error")) for i, item in enumerate(shared) if not item["ok"]]))
    check(shared[1]["asm"] == shared[5]["asm"] and shared[3]["asm"] == shared[7]["asm"],
          "shared-tail structural edit undo/redo is exact")
    # Pan operands are data even when their values resemble control-flow
    # opcodes. Treating pan as one byte can relocate an instrument/duration
    # pair as though it were a loop address.
    for pan in (0, 0x0e, 0x0f, 0x10, 0x12, 0x16, 0x1e, 0x1f):
        source = fixture(f"db $18,${pan:02x},$08,1,$06,$e0,$61,$17")
        r = run([{"cmd": "inspectAsm", "asm": source}, {"cmd": "importAsm", "asm": source},
                 {"cmd": "exportAsm"}])
        check(all(item["ok"] for item in r), f"pan operand {pan:02x} is not a command")
        check(r[0]["asmReport"]["sourcePrograms"] == [1], "pan preserves following program selection")
        check(sum(len(t["notes"]) for t in r[1]["state"]["tracks"]) == 1,
              "pan preserves following duration and note boundaries")
    for opcode in (0x1e, 0x1f):
        target = struct.unpack_from("<H", bank_bytes, 0x100 + 0x842 + opcode * 2)[0]
        check(target == 0x841 and bank_bytes[0x100 + target] == 0x6f,
              "bank confirms reserved NOP handler")
    noop = fixture("db $08,1,$1e,$ff,$1f,$08,$61,$17")
    nooped = run([{"cmd": "importAsm", "asm": noop}, {"cmd": "exportAsm"}])
    check(all(item["ok"] for item in nooped), "two-byte NOPs import without consuming subsequent notes")
    check(sum(len(t["notes"]) for t in nooped[0]["state"]["tracks"]) == 1,
          "NOP operands are not mistaken for notes or instruments")
    inactive = fixture("db $08,1,$12,0,$ff,$ff,$61,$17")
    r = run([{"cmd": "inspectAsm", "asm": inactive}, {"cmd": "importAsm", "asm": inactive},
             {"cmd": "exportAsm"}])
    check(all(item["ok"] for item in r), "untaken external repeat-break is preserved")
    check(r[0]["asmReport"]["inactiveExternalJumps"] == 1, "review reports preserved inactive branch")
    track = next(t for t in r[1]["state"]["tracks"] if t["notes"])
    edits = run([{"cmd": "importAsm", "asm": inactive}, {"cmd": "exportAsm"},
                 {"cmd": "setInstrument", "notes": [{"track": track["index"], "note": track["notes"][0]["i"]}],
                  "program": 2}, {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                 {"cmd": "redo"}, {"cmd": "exportAsm"}])
    check(all(item["ok"] for item in edits), "structural edit proves external conditional remains untaken")
    check(edits[1]["asm"] != edits[3]["asm"] and edits[1]["asm"] == edits[5]["asm"] and
          edits[3]["asm"] == edits[7]["asm"], "inactive conditional edit undo/redo is exact")
    conditional = fixture("db $08,1,$61,$12,0,$ff,$ff,$62,$17")
    activated = run([{"cmd": "importAsm", "asm": conditional}, {"cmd": "exportAsm"},
                     {"cmd": "createLoop", "track": track["index"], "startTick": 0, "endTick": 24,
                      "slot": 0, "count": 2}, {"cmd": "exportAsm"}])
    check(activated[0]["ok"] and not activated[2]["ok"], "edit activating unresolved conditional is rejected")
    check(activated[1]["asm"] == activated[3]["asm"] and activated[1]["state"] == activated[3]["state"],
          "unsafe conditional edit preserves sequence and history")
    rejection("taken external repeat-break", {"cmd": "importAsm", "asm": fixture(
        "db $08,1\nrepeat: db $12,0,$ff,$ff,$61,$0e,1\ndw repeat\ndb $17")})
    rejection("external break activated after repeated goto", {"cmd": "importAsm", "asm": fixture(
        "db $08,1\nloop: db $12,0,$ff,$ff,$61,$0e,3\ndw trampoline\ndb $17\n"
        "trampoline: db $16\ndw loop")})
    rejection("zero-count repeat follows its target", {"cmd": "importAsm", "asm": fixture(
        "db $08,1,$61,$0e,0,$ff,$ff,$17")})
    cycle = run([{"cmd": "importAsm", "asm": fixture(
        "db $08,1\nloop: db $61,$0e,0\ndw loop\ndb $17")}])
    check(cycle[0]["ok"], "bounded zero-count cycle validates without hanging")

    for name, asm in [
        ("undefined alias", fixture().replace("!instrument_16, $61", "!missing, $61")),
        ("conflicting alias", fixture().replace("!instrument_16 = #$01", "!instrument_16 = #$02", 1)),
        ("all-end silent sequence", fixture("db $17")),
        ("uninitialized instrument", fixture("db $61, $17")),
        ("truncated command", fixture("db $08").replace("\nend: db $17", "\nend:")),
        ("out-of-image pointer", fixture().replace("dw melody,", "dw $4000,")),
        ("out-of-image jump", fixture("db $08, 1, $61, $16\ndw $4000")),
        ("unsupported directive", fixture() + "\nincbin missing.bin"),
    ]:
        rejection(name, {"cmd": "importAsm", "asm": asm})
    for name, mapping in [
        ("fractional target", [{"from": 1, "to": 1.5}]),
        ("out-of-range target", [{"from": 1, "to": 256}]),
        ("negative source", [{"from": -1, "to": 1}]),
        ("conflicting map", [{"from": 1, "to": 8}, {"from": 1, "to": 2}]),
        ("unused alias suffix", [{"from": 16, "to": 8}]),
        ("wrong mapping shape", {"1": 8}),
    ]:
        rejection(name, {"cmd": "importAsm", "asm": fixture(), "programMap": mapping})

    r = run([{"cmd": "open", "path": str(bank)}, {"cmd": "exportAsm"},
             {"cmd": "inspectAsm", "asm": fixture()}, {"cmd": "exportAsm"},
             {"cmd": "importAsm", "asm": fixture(), "programMap": [
                 {"from": 1, "to": 8}, {"from": 8, "to": 2}, {"from": 1, "to": 8}]},
             {"cmd": "exportAsm"}])
    check(all(x["ok"] for x in r), "inspect and simultaneous remap succeed: " + str([x.get("error") for x in r]))
    check(r[1]["asm"] == r[3]["asm"] and r[1]["state"] == r[3]["state"], "inspection preserves current song")
    check(r[2]["asmReport"]["sourcePrograms"] == [1, 8], "review reports resolved source programs")
    mapped = [n["program"] for t in r[4]["state"]["tracks"] for n in t["notes"] if not n["rest"]]
    check(mapped == [8, 2, 8], "all repeated program changes remap exactly once, without cascading")
    invalid = next((p for p in range(256) if p not in r[2]["asmReport"]["targetPrograms"]), None)
    check(invalid is not None, "target bank has an unavailable program to test")
    rejection("unavailable bank target", {"cmd": "importAsm", "asm": fixture(),
                                          "programMap": [{"from": 1, "to": invalid}]})
    bad_source = fixture().replace("!instrument_16 = #$01", f"!instrument_16 = {invalid}")
    rejection("unmapped unavailable source", {"cmd": "importAsm", "asm": bad_source})
    reviewed = run([{"cmd": "inspectAsm", "asm": bad_source}])[0]
    check(reviewed["ok"] and invalid in reviewed["asmReport"]["unmappedPrograms"],
          "review allows unavailable source so user can map it")
    roundtrip = run([{"cmd": "importAsm", "asm": r[5]["asm"]}, {"cmd": "exportAsm"}])
    check(all(x["ok"] for x in roundtrip) and roundtrip[1]["asm"] == r[5]["asm"],
          "mapped export imports losslessly")
    with tempfile.TemporaryDirectory(prefix="gtb-asm-bank-") as tmp:
        silent = fixture("db $05,$03,$33,$06,$B4,$07,$7F,$09,$03,$19,$5F,$08,$17,$e1,$e2,$e3,$17")
        silence_path = Path(tmp) / "intentional-mute.wav"
        muted = run([{"cmd": "inspectAsm", "asm": silent}, {"cmd": "importAsm", "asm": silent},
                     {"cmd": "render", "seconds": 2, "path": str(silence_path)}])
        check(all(item["ok"] for item in muted), "qualified mute program imports without remapping")
        check(muted[0]["asmReport"]["silentPrograms"] == [23], "review labels the qualified mute program")
        with wave.open(str(silence_path), "rb") as wav:
            # Restarting an SPC snapshot has a short initial voice/filter tail.
            # Afterwards the renderer's idle floor is at most one PCM unit.
            wav.setpos(wav.getframerate() // 4)
            pcm = wav.readframes(wav.getnframes())
            samples = struct.unpack("<" + "h" * (len(pcm) // 2), pcm)
            check(max(map(abs, samples)) <= 1, "mute has no sustained signal above the one-unit idle floor")
        bad_mute_bank = bytearray(bank.read_bytes())
        bad_mute_bank[0x100 + 0x505c + 6 * 23] = 255
        bad_mute_path = Path(tmp) / "unqualified-mute.spc"
        bad_mute_path.write_bytes(bad_mute_bank)
        rejected_mute = run([{"cmd": "importAsm", "asm": silent}], bad_mute_path)
        check(not rejected_mute[0]["ok"], "modified mute header is not automatically allowed")
        missing = run([{"cmd": "importAsm", "asm": fixture()}], Path(tmp) / "absent.spc")
        check(not missing[0]["ok"], "missing target bank fails cleanly")
        # Synthetic, initialized melody: verify the supported mapped workflow
        # produces nonzero PCM, without making an acoustic-parity claim.
        audible = fixture("db $05,$03,$33,$06,$B4,$07,$7F,$09,$03,$19,$5F,"
                          "$08,!instrument_16,$61,$62,$63,$17")
        wav_path = Path(tmp) / "mapped-synthetic.wav"
        rendered = run([{"cmd": "importAsm", "asm": audible, "programMap": [{"from": 1, "to": 8}]},
                        {"cmd": "render", "seconds": 2, "path": str(wav_path)}])
        check(all(item["ok"] for item in rendered), "initialized mapped fixture renders")
        with wave.open(str(wav_path), "rb") as wav:
            check(any(wav.readframes(wav.getnframes())), "mapped synthetic melody has nonzero PCM")
    # Run local extraction corpus in-process with a known baseline after every
    # attempt. Keep raw music out of generated artifacts and distribution.
    for path in corpus_files():
        responses = run([{"cmd": "open", "path": str(bank)}, {"cmd": "exportAsm"},
                         {"cmd": "importAsm", "path": str(path)}, {"cmd": "exportAsm"}])
        outcome = responses[2]
        RESULTS.append(f"CORPUS {path.name}: ok={outcome['ok']} error={outcome.get('error', '')}")
        if not outcome["ok"]:
            check(responses[1]["asm"] == responses[3]["asm"] and responses[1]["state"] == responses[3]["state"],
                  "corpus rejection preserves session: " + path.name)
        check(outcome["ok"] == (not path.name.startswith("0x")),
              "corpus import expectation: " + path.name)


if __name__ == "__main__":
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--parser-only", action="store_true")
    args.add_argument("--engine", action="store_true")
    options = args.parse_args()
    LOG = LOG.with_name("asm-engine-regression.log" if options.engine else "asm-parser-regression.log")
    try:
        if options.engine:
            engine_tests()
        else:
            parser_tests()
    except Exception as exc:
        RESULTS.append("FAIL " + str(exc))
        raise
    finally:
        LOG.write_text("\n".join(RESULTS) + "\n", encoding="utf-8")
        print("\n".join(RESULTS))
        print("Log:", LOG)
