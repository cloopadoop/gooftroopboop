"""Positive and fail-closed controls for the optional source-driver CPU probe."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "build/driver-probe/driver-probe.exe"


def snapshot(code):
    image = bytearray(0x10200)
    image[:25] = b"SNES-SPC700 Sound File Data"
    image[0x25:0x27] = (0x400).to_bytes(2, "little")
    image[0x2b] = 0xef
    image[0x500:0x500 + len(code)] = code
    image[0x2f0:0x2f2] = (0xfe00).to_bytes(2, "little")
    image[0xff00:0xff02] = bytes([0x2f, 0xfe])
    image[0x1016c] = 0x60
    return image


def main():
    if not PROBE.is_file():
        raise SystemExit("Build the optional driver probe before running its controls")
    checks = 0
    with tempfile.TemporaryDirectory(prefix="gtb-driver-probe-") as directory:
        folder = Path(directory)
        for name, code, expected in (
            ("write-return", bytes([0xe8, 55, 0xc5, 0x40, 0x01, 0x6f]), 0),
            ("dsp-write", bytes([0x8f, 0x0c, 0xf2, 0x8f, 55, 0xf3, 0x6f]), 0),
            ("halt", bytes([0xff]), 1),
            ("loop", bytes([0x2f, 0xfe]), 1),
            ("jump-without-return", bytes([0x5f, 0x00, 0xfe]), 1),
        ):
            source, result = folder / (name + ".spc"), folder / (name + ".json")
            source.write_bytes(snapshot(code))
            process = subprocess.run([str(PROBE), str(source), "0xfe00", str(result)],
                                     capture_output=True, timeout=5)
            assert process.returncode == expected, "Probe return contract failed: " + name
            report = json.loads(result.read_text())
            assert report["returned"] == (expected == 0)
            assert report["cycle_budget"] == 4096
            if name == "write-return":
                assert report["sp"] == 0xf1
                assert {"address": 0x140, "before": 0, "after": 55} in report["changes"]
            if name == "dsp-write":
                assert {"address": 0x0c, "before": 0, "after": 55} in report["dsp_changes"]
            checks += 1
            before = result.read_bytes()
            repeated = subprocess.run([str(PROBE), str(source), "0xfe00", str(result)],
                                      capture_output=True, timeout=5)
            assert repeated.returncode == 2 and result.read_bytes() == before
            checks += 1
        invalid = folder / "invalid.spc"
        invalid.write_bytes(b"not an SPC")
        missing = folder / "must-not-exist.json"
        rejected = subprocess.run([str(PROBE), str(invalid), "0xfe00", str(missing)],
                                  capture_output=True, timeout=5)
        assert rejected.returncode == 2 and not missing.exists()
        checks += 1
        for sentinel in ("-4294902272", "4295032320", "0x10000", "0x1ff", "0x1000000000000"):
            rejected = subprocess.run([str(PROBE), str(source), sentinel, str(missing)],
                                      capture_output=True, timeout=5)
            assert rejected.returncode == 2 and not missing.exists()
            checks += 1
    print(f"{checks} bounded driver-probe controls passed")


if __name__ == "__main__":
    main()
