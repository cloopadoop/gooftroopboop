"""Compare editor playback against a separately compiled Snes9x BAPU renderer.

Private ASM/SPC inputs are supplied by the caller. Requires numpy and scipy.
Outputs must be Git-ignored. Missing fixtures, silent captures and failed
comparisons are errors, not skips. No time stretching or per-song EQ fitting.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import wave

import numpy as np
from scipy import signal

ROOT = Path(__file__).resolve().parents[1]


def pcm(path):
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (2, 2, 32000):
            raise ValueError("Expected stereo 16-bit 32 kHz capture")
        return np.frombuffer(reader.readframes(reader.getnframes()), dtype="<i2").reshape(-1, 2).astype(float)


def compare(reference, test):
    if reference.shape != test.shape or len(test) < 64000:
        raise ValueError("Capture dimensions or duration mismatch")
    # Fixed, independently expressed player-output filter. Keep raw references.
    reference = signal.lfilter([0, .25, .5, -.75], [1, -255 / 256], reference, axis=0)
    # One constant start offset, bounded to 1 ms. Never rescale the timeline.
    a = reference[32000:].mean(axis=1)
    b = test[32000:].mean(axis=1)
    correlation = signal.correlate(b, a, method="fft")
    lags = signal.correlation_lags(len(b), len(a))
    keep = abs(lags) <= 32
    lag = int(lags[keep][np.argmax(correlation[keep])])
    indices = np.arange(32032, len(test) - 32)
    x, y = reference[indices - lag], test[indices]
    # Correlation alone can hide silence, gain errors, or a missing stereo side.
    energy = np.sqrt(np.mean(x * x, axis=0))
    test_energy = np.sqrt(np.mean(y * y, axis=0))
    if np.any(energy < 2) or np.any(test_energy < 2):
        raise ValueError("Capture is silent or has insufficient signal")
    coefficients = [float(np.corrcoef(x[:, c], y[:, c])[0, 1]) for c in range(2)]
    gain_db = (20 * np.log10(test_energy / energy)).tolist()
    return dict(lag_frames=lag, channel_correlations=coefficients, gain_db=gain_db,
                passed=all(np.isfinite(c) and c >= .999 for c in coefficients) and
                       all(np.isfinite(g) and abs(g) <= .1 for g in gain_db))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--reference-exe", type=Path, required=True)
    parser.add_argument("--bank", type=Path, required=True)
    parser.add_argument("--asm-dir", type=Path, required=True)
    parser.add_argument("--pattern", default="*.asm")
    parser.add_argument("--out", type=Path, default=ROOT / "build/reference-results")
    parser.add_argument("--discard-passing-captures", action="store_true",
                        help="Use a new output directory; keep metrics, retain audio only for failed comparisons")
    args = parser.parse_args()
    args.out = args.out.resolve()
    if subprocess.run(["git", "check-ignore", "-q", str(args.out / "probe.wav")], cwd=ROOT).returncode:
        raise ValueError("Output directory must be Git-ignored before creating private captures")
    files = sorted(args.asm_dir.glob(args.pattern))
    if not files or not all(p.is_file() for p in [args.engine, args.reference_exe, args.bank]):
        raise ValueError("Required engine, reference renderer, bank or ASM fixtures are missing")
    if args.discard_passing_captures and args.out.exists():
        raise ValueError("Capture cleanup requires a new output directory, never an existing one")
    args.out.mkdir(parents=True, exist_ok=True)
    reports = []
    report = dict(engine_sha256=hashlib.sha256(args.engine.read_bytes()).hexdigest(),
                  reference_sha256=hashlib.sha256(args.reference_exe.read_bytes()).hexdigest(),
                  passed=False, status="running", expected_captures=len(files), captures=reports)
    def checkpoint(status):
        report["status"] = status
        report["passed"] = status == "complete" and len(reports) == len(files) and all(r["passed"] for r in reports)
        (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    checkpoint("running")
    for index, source in enumerate(files):
        stem = args.out / f"capture-{index:03d}"
        session, snapshot = stem.with_suffix(".gtb"), stem.with_suffix(".spc")
        rendered, reference = stem.with_suffix(".wav"), stem.with_suffix(".pcm")
        # Sessions retain the exact bank of a ROM/SPC-loaded song. Do not
        # silently reinterpret them through an unrelated ASM target bank.
        open_command = "openSession" if source.suffix.lower() == ".gtb" else "importAsm"
        requests = [{"cmd": open_command, "path": str(source.resolve())},
                    {"cmd": "saveSession", "path": str(session)},
                    {"cmd": "render", "seconds": 30, "path": str(rendered)}]
        result = subprocess.run([str(args.engine.resolve())],
                                input="".join(json.dumps(r) + "\n" for r in requests),
                                text=True, capture_output=True, timeout=120,
                                env=dict(os.environ, GTB_SOUNDBANK=str(args.bank.resolve())))
        responses = [json.loads(line) for line in result.stdout.splitlines()]
        if result.returncode or len(responses) != 3 or not all(r.get("ok") for r in responses):
            report["failure"] = dict(index=index, category="editor-capture")
            checkpoint("failed")
            raise RuntimeError("Editor capture failed")
        snapshot.write_bytes(base64.b64decode(json.loads(session.read_text())["spcBase64"], validate=True))
        subprocess.run([str(args.reference_exe.resolve()), str(snapshot), str(reference), "30"],
                       check=True, timeout=120)
        raw = reference.read_bytes()
        if len(raw) != 30 * 32000 * 4:
            raise RuntimeError("Reference capture length mismatch")
        reference_pcm = np.frombuffer(raw, dtype="<i2").reshape(-1, 2).astype(float)
        metrics = compare(reference_pcm, pcm(rendered))
        metrics.update(fixture_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                       independent_cpu=True, independent_dsp=False, seconds=30)
        reports.append(metrics)
        checkpoint("running")
        print(f"Capture {index + 1}/{len(files)}: {'PASS' if metrics['passed'] else 'FAIL'}", flush=True)
        if args.discard_passing_captures and metrics["passed"]:
            trace = Path(str(reference) + ".trace.csv")
            for generated in (session, snapshot, rendered, reference, trace):
                if generated.resolve().parent != args.out:
                    raise ValueError("Generated capture escaped the owned output directory")
                if generated.exists():
                    generated.unlink()
        elif args.discard_passing_captures:
            break  # retain the first failure for diagnosis; bound temporary storage
    if hashlib.sha256(args.engine.read_bytes()).hexdigest() != report["engine_sha256"]:
        checkpoint("engine-changed")
        raise RuntimeError("Engine changed during playback qualification")
    checkpoint("complete" if len(reports) == len(files) else "failed")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
