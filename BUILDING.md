# Building Goof Troop Boop 0.1.0

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
cmake --build --preset win-x64-release --target gtb-engine GTBoop-cli gtb-model-test
```

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

This produces a desktop executable without an installer. `npm run dev` starts
the read-only browser preview; it does not exercise the C++ sidecar.

## Tests

Python is required for the engine and Rust fault suites. Install the matching
WebView2 driver and `tauri-driver` for native tests, and install Chromium with
`npx playwright install chromium` for browser tests.

- `npm test` in `studio`: browser and component tests.
- `cargo test --manifest-path src-tauri/Cargo.toml --lib` in `studio`: bridge fault tests.
- `npm run test:tauri` or `npm run test:release` in `studio`: real desktop tests.
- `npm run test:all` in `studio`: combined build and test runner.
- `python tests/regression.py` at the root: engine regressions.
- `python tests/stress.py` at the root: seeded model and CLI stress tests.

Supply local fixtures through `GTB_TEST_SPC_CORPUS` (directory of SPC songs),
`GTB_TEST_CORPUS` (directory containing `SPC` and `RAW`), and `GTB_TEST_ROM`
(compatible ROM file). `GTB_TEST_ENGINE` selects the native test sidecar.
Corpus tests deliberately fail when their required fixtures are absent.
The stress CLI currently expects a Debug build at `build/win-x64/bin/Debug`.

Automated checks cover file integrity, editing, transport, dialogs, and child
process failures. They do not replace listening tests or clean-machine
installation tests. Test outputs are local artifacts, not input fixtures.
