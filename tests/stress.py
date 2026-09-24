"""Repeatable seeded model/IR stress campaign over local SPC and raw songs."""
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CORPUS = Path(os.environ.get("GTB_TEST_CORPUS", str(ROOT.parents[1] / "Music Sources")))
OUTPUT = ROOT / "build/test-artifacts/stress"


def check_model_startup(model):
    result = subprocess.run([str(model)], capture_output=True, timeout=20, cwd=ROOT)
    if result.returncode != 2 or b"Usage: gtb-model-test" not in result.stderr:
        raise RuntimeError(f"Model tester cannot start (exit 0x{result.returncode & 0xffffffff:08X}); "
                           "build Release with the matching Qt Core runtime before stress")


def main():
    inputs = sorted((CORPUS / "SPC").glob("*.spc"))
    inputs += sorted(p for p in (CORPUS / "RAW").iterdir()
                     if p.is_file() and p.name not in {"CapcomSnesInstrSet", "SNESSampColl"})
    if len(inputs) < 36:
        raise RuntimeError("Full stress requires the local SPC and RAW song corpus")
    model = ROOT / "build/win-x64/bin/gtb-model-test.exe"
    cli = ROOT / "build/win-x64/bin/Debug/GTBoop-cli.exe"
    for binary in [model, cli]:
        if not binary.is_file():
            raise RuntimeError("Build model tester and CLI before stress")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    check_model_startup(model)
    jobs = [(kind, source, seed) for kind in ["model", "ir"] for source in inputs for seed in [1, 17, 20260904]]

    def run(job):
        kind, source, seed = job
        args = [str(model)] if kind == "model" else [str(cli), "fuzz"]
        args += [str(source), "--seed", str(seed), "--ops", "300" if kind == "model" else "400"]
        try:
            result = subprocess.run(args, capture_output=True, text=True, timeout=180, cwd=ROOT)
            output = result.stdout + result.stderr
            passed = result.returncode == 0 and "PASS" in output
            output = f"Process exit code: {result.returncode}\n" + output
        except subprocess.TimeoutExpired:
            output, passed = "Timed out after 180 seconds", False
        name = f"{kind}-{source.parent.name}-{source.stem}-{seed}"
        (OUTPUT / (name + ".log")).write_text(output, encoding="utf-8")
        return {"case":name, "passed":passed}

    started = time.monotonic()
    results = []
    with ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(run, job) for job in jobs]
        for future in as_completed(futures):
            results.append(future.result())
            if len(results) % 12 == 0 or len(results) == len(jobs):
                elapsed = time.monotonic() - started
                remaining = len(jobs) - len(results)
                progress = {"completed": len(results), "total": len(jobs),
                            "failed": sum(not result["passed"] for result in results),
                            "elapsed_seconds": round(elapsed, 1),
                            "estimated_remaining_seconds": round(elapsed / len(results) * remaining, 1)}
                (OUTPUT / "progress.json").write_text(json.dumps(progress, indent=2), encoding="utf-8")
                print(f"Stress progress: {len(results)}/{len(jobs)}; failed={progress['failed']}; "
                      f"ETA {progress['estimated_remaining_seconds']} seconds", flush=True)
    results.sort(key=lambda result: result["case"])
    report = {"cases":results, "inputs":len(inputs),
              "model_sha256":hashlib.sha256(model.read_bytes()).hexdigest(),
              "cli_sha256":hashlib.sha256(cli.read_bytes()).hexdigest(),
              "model_operations":sum(r["passed"] and r["case"].startswith("model-") for r in results)*300,
              "ir_operations":sum(r["passed"] and r["case"].startswith("ir-") for r in results)*400,
              "requested_model_operations":len(inputs)*3*300,"requested_ir_operations":len(inputs)*3*400}
    (OUTPUT / "summary.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    failed = sum(not result["passed"] for result in results)
    print(f"Stress: {len(results)-failed}/{len(results)} cases passed; {report['model_operations']} model + {report['ir_operations']} IR operations")
    print("Detailed evidence: build/test-artifacts/stress/summary.json")
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
