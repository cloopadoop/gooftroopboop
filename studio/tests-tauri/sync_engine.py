"""Copy the explicitly selected build into ignored test sidecars and record hashes."""
import hashlib
import json
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
engine_root = ROOT.parent if (ROOT.parent / "src/engine").is_dir() else ROOT.parent / "gooftroopboop"
source = Path(os.environ.get("GTB_TEST_ENGINE", str(engine_root / "build/win-x64/bin/gtb-engine.exe"))).resolve(strict=True)
destination = ROOT / "src-tauri/bin/gtb-engine.exe"
destination.parent.mkdir(parents=True, exist_ok=True)
(ROOT / "output/tauri").mkdir(parents=True, exist_ok=True)
if source != destination.resolve():
    shutil.copy2(source, destination)
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
if digest(source) != digest(destination):
    raise SystemExit("Engine copy hash mismatch")
(ROOT / "output/tauri/engine-manifest.json").write_text(json.dumps({"sha256":digest(source)},indent=2),encoding="utf-8")
print("Test sidecar synchronized; SHA-256:", digest(source))
