"""Source-backed repair safety tests using generated data, never commercial ROMs.

Requires GTB_SOUNDBANK and a built engine; writes tests/source-repair-regression.log.
"""
from pathlib import Path
import hashlib
import json
import os
import random
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RESULTS = []


def check(condition, description):
    if not condition:
        raise AssertionError(description)
    RESULTS.append("PASS " + description)


def fixtures():
    rng = random.Random(724)
    notes = bytes(rng.randrange(0x61, 0x80) for _ in range(220))
    body = bytes([8, 0x2e, 0x18, 0x10, 8, 0x2e, 6, 0xe0]) + notes + bytes([0x17])
    source = struct.pack(">8H", *([0xe10] * 8)) + body
    text = (".base $0D20\n!instrument_2E = #$08\n" +
            "dw melody,melody,melody,melody,melody,melody,melody,melody\n" +
            "melody: db 8,!instrument_2E\ndb $18\ndb $10,$08,$2D,$26\ndb $E0\n" +
            "db " + ",".join(f"${n:02X}" for n in notes) + ",$17\n")
    rom = bytearray(4096)
    # Synthetic signature bytes: this is not executable game content.
    signature = bytes.fromhex("1c fd f6 01 04 2d f6 00 04 2d ad 08 90 10 fb 00 f4 08 da a0 8d 00 f7 a0 bb 00 d0 02 bb 08 6f")
    rom[32:32 + len(signature)] = signature
    table = 32 + len(signature)
    for opcode in (0x1e, 0x1f):
        struct.pack_into('<H', rom, table + opcode * 2, 0x500)
    rom[table + 0x100] = 0x6f
    rom[512:512 + len(source)] = source
    return text, rom, source


def run(requests):
    engine = os.environ.get("GTB_TEST_ENGINE", str(ROOT / "build/win-x64/bin/gtb-engine.exe"))
    process = subprocess.run([engine], input="".join(json.dumps(r) + "\n" for r in requests),
                             capture_output=True, text=True, timeout=120)
    check(process.returncode == 0, "engine exits normally")
    replies = [json.loads(line) for line in process.stdout.splitlines()]
    check(len(replies) == len(requests), "one reply for every repair request")
    return replies


def main():
    bank = os.environ.get("GTB_SOUNDBANK")
    if not bank or not Path(bank).is_file():
        raise RuntimeError("Set GTB_SOUNDBANK to a local test SPC; absent prerequisites must fail")
    text, rom, source = fixtures()
    with tempfile.TemporaryDirectory(prefix="gtb-repair-test-") as directory:
        path = Path(directory) / "source.sfc"
        path.write_bytes(rom)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        inspect = {"cmd": "inspectAsmRepair", "asm": text, "sourceRom": str(path)}
        replies = run([{"cmd": "open", "path": bank}, {"cmd": "exportAsm"}, inspect,
                       {"cmd": "exportAsm"}])
        check(all(r["ok"] for r in replies), "valid source-backed proposal accepted")
        proposal = replies[2]
        check(replies[1]["asm"] == replies[3]["asm"], "review leaves live sequence unchanged")
        check(proposal["repairReport"]["requiresReview"], "explicit review is required")
        changes = proposal["repairReport"]["changes"]
        check({c["offset"] for c in changes} == {21, 22}, "only the proven corrupted operands change")
        check(all(c["offset"] >= 16 for c in changes), "channel header is never repaired as an operand")
        check(hashlib.sha256(path.read_bytes()).hexdigest() == digest, "source ROM stays byte-identical")
        imported = run([{"cmd": "importAsm", "asm": proposal["asm"]}, {"cmd": "exportAsm"}])
        check(all(r["ok"] for r in imported), "reviewed copy can be explicitly imported")
        duplicate = bytearray(rom)
        duplicate[2048:2048 + len(source)] = source
        wrong = Path(directory) / "ambiguous.sfc"
        wrong.write_bytes(duplicate)
        missing = Path(directory) / "missing.sfc"
        unsupported = Path(directory) / "unsupported.sfc"
        unsupported.write_bytes(bytes(4096))
        commented = text.replace("!instrument_2E = #$08", "; !instrument_2E = #$08").replace(
            "db 8,!instrument_2E", "db 8,8")
        unmapped = run([dict(inspect, asm=commented)])[0]
        check(unmapped["ok"] and 46 in unmapped["asmReport"]["sourcePrograms"],
              "commented alias leaves the restored source instrument explicitly unmapped")
        check(not any(c["rule"] == "source-instrument-alias" for c in unmapped["repairReport"]["changes"]),
              "commented aliases cannot authorize a mapping")
        check(not run([{"cmd": "importAsm", "asm": unmapped["asm"]}])[0]["ok"],
              "restored source instrument cannot import without an explicit mapping")
        cases = [
            (dict(inspect, sourceRom=str(wrong)), "ambiguous source is rejected"),
            (dict(inspect, sourceRom=str(missing)), "missing source is rejected"),
            (dict(inspect, sourceRom=str(unsupported)), "unknown driver is rejected"),
            (dict(inspect, asm="incbin arbitrary.bin"), "ASM is never executed"),
            (dict(inspect, asm=text.replace("#$08", "#$FF")), "out-of-bank aliases are rejected"),
        ]
        for request, label in cases:
            rejected = run([{"cmd": "open", "path": bank}, {"cmd": "exportAsm"}, request,
                            {"cmd": "exportAsm"}])
            check(not rejected[2]["ok"], label)
            check(rejected[1]["asm"] == rejected[3]["asm"], label + " without changing the live song")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        RESULTS.append("FAIL " + str(error))
        raise
    finally:
        (ROOT / "tests/source-repair-regression.log").write_text("\n".join(RESULTS) + "\n")
        print("\n".join(RESULTS))
