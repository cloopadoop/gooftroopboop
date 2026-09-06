# Goof Troop Boop

A Windows music editor for Goof Troop's Capcom SNES sequence format.
The desktop application combines a piano roll, tracker, and instrument controls
with a C++ sequence engine and SPC audio renderer.

## Features

- Compose and edit notes, instruments, track settings, and loops.
- Undo and redo edits; save editor sessions as `.gtb` files.
- Play, pause, seek, mute, and solo tracks.
- Import and export SPC, ASM, and MIDI; export WAV and MP3 audio.
- Insert sequence data into a compatible ROM with byte-budget checks.
- Keep over-budget edits as ghost notes for later adjustment.

Use your own compatible input files. ROMs, SPC soundbanks, and audio conversion
executables are not supplied with the source distribution.

## Build and test

See [BUILDING.md](BUILDING.md) for Windows prerequisites, source layout, and
test commands. The desktop frontend is in [studio](studio/README.md).

## Limitations

The sequence format supports a fixed set of note durations; requested lengths
may be quantized. Group moves and loop replacement can require several undo
steps. Save regularly: an engine crash cannot recover unsaved song data.

## License and attribution

This is a modified derivative of [VGMTrans](https://github.com/vgmtrans/vgmtrans),
specialized for Capcom SNES music editing. See [LICENSE](LICENSE) for the zlib
license. Third-party components retain their own copyright notices and licenses
in their source directories and `licenses/`.
