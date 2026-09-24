#!/usr/bin/env python3
"""Private-input qualification; no archive members are extracted or executed.

Pass --zip, --bank, --rom (repeatable), and --report in a git-ignored directory.
Reports retain file names/hashes and safe result categories, never music or raw errors.
Rejected source files are recorded, not silently repaired. A rejected import must
leave the previous song unchanged. Accepted imports must survive ASM/session
round trips and produce nonzero PCM. This is not a musical-fidelity oracle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import wave
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def category(reply):
    error = reply.get("error", "")
    for fragment, rule in (("unavailable target-bank", "program-map-required"),
                           ("before a program", "uninitialized-program"),
                           ("jump leaves", "out-of-image-jump"),
                           ("truncated/unsupported", "invalid-command-range"),
                           ("wrapper", "unsupported-wrapper")):
        if fragment in error:
            return rule
    return "accepted" if reply.get("ok") else "other-rejection"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip", type=Path, required=True)
    parser.add_argument("--bank", type=Path, required=True)
    parser.add_argument("--rom", type=Path, action="append", default=[])
    parser.add_argument("--engine", type=Path, default=ROOT / "build/win-x64/bin/gtb-engine.exe")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report_path = args.report.resolve()
    ignored = subprocess.run(["git", "check-ignore", "-q", str(report_path)], cwd=ROOT)
    if ignored.returncode != 0:
        raise RuntimeError("report must be in a git-ignored project directory")
    inputs = [args.zip, args.bank, args.engine, *args.rom]
    hashes = {str(p): digest(p.read_bytes()) for p in inputs}
    report = {"engine_sha256": hashes[str(args.engine)], "archive_sha256": hashes[str(args.zip)],
              "asm": [], "roms": [], "status": "running"}

    def run(requests):
        result = subprocess.run([str(args.engine.resolve())],
                                input="".join(json.dumps(r) + "\n" for r in requests),
                                text=True, capture_output=True, timeout=180, cwd=ROOT,
                                env=dict(os.environ, GTB_SOUNDBANK=str(args.bank.resolve())))
        if result.returncode:
            raise AssertionError("engine process failed")
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        if len(replies) != len(requests):
            raise AssertionError("protocol reply count mismatch")
        return replies

    def require(replies):
        if not all(r.get("ok") for r in replies):
            raise AssertionError("qualification command failed")

    def qualify(open_request, tmp):
        wav1, wav2, session = tmp / "before.wav", tmp / "after.wav", tmp / "song.gtb"
        first = run([open_request, {"cmd": "exportAsm"}])
        require(first)
        asm = first[1]["asm"]
        track, note = next((t, n) for t in first[0]["state"]["tracks"] for n in t["notes"]
                           if not n["rest"] and not n["loopRepeat"])
        edited = run([open_request,
                      {"cmd": "setInstrument", "notes": [{"track": track["index"], "note": note["i"]}],
                       "program": 1 if note["program"] == 0 else 0},
                      {"cmd": "exportAsm"}, {"cmd": "undo"}, {"cmd": "exportAsm"},
                      {"cmd": "redo"}, {"cmd": "exportAsm"}])
        require([edited[0], edited[2]])
        edit_supported = edited[1]["ok"]
        if edit_supported:
            require(edited)
            if edited[2]["asm"] == asm or edited[4]["asm"] != asm or edited[6]["asm"] != edited[2]["asm"]:
                raise AssertionError("instrument edit/undo/redo changed unexpected music")
        else:
            if edited[2]["asm"] != asm or edited[2]["state"] != edited[0]["state"]:
                raise AssertionError("rejected instrument edit corrupted the session")
            # This is a recorded unsupported edit, NOT a successful edit test.
        edit_error = edited[1].get("error", "")
        edit_result = "passed" if edit_supported else (
            "blocked-unresolved-control-flow-preserved-session"
            if "Jump target has no emitted home" in edit_error or "Unresolved destination" in edit_error
            else "blocked-other-edit-rejection-preserved-session")
        results = run([open_request,
                       {"cmd": "render", "seconds": 3, "path": str(wav1)},
                       {"cmd": "saveSession", "path": str(session)},
                       {"cmd": "openSession", "path": str(session)},
                       {"cmd": "exportAsm"},
                       {"cmd": "render", "seconds": 3, "path": str(wav2)}])
        require(results)
        if results[4]["asm"] != asm:
            raise AssertionError("session round trip changed music")
        with wave.open(str(wav1), "rb") as wav:
            pcm = wav.readframes(wav.getnframes())
            startup_bytes = wav.getframerate() // 4 * wav.getnchannels() * wav.getsampwidth()
        with wave.open(str(wav2), "rb") as wav:
            reopened = wav.readframes(wav.getnframes())
        if not any(pcm) or pcm != reopened:
            raise AssertionError("silent output or session audio mismatch")
        sustained = struct.unpack("<" + "h" * ((len(pcm) - startup_bytes) // 2), pcm[startup_bytes:])
        peak = max(map(abs, sustained), default=0)
        if peak <= 32:
            raise AssertionError("no signal above idle floor after startup")
        # ASM reimport uses strict program validation, which may reject ROMs
        # containing instrument references unsupported by the selected bank.
        if open_request["cmd"] == "importAsm":
            again = run([{"cmd": "importAsm", "asm": asm}, {"cmd": "exportAsm"}])
            require(again)
            if again[1]["asm"] != asm:
                raise AssertionError("ASM round trip changed music")
        return {"sequence_sha256": digest(asm.encode()), "pcm_sha256": digest(pcm),
                "notes": sum(len(t["notes"]) for t in first[0]["state"]["tracks"]),
                "session_roundtrip": True, "instrument_edit_undo_redo": edit_supported,
                "edit_result": edit_result,
                "nonzero_pcm": True, "post_startup_peak": peak}

    try:
        with tempfile.TemporaryDirectory(prefix="gtb-pack-check-") as temporary:
            tmp = Path(temporary)
            with zipfile.ZipFile(args.zip) as archive:
                entries = [e for e in archive.infolist() if e.filename.lower().endswith(".asm")]
                if not entries or len(entries) > 500 or sum(e.file_size for e in entries) > 16_000_000:
                    raise RuntimeError("missing or oversized ASM corpus")
                for i, entry in enumerate(entries):
                    raw = archive.read(entry)
                    text = raw.decode("utf-8-sig")
                    request = {"cmd": "importAsm", "asm": text}
                    baseline = run([{"cmd": "open", "path": str(args.bank.resolve())},
                                    {"cmd": "exportAsm"}, request, {"cmd": "exportAsm"}])
                    require([baseline[0], baseline[1], baseline[3]])
                    result = {"file": entry.filename, "sha256": digest(raw), "result": category(baseline[2])}
                    if baseline[2]["ok"]:
                        result.update(qualify(request, tmp))
                    else:
                        if (baseline[1]["asm"] != baseline[3]["asm"] or
                                baseline[1]["state"] != baseline[3]["state"]):
                            raise AssertionError("rejection changed prior session")
                        result["rejection_preserved_session"] = True
                    report["asm"].append(result)
                    print(f"ASM {i + 1}/{len(entries)}: {result['result']} {result.get('edit_result', '')}", flush=True)
            for rom in args.rom:
                listed = run([{"cmd": "listRomSongs", "path": str(rom.resolve())}])
                require(listed)
                slots = listed[0]["slots"]
                if not slots:
                    raise AssertionError("ROM contains no supported song slots")
                record = {"file": rom.name, "sha256": hashes[str(rom)], "slots": []}
                report["roms"].append(record)
                for slot in slots:
                    record["slots"].append({"slot": slot["slot"], "size": slot["size"],
                        **qualify({"cmd": "openRom", "path": str(rom.resolve()), "slot": slot["slot"]}, tmp)})
                    print(f"ROM {len(report['roms'])}/{len(args.rom)} slot {slot['slot']}: passed", flush=True)
        if any(digest(p.read_bytes()) != hashes[str(p)] for p in inputs):
            raise AssertionError("original input changed")
        report["originals_unchanged"] = True
        report["unresolved_imports"] = sum(r["result"] != "accepted" for r in report["asm"])
        report["unresolved_edits"] = sum(not r["instrument_edit_undo_redo"] for r in report["asm"]
                                         if r["result"] == "accepted") + sum(
            not s["instrument_edit_undo_redo"] for r in report["roms"] for s in r["slots"])
        report["status"] = "partial" if report["unresolved_imports"] or report["unresolved_edits"] else "passed"
    except Exception:
        report["status"] = "failed"
        raise
    finally:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Qualification: {report['status']}; unresolved imports={report['unresolved_imports']}; "
          f"unresolved edits={report['unresolved_edits']}")
    return 2 if report["status"] == "partial" else 0


if __name__ == "__main__":
    raise SystemExit(main())
