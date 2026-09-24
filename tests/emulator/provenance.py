"""Capture private, content-addressed local dependency and harness build evidence."""
import hashlib
import re
from pathlib import Path
import subprocess


def capture(executable):
  harness = Path(__file__).resolve().parent
  build = executable.resolve().parent.parent
  cache = build / "CMakeCache.txt"
  settings = {}
  for line in cache.read_text(encoding="utf-8").splitlines():
    if line and not line.startswith(("#", "//")) and "=" in line:
      key, value = line.split("=", 1)
      settings[key.split(":", 1)[0]] = value
  source = Path(settings["SNSF9X_SOURCE"])
  compiler_files = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
  for file in compiler_files:
    for key, value in re.findall(r'set\((CMAKE_CXX_[A-Z_]+) "([^"]*)"\)',
                                 file.read_text(encoding="utf-8")):
      settings[key] = value

  def git(*args):
    result = subprocess.run(["git", "-C", str(source), *args], capture_output=True, timeout=30)
    if result.returncode:
      raise RuntimeError("Dependency git provenance unavailable")
    return result.stdout.decode("utf-8", errors="replace").strip()

  def hashes(root, files):
    return {file.relative_to(root).as_posix(): hashlib.sha256(file.read_bytes()).hexdigest()
            for file in sorted(files) if file.is_file()}

  return {
    "dependency_commit": git("rev-parse", "HEAD"),
    "dependency_status": git("status", "--short", "--untracked-files=all"),
    "dependency_source_hashes": hashes(source, [file for file in source.rglob("*")
                                               if file.suffix in (".h", ".cpp", ".c")]),
    "harness_source_hashes": hashes(harness, [file for file in harness.iterdir()
                                              if file.suffix in (".cpp", ".py", ".txt", ".cmd", ".md")]),
    "build_hashes": hashes(build, [cache, build / "observed_cpuexec.cpp", executable.resolve(),
                                    *compiler_files]),
    "cmake": {key: settings.get(key) for key in (
      "CMAKE_GENERATOR", "CMAKE_GENERATOR_PLATFORM", "CMAKE_CXX_COMPILER",
      "CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION")},
    "scope": "External modified local emulator; software evidence only, not hardware certification",
  }
