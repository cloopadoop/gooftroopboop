#!/usr/bin/env python3
"""Independent ROM safety tests; never imports regression.py.

Run: python tests/test_rom_safety.py
GTB_TEST_ENGINE / GTB_TEST_ROM / GTB_SOUNDBANK override private local fixtures.
Generate emulator inputs: python tests/test_rom_safety.py --fixtures <directory>

Oracle: Goof Troop U CPU $8098DC decodes table entries; $809928 wraps the
source bank; $80995C reads each upload block header, including the terminator.
The Python oracle models the loader's bank/Y bus reads and upload blocks; it
does not call the editor resolver. This is NOT a full CPU/emulator claim.
No commercial ROM bytes are stored in this test source or published.
"""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent.parent
ENGINE = Path(os.environ.get("GTB_TEST_ENGINE", ROOT / "build/win-x64/bin/gtb-engine.exe"))
ROM = Path(os.environ.get("GTB_TEST_ROM", WORKSPACE / "Roms/Goof Troop (U) [!].smc"))
BANK = Path(os.environ.get("GTB_SOUNDBANK", WORKSPACE / "Music Sources/SPC/08 Hamlet.spc"))
TABLE = 0x20000


def run_engine(*requests):
    env = dict(os.environ, GTB_SOUNDBANK=str(BANK))
    # The engine currently uses a fixed import ARAM filename. Isolate this
    # process from parallel UI/tests so independent checks cannot race it.
    with tempfile.TemporaryDirectory(prefix="gtb-rom-process-") as temporary:
        env.update(TEMP=temporary, TMP=temporary, TMPDIR=temporary)
        process = subprocess.run([str(ENGINE)], input="".join(json.dumps(r) + "\n" for r in requests),
                                 text=True, capture_output=True, timeout=90, cwd=ROOT, env=env)
    if process.returncode:
        raise AssertionError(f"engine exited with status {process.returncode}")
    try:
        replies = [json.loads(line) for line in process.stdout.splitlines()]
    except json.JSONDecodeError:
        raise AssertionError("engine returned invalid protocol JSON") from None
    if len(replies) != len(requests):
        raise AssertionError(f"expected {len(requests)} replies, got {len(replies)}")
    return replies


def header(rom):
    return 512 if len(rom) % 32768 == 512 else 0


def payload(rom):
    return rom[header(rom):]


def loader_stream(rom, slot):
    """Actual bus-address sequence: DP long pointer $bank:8000 plus 16-bit Y.

    Incrementing Y into bit15 resets it to zero and increments the bank. This
    intentionally does not use (table_base + byte2 * bank_size), the old bug.
    """
    rom = payload(rom)
    entry = TABLE + 3 * slot
    y = struct.unpack_from("<H", rom, entry)[0] & 0x7fff
    bank = rom[entry + 2] | 0x84
    while True:
        address = (bank << 16) | (0x8000 + y)
        pc = ((address & 0x7f0000) >> 1) | (address & 0x7fff)
        if pc >= len(rom):
            raise AssertionError(f"loader read outside ROM at ${address:06X}")
        yield address, rom[pc]
        y += 1
        if y & 0x8000:
            y = 0
            bank = (bank + 1) & 255


def upload(rom, slot):
    stream = loader_stream(rom, slot)
    start, lo = next(stream)
    size = lo | next(stream)[1] << 8
    load = next(stream)[1] | next(stream)[1] << 8
    if not (0 < size <= 0x32e0 and load == 0x0d20):
        raise AssertionError(f"not a supported song block: size={size:x} load={load:x}")
    sequence = bytes(next(stream)[1] for _ in range(size))
    terminal = bytes(next(stream)[1] for _ in range(4))
    if terminal != bytes(4):
        raise AssertionError(f"game would consume another block: {terminal.hex()}")
    pc = ((start & 0x7f0000) >> 1) | (start & 0x7fff)
    return pc, sequence


def checksum(rom):
    offset = header(rom)
    rom[offset + 0x7fdc:offset + 0x7fe0] = bytes.fromhex("ffff0000")
    value = sum(rom[offset:]) & 65535
    struct.pack_into("<HH", rom, offset + 0x7fdc, value ^ 65535, value)


def expanded(stock, size, fill=0xff):
    rom = bytearray(payload(stock))
    rom.extend(bytes([fill]) * (size - len(rom)))
    rom[0x7fd7] = size.bit_length() - 11
    checksum(rom)
    return rom


def point(rom, slot, cpu_address, redundant_bits=0):
    """Encode from a literal CPU address, independent of engine PC encoder."""
    assert cpu_address >> 16 & 0x84 == 0x84
    struct.pack_into("<HB", rom, header(rom) + TABLE + 3 * slot,
                     cpu_address & 0xffff, ((cpu_address >> 16) & 0x7b) | redundant_bits)


class RomSafety(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stock = ROM.read_bytes()
        cls.clean = payload(cls.stock)
        assert len(cls.clean) == 0x80000, "GTB_TEST_ROM must be the qualified stock U image"
        assert cls.clean[0x1903:0x1905] == bytes.fromhex("0984"), "missing ORA #$84"
        assert cls.clean[0x1928:0x1932] == bytes.fromhex("b710c81005a00000e612")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gtb-rom-safety-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def file(self, name, data):
        path = self.directory / name
        path.write_bytes(data)
        return path

    def export(self, template, slot=0x10, source=0x1e, out=None):
        out = out or self.directory / "output.sfc"
        replies = run_engine({"cmd": "openRom", "path": str(ROM), "slot": source},
                             {"cmd": "exportRom", "rom": str(template), "slot": slot, "path": str(out)})
        self.assertTrue(replies[0]["ok"], "source open rejected at command 0")
        return replies[1], out

    def preserved(self, before, after, slot, spans=()):
        """Compare ALL input bytes, not just occupied/non-FF sentinels."""
        expected = bytearray(before)
        h = header(before)
        for begin, end in ((TABLE + slot * 3, TABLE + slot * 3 + 3),
                           (0x7fd7, 0x7fd8), (0x7fdc, 0x7fe0), *spans):
            expected[h + begin:h + end] = after[h + begin:h + end]
        self.assertEqual(bytes(expected), after[:len(before)])

    def valid_checksum(self, rom):
        body = payload(rom)
        complement, value = struct.unpack_from("<HH", body, 0x7fdc)
        self.assertEqual(complement ^ value, 65535)
        self.assertEqual(value, sum(body) & 65535)
        self.assertEqual(body[0x7fd7], len(body).bit_length() - 11)

    def test_all_stock_songs_noop_and_terminal(self):
        listed = run_engine({"cmd": "listRomSongs", "path": str(ROM)})[0]
        self.assertTrue(listed["ok"], "song listing rejected")
        self.assertEqual([s["slot"] for s in listed["slots"]], list(range(0x10, 0x23)))
        for slot in range(0x10, 0x23):
            with self.subTest(slot=slot):
                upload(self.stock, slot)
                reply, out = self.export(ROM, slot, slot)
                self.assertTrue(reply["ok"], "export rejected")
                self.assertFalse(reply["relocated"])
                self.assertEqual(out.read_bytes(), self.stock)

    def test_fresh_expansion_and_loader_or_address(self):
        reply, out = self.export(ROM)
        self.assertTrue(reply["ok"], "export rejected")
        data = out.read_bytes()
        pc, seq = upload(data, 0x10)
        self.assertEqual(pc, 0xa0000)
        # Stock Hamlet has one unused FF tail byte, omitted by serialization.
        self.assertEqual(seq, upload(self.stock, 0x1e)[1][:-1])
        self.assertEqual(reply["blobPc"], pc)
        self.assertEqual(len(payload(data)), 0x100000)
        self.preserved(self.stock, data, 0x10)
        self.valid_checksum(data)

    def test_existing_expansion_reserved_including_ff(self):
        for size, address in ((0x100000, 0x120000), (0x200000, 0x220000)):
            for fill in (0x00, 0x5a, 0xff):
                with self.subTest(size=size, fill=fill):
                    before = expanded(self.stock, size, fill)
                    template = self.file("expanded.sfc", before)
                    reply, out = self.export(template)
                    self.assertTrue(reply["ok"], "export rejected")
                    after = out.read_bytes()
                    self.assertEqual(upload(after, 0x10)[0], address)
                    self.assertEqual(len(after), size * 2)
                    self.preserved(before, after, 0x10)
                    self.valid_checksum(after)

    def test_headered_expansion_and_inplace(self):
        copier = bytes(range(256)) * 2
        for size in (0x80000, 0x100000):
            before = copier + expanded(self.stock, size)
            path = self.file("headered.smc", before)
            reply, out = self.export(path)
            self.assertTrue(reply["ok"], "export rejected")
            self.assertEqual(out.read_bytes()[:512], copier)
            self.preserved(before, out.read_bytes(), 0x10)
            self.valid_checksum(out.read_bytes())
            reply, out2 = self.export(out, out=self.directory / "again.smc")
            self.assertTrue(reply["ok"], "export rejected")
            self.assertFalse(reply["relocated"])
            self.assertEqual(out.read_bytes(), out2.read_bytes())

    def test_shrink_terminator_stays_inside_owned_span(self):
        original_pc, original = upload(self.stock, 0x1e)
        reply, out = self.export(ROM, slot=0x1e, source=0x21)
        self.assertTrue(reply["ok"], "export rejected")
        self.assertFalse(reply["relocated"])
        # Stock Game Over ends with one redundant END byte beyond live tracks.
        self.assertEqual(upload(out.read_bytes(), 0x1e)[1], upload(self.stock, 0x21)[1][:-1])
        self.preserved(self.stock, out.read_bytes(), 0x1e,
                       ((original_pc, original_pc + len(original) + 8),))

    def test_alias_and_partial_overlap_copy_on_write(self):
        for overlap in (False, True):
            before = expanded(self.stock, 0x100000)
            pc = 0xa0100
            sequence = upload(self.stock, 0x1e)[1]
            before[pc:pc + len(sequence) + 8] = struct.pack("<HH", len(sequence), 0xd20) + sequence + bytes(4)
            point(before, 0x10, 0x948100)
            if overlap:
                # A second valid song starts inside the first and shares its tail.
                other = pc + 16
                struct.pack_into("<HH", before, other, len(sequence) - 16, 0xd20)
                point(before, 0x11, 0x948110)
            else:
                point(before, 0x11, 0x948100)
            template = self.file("shared.sfc", before)
            reply, out = self.export(template, source=0x21)
            self.assertTrue(reply["ok"], "export rejected")
            self.assertTrue(reply["relocated"])
            self.assertEqual(upload(out.read_bytes(), 0x10)[0], 0x120000)
            self.preserved(before, out.read_bytes(), 0x10)

    def test_relocation_never_erases_old_expanded_blob(self):
        before = expanded(self.stock, 0x100000)
        seq = upload(self.stock, 0x21)[1]
        before[0xa0000:0xa0000 + len(seq) + 8] = struct.pack("<HH", len(seq), 0xd20) + seq + bytes(4)
        point(before, 0x10, 0x948000)
        reply, out = self.export(self.file("old-relocation.sfc", before))
        self.assertTrue(reply["ok"], "export rejected")
        self.preserved(before, out.read_bytes(), 0x10)

    def test_or_aliases_and_cross_bank_headers_payload_terminator(self):
        for cpu in (0x94fffd, 0x97fff0, 0x94ffff):
            for bits in (0, 0x04, 0x80, 0x84):
                before = expanded(self.stock, 0x100000)
                seq = upload(self.stock, 0x1e)[1]
                pc = ((cpu & 0x7f0000) >> 1) | (cpu & 0x7fff)
                before[pc:pc + len(seq) + 8] = struct.pack("<HH", len(seq), 0xd20) + seq + bytes(4)
                point(before, 0x10, cpu, bits)
                checksum(before)
                template = self.file("cross-bank.sfc", before)
                reply, out = self.export(template)
                self.assertTrue(reply["ok"], "export rejected")
                self.assertFalse(reply["relocated"])
                self.assertEqual(reply["blobPc"], pc)
                self.assertEqual(upload(out.read_bytes(), 0x10)[1], seq)
                self.assertEqual(out.read_bytes(), before)

    def test_all_256_loader_bank_bytes_resolve_via_or(self):
        for group in range(0, 256, 48):
            rom = expanded(self.stock, 0x400000)
            expected = {}
            rom[TABLE:TABLE + 48 * 3] = bytes(48 * 3)
            for slot, byte2 in enumerate(range(group, min(group + 48, 256))):
                # Literal bus read address, with a unique size for each bank.
                cpu = ((0x84 | byte2) << 16) | 0xe000
                pc = ((cpu & 0x7f0000) >> 1) | 0x6000
                size = (cpu >> 16 & 0x7f) + 1
                struct.pack_into("<HB", rom, TABLE + slot * 3, 0xe000, byte2)
                rom[pc:pc + size + 8] = struct.pack("<HH", size, 0xd20) + bytes(size + 4)
                expected[slot] = size
            path = self.file("all-banks.sfc", rom)
            reply = run_engine({"cmd": "listRomSongs", "path": str(path)})[0]
            self.assertTrue(reply["ok"], "song listing rejected")
            actual = {s["slot"]: s["size"] for s in reply["slots"]}
            self.assertEqual({s: actual.get(s) for s in expected}, expected)

    def test_rejected_exports_preserve_existing_and_absent_destinations(self):
        invalid = []
        invalid.append(("full", expanded(self.stock, 0x400000), 0x10))
        invalid.append(("truncated", self.clean[:-1], 0x10))
        invalid.append(("odd-size", self.clean + bytes(0x8000), 0x10))
        invalid.append(("invalid-slot", self.clean, 0x30))
        invalid.append(("driver-slot", self.clean, 0))
        for label, position in (("mapper", 0x7fd5), ("decoder", 0x1903), ("stream-loop", 0x1928)):
            bad = bytearray(self.clean)
            bad[position] ^= 1
            invalid.append((label, bad, 0x10))
        broken = bytearray(self.clean)
        pc, seq = upload(broken, 0x10)
        broken[pc + 4 + len(seq)] = 0xff
        invalid.append(("missing-terminator", broken, 0x10))
        for label, before, slot in invalid:
            for exists in (False, True):
                with self.subTest(label=label, exists=exists):
                    template = self.file("invalid.sfc", before)
                    out = self.directory / "reject.sfc"
                    if exists:
                        out.write_bytes(b"existing destination must survive")
                    elif out.exists():
                        out.unlink()
                    reply, _ = self.export(template, slot=slot, out=out)
                    self.assertFalse(reply["ok"], "invalid export unexpectedly succeeded")
                    self.assertEqual(template.read_bytes(), before)
                    if exists:
                        self.assertEqual(out.read_bytes(), b"existing destination must survive")
                    else:
                        self.assertFalse(out.exists())
                    self.assertFalse(list(self.directory.glob("*.gtb-rom-*")))

    def test_atomic_replacement_failure_and_inplace_destination(self):
        destination = self.directory / "destination.sfc"
        destination.mkdir()
        keep = destination / "keep"
        keep.write_bytes(b"unrelated")
        reply, _ = self.export(ROM, out=destination)
        self.assertFalse(reply["ok"], "invalid destination unexpectedly succeeded")
        self.assertEqual(keep.read_bytes(), b"unrelated")
        self.assertFalse(list(self.directory.glob("*.gtb-rom-*")))
        template = self.file("same.sfc", self.stock)
        reply, _ = self.export(template, out=template)
        self.assertTrue(reply["ok"], "export rejected")
        self.assertEqual(upload(template.read_bytes(), 0x10)[1], upload(self.stock, 0x1e)[1][:-1])


def make_fixtures(directory):
    """Private derived inputs for the independent CPU harness, with hashes."""
    directory.mkdir(parents=True, exist_ok=True)
    stock = ROM.read_bytes()
    small = bytearray(stock)
    h = header(small)
    small[h + TABLE + 0x10 * 3:h + TABLE + 0x10 * 3 + 3] = small[h + TABLE + 0x21 * 3:h + TABLE + 0x21 * 3 + 3]
    template = directory / "small-logo-template.sfc"
    template.write_bytes(small)
    fixtures = {}
    for name, source, target in (("baseline", 0x10, ROM),
                                 ("relocated-logo", 0x10, template),
                                 ("relocated-hamlet", 0x1e, ROM)):
        path = directory / (name + ".sfc")
        replies = run_engine({"cmd": "openRom", "path": str(ROM), "slot": source},
                             {"cmd": "exportRom", "rom": str(target), "slot": 0x10, "path": str(path)})
        if not all(r["ok"] for r in replies):
            failed = next(i for i, reply in enumerate(replies) if not reply["ok"])
            raise AssertionError(f"fixture command {failed} rejected")
        pc, seq = upload(path.read_bytes(), 0x10)
        fixtures[name] = {"path": str(path), "blobPc": pc, "terminatorPc": pc + 4 + len(seq),
                          "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    broken = bytearray(Path(fixtures["relocated-hamlet"]["path"]).read_bytes())
    terminal = header(broken) + fixtures["relocated-hamlet"]["terminatorPc"]
    broken[terminal:terminal + 4] = b"\xff" * 4
    checksum(broken)
    bad_path = directory / "missing-terminator-hamlet.sfc"
    bad_path.write_bytes(broken)
    fixtures["missing-terminator-hamlet"] = {"path": str(bad_path), "terminatorPc": terminal,
                                            "sha256": hashlib.sha256(broken).hexdigest()}
    manifest = json.dumps(fixtures, indent=2)
    (directory / "manifest.json").write_text(manifest + "\n", encoding="utf-8")
    print(manifest)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--fixtures":
        make_fixtures(Path(sys.argv[2]).resolve())
    else:
        class Tee:
            def __init__(self, log):
                self.log = log

            def write(self, text):
                sys.stderr.write(text)
                self.log.write(text)

            def flush(self):
                sys.stderr.flush()
                self.log.flush()

        with (ROOT / "tests/test_rom_safety.log").open("w", encoding="utf-8") as log:
            unittest.main(testRunner=unittest.TextTestRunner(stream=Tee(log), verbosity=2))
