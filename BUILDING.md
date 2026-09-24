# Building Goof Troop Boop

## Windows prerequisites

Use an x64 MSVC developer command prompt with CMake 3.21 or newer, Ninja,
Qt 6 Core for MSVC, Node.js with npm, Rust's MSVC toolchain, and WebView2.
Set `CMAKE_PREFIX_PATH` to your Qt installation. The headless engine does not
need Qt Widgets or BASS. The legacy widget application is not part of this build.

The source tree contains the C++ engine at the root and the Tauri application
under `studio/`. For a source snapshot, obtain `lib/spdlog` and `lib/zlib` from
the public URLs and exact commits listed in `PUBLIC_DEPENDENCIES.json`.
Do not substitute a dependency's latest branch for its pinned commit.

```cmd
cmake --preset win-x64 -DENABLE_UI_QT=OFF
cmake --build --preset win-x64-release --target gtb-engine gtb-driver-profile-test GTBoop-cli gtb-model-test
```

Release executables go to `build/win-x64/bin/` and Debug executables to
`build/win-x64/bin/Debug/`, so the two configurations never overwrite each other.

After configuring, the wrapper scripts rebuild without a developer prompt:
`build-engine.bat` (engine + driver-profile test, Release), `build-modeltest.bat`,
`build-all.bat` (Debug), `build-all-release.bat`, and `build-cli.bat`. They
share `tools/vs-env.cmd`, which does four things:

- It loads the Visual Studio install the CMake cache was configured with, so the
  environment and the cached compiler come from one toolset.
- It writes the vcvars output to `build/vs-env-vcvars.log`.
- It fails if `cl.exe` is unusable.
- It calls the cache's own `cmake.exe`, so a different `cmake` earlier on
  `PATH` is never used. Each script writes a log named after itself.

Before building Tauri, create `studio/src-tauri/bin/` and place the built
`gtb-engine.exe` and its matching Qt Core runtime there. Supply a compatible,
lawfully obtained SPC as `soundbank.spc` for the default soundbank. MP3 export
requires FFmpeg; obtain it separately and follow its distribution license.
Do not mix debug Qt runtimes with release executables.

```cmd
cd studio
npm ci
npm run tauri build -- --no-bundle
```

This produces a desktop executable without an installer. For a build you plan
to share, run `build-release.cmd` in `studio` instead: it does the same build
but keeps your local user-profile paths out of the executable. `npm run dev` starts
the read-only browser preview; it does not exercise the C++ sidecar.

The Rust release profile in `studio/src-tauri/Cargo.toml` turns off LTO and
uses 16 codegen units at `opt-level = 1`. With the stock thin-LTO profile, the
release build ran out of memory in LLVM on the development machine. The Rust
side is a thin shell around the sidecar, so the size and speed cost is small.

## Tests

Python is required for the engine and Rust fault suites. Install the matching
WebView2 driver and `tauri-driver` for native tests, and install Chromium with
`npx playwright install chromium` for browser tests.

- `npm test` in `studio`: browser and component tests.
- `cargo test --manifest-path src-tauri/Cargo.toml --lib` in `studio`: bridge fault tests.
- `npm run test:tauri` or `npm run test:release` in `studio`: real desktop tests.
- `npm run test:all` in `studio`: combined build and test runner.
- `run-tests.cmd` at the root: the whole engine lane. It runs the regression
  suite, the driver-profile test, the ASM tests (both modes), source repair,
  and every `tests/test_*.py`. It logs to `tests/run-tests.log`. It uses
  `GTB_FFMPEG` if set, otherwise the studio's sidecar `ffmpeg.exe`, otherwise
  `PATH`.
- `python tests/regression.py` at the root: engine regressions only.
- `python -m unittest discover -s tests -p "test_*.py"` at the root: focused format, ROM-safety, and audio-metric tests.
- `python tests/stress.py` at the root: seeded model and CLI stress tests.

`npm run test:qualify` in `studio` runs the combined suite, the release desktop
suite, the seeded stress campaign, and independent CPU playback checks. It
requires `SNSF9X_SOURCE` and `GTB_CPU_PLAYBACK_MANIFEST`; see
[CPU playback tests](tests/emulator/README.md) for the emulator dependency and
private fixture-manifest contract. Generate fixtures from the current engine
build before qualification. This command only builds and tests locally.

Supply local fixtures through `GTB_TEST_SPC_CORPUS` (directory of SPC songs),
`GTB_TEST_CORPUS` (directory containing `SPC` and `RAW`), and `GTB_TEST_ROM`
(compatible ROM file). `GTB_TEST_ENGINE` selects the native test sidecar.
Corpus tests deliberately fail when their required fixtures are absent.
The stress CLI currently expects a Debug build at `build/win-x64/bin/Debug`.

Automated checks cover file integrity, editing, transport, dialogs, and child
process failures. They do not replace listening tests or clean-machine
installation tests. Test outputs are local artifacts, not input fixtures.

For an additional local music corpus, run `python tests/qualify_music_pack.py --help`.
It accepts an ASM ZIP, a compatible SPC bank, optional ROMs, and an
explicit report path in a Git-ignored directory. The archive is read as data
without extracting or executing its contents. Accepted songs undergo instrument
edit/undo/redo, session round-trip, and post-startup audio checks. Rejected
imports must preserve the previous session. A passing run qualifies those
invariants, not every song's musical correctness or every rejected input.
Exit code 2 means the run completed with explicitly reported unsupported imports
or edits; it is not a full pass. Unexpected failures exit nonzero too.

### Source repair and reference audio tests

Set `GTB_SOUNDBANK` to a compatible local SPC and run
`python tests/source_repair_regression.py`. This generates synthetic source data
and tests repair proposals, ambiguous/missing sources, unmapped instruments,
and preservation of the current song. `gtb-model-test --legacy-noops` checks
operand consumption and the distinction between older and newer drivers.

For a second emulator execution path, use an x64 MSVC developer prompt and run
`tests\build-reference.cmd PATH_TO_SNES9X_SOURCE`. The external source must have
the BAPU `apu/bapu/smp/smp.cpp` and `apu/bapu/dsp/sdsp.cpp` layout. No emulator
source or game data is bundled or downloaded. Build logs remain in `build/`.
Install NumPy and SciPy, then run:

```cmd
python tests/reference_playback.py --engine build/win-x64/bin/gtb-engine.exe --reference-exe build/reference-apu/reference-apu.exe --bank LOCAL_BANK.spc --asm-dir LOCAL_ASM_DIRECTORY
```

This compares thirty-second recordings from separate SPC700 CPU implementations,
with a common initial timer-phase convention and a fixed output-filter model.
It permits one constant start offset of at most one millisecond, not time
stretching. Both stereo channels must exceed 0.999 waveform correlation and
remain within 0.1 dB RMS. Silence, missing channels, and missing prerequisites
fail. The comparison gate has synthetic positive and negative controls in
`tests/test_reference_playback.py`; that test also requires NumPy and SciPy.

SPC snapshots omit timer prescaler phase; matching that initialization is
necessary for a meaningful comparison. The DSP implementations share ancestry,
so this is not independent DSP or hardware certification. It also does not
establish the intended arrangement or instrument choices of imported music.
Use the separate CPU playback tests above for the full ROM-loading path.
