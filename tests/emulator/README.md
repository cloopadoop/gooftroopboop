# CPU playback qualification

Independent of the application engine: boot a user-supplied unheadered ROM from reset in an
external local snsf9x/Snes9x-derived CPU/APU core, collect signed 16-bit stereo 32 kHz WAV,
and observe actual CPU loader entry, block acceptance, and return. No ROM, emulator source,
PCM, or game payload is included here. No SPC state is injected and no ROM code is patched.

## Build

Set `SNSF9X_SOURCE` to the external folder containing `SNESSystem.cpp` and `snes9x/`.
The supported source is [snsf9x](https://github.com/loveemu/snsf9x) at commit
`128cb0d500c98caa815da117eb0339873cf157ae`; the directory is its `snsf9x/` subfolder.
The emulator is not vendored here; obtain it separately and retain its license notices.
Qualification fingerprints the actual local sources rather than assuming a clean checkout.
Use local Visual Studio with C++ and CMake installed. Set `CMAKE_EXE` if CMake is not on PATH,
then run `build.cmd`. Build products and logs are under ignored `build/`.
Only one CPU translation unit is generated in that directory; an exact source anchor must
exist before CMake adds a read-only pre-opcode observation callback. All other source files
compile directly from the external path. The external source is never edited.

This target supports Windows x64. The legacy core misses MSVC `_M_X64` in its endian detection;
the target defines the recognized `__x86_64__` spelling. Defining `LSB_FIRST` alone leaves a
conflicting `MSB_FIRST` definition for the APU. Channel mask zero is applied explicitly because
the external wrapper initializes its cached mask to zero and would otherwise skip it.

## Run

Create a private manifest, then run:

```text
python qualify.py PRIVATE_MANIFEST.json --out PRIVATE_EMPTY_OUTPUT_DIRECTORY
```

ROM paths are relative to the current working directory, or absolute, with environment
variable expansion. The output directory must be empty. Example schema (supply your own
verified addresses, song slot, and fixtures):

```json
{
  "frames": 600,
  "watches": {"entry": "HEX_PC", "block": "HEX_PC", "return": "HEX_PC"},
  "cases": [
    {"name": "baseline", "rom": "PRIVATE_BASELINE", "slot": 16, "expect": "audible"},
    {"name": "relocated", "rom": "PRIVATE_RELOCATED", "slot": 16,
     "source_bank": 148, "expect": "audible", "pcm_equal": "baseline"},
    {"name": "larger", "rom": "PRIVATE_LARGER", "slot": 16,
     "expect": "audible", "pcm_different": "baseline"},
    {"name": "broken", "rom": "PRIVATE_MISSING_TERMINATOR", "slot": 16,
     "expect": "loader-rejected"},
    {"name": "muted", "rom": "PRIVATE_BASELINE", "slot": 16,
     "expect": "silent", "channel_mask": 0}
  ]
}
```

The configured entry must expose song ID in A.low. The block observation must occur after
header decode, with X=length and DP source pointer in WRAM `$10..$12`, Y=offset. These are
game-loader-specific conventions, not a generic SNES API. CSV values other than frame and
instruction are hexadecimal. Addresses and fixture provenance must be independently verified.

`report.json` separates PCM audibility, exact PCM equality, and loader success. Success requires
the selected song to reach the loader, accept a nonempty block, accept a zero-length final
block, and return. Defect-control success requires reaching the selected loader and failing
its completion checks; process crashes and timeouts fail the suite. A muted control must still
complete the loader and emit exactly zero-valued PCM. Each process has a 90-second timeout,
500-million-instruction limit, and one-million-trace-row limit; frame count is bounded to 3600.

`provenance.json` records external git commit and dirty status, hashes of local dependency
sources/headers, harness sources, CMake cache, generated CPU hook, executable, and generator
details. Reports use anonymous case labels; external stdout/stderr is not copied because it
may contain ROM metadata. Build logs and provenance can contain private local paths: keep all
generated evidence private. There is no automatic download or publication.

## Evidence limits

This is actual game CPU execution in a locally modified, stripped emulator core. It is not
hardware qualification, a certified emulator build, a full graphical gameplay test, or proof
for any ROM/hack not supplied and exercised. A larger song producing different non-silent PCM
proves playback changed; it does not prove musical fidelity to an external reference. Exact
PCM equality is strict and can fail for valid relocations that alter cycle timing. The
channel-mask control tests audio detection, not the game's ability to handle a silent song.
