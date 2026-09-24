"""Independent CPU playback qualification; all fixture paths live in a private manifest."""
import argparse
import array
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import wave

from provenance import capture


def digest(data):
  return hashlib.sha256(data).hexdigest()


def analyze(wav_path, trace_path, case, watches):
  with wave.open(str(wav_path), "rb") as wav:
    if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (2, 2, 32000):
      raise ValueError("Unexpected PCM format")
    pcm = wav.readframes(wav.getnframes())
  samples = array.array("h", pcm)
  if sys.byteorder != "little":
    samples.byteswap()
  nonzero = sum(value != 0 for value in samples)
  rms = math.sqrt(sum(value * value for value in samples) / max(1, len(samples)))
  with trace_path.open(newline="") as stream:
    rows = list(csv.DictReader(stream))
  calls = []
  current = None
  for row in rows:
    pc = int(row["pc"], 16)
    if pc == int(watches["entry"], 16):
      current = {"slot": int(row["a"], 16) & 255, "blocks": [], "returned": False}
      calls.append(current)
    elif current is not None and pc == int(watches["block"], 16):
      current["blocks"].append({
        "length": int(row["x"], 16),
        "source_bank": int(row["ram12"], 16),
        "source_y_after_header": int(row["y"], 16),
      })
    elif current is not None and pc == int(watches["return"], 16):
      current["returned"] = True
  selected = [call for call in calls if call["slot"] == case["slot"]]
  loader_ok = bool(selected) and all(
    call["returned"] and call["blocks"] and call["blocks"][-1]["length"] == 0
    and any(block["length"] > 0 for block in call["blocks"])
    for call in selected
  )
  if "source_bank" in case:
    loader_ok = loader_ok and all(
      call["blocks"][0]["source_bank"] == case["source_bank"] for call in selected
    )
  return {
    "pcm_sha256": digest(pcm), "sample_count": len(samples), "nonzero_samples": nonzero,
    "peak": max((abs(value) for value in samples), default=0), "rms": rms,
    "audible": nonzero > 320 and rms > 1.0, "loader_ok": loader_ok, "calls": calls,
  }


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("manifest", type=Path)
  parser.add_argument("--exe", type=Path, default=Path(__file__).parent / "build/Release/cpu_playback.exe")
  parser.add_argument("--out", type=Path, required=True)
  args = parser.parse_args()
  config = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
  args.out.mkdir(parents=True, exist_ok=True)
  # Refuse collisions so previous qualification evidence cannot be silently replaced.
  if any(args.out.iterdir()):
    raise ValueError("Output directory must be empty")
  watches = config["watches"]
  report = {"schema": 1, "mode": "unmodified-rom-reset-cpu-boot", "cases": [],
            "exe_sha256": digest(args.exe.read_bytes()), "frames": config.get("frames", 600)}
  (args.out / "provenance.json").write_text(json.dumps(capture(args.exe), indent=2) + "\n", encoding="utf-8")
  by_name = {}
  for case in config["cases"]:
    name = case["name"]
    if not name or any(char not in "abcdefghijklmnopqrstuvwxyz0123456789-_" for char in name):
      raise ValueError("Use anonymous lowercase case names")
    if name in by_name:
      raise ValueError("Duplicate case name")
    rom = Path(os.path.expandvars(case["rom"]))
    command = [str(args.exe.resolve()), str(rom.resolve()), str((args.out / (name + ".wav")).resolve()),
               str((args.out / (name + ".csv")).resolve()), str(report["frames"]),
               str(case.get("channel_mask", 255)), *watches.values()]
    result = {"name": name, "rom_sha256": digest(rom.read_bytes()), "expected": case["expect"]}
    try:
      run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
      # External emulator diagnostics may contain ROM metadata: preserve only return code.
      result["exit_code"] = run.returncode
      if run.returncode:
        result.update(passed=False, error="emulator-process-failed")
      else:
        result.update(analyze(args.out / (name + ".wav"), args.out / (name + ".csv"), case, watches))
        if case["expect"] == "audible":
          result["passed"] = result["loader_ok"] and result["audible"]
        elif case["expect"] == "silent":
          result["passed"] = result["loader_ok"] and result["nonzero_samples"] == 0
        elif case["expect"] == "loader-rejected":
          # A crash or absent loader is NOT a successful defect control.
          selected = [call for call in result["calls"] if call["slot"] == case["slot"]]
          result["passed"] = any(
            call["blocks"] and not call["returned"] and call["blocks"][-1]["length"] != 0
            for call in selected
          )
        else:
          raise ValueError("Unknown expectation")
        for relation in ("pcm_equal", "pcm_different"):
          if relation in case:
            other = by_name[case[relation]]
            equal = result["pcm_sha256"] == other.get("pcm_sha256")
            result[relation] = equal if relation == "pcm_equal" else not equal
            result["passed"] &= result[relation] and other["passed"]
    except subprocess.TimeoutExpired:
      result.update(passed=False, error="timeout")
    report["cases"].append(result)
    by_name[name] = result
    print(name, "PASS" if result["passed"] else "FAIL", flush=True)
    report["passed"] = all(item["passed"] for item in report["cases"])
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
  return 0 if report["cases"] and report["passed"] else 1


if __name__ == "__main__":
  sys.exit(main())
