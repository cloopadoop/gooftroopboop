# Goof Troop Boop

A Windows music editor for Goof Troop's Capcom SNES sequence format.
The desktop application combines a piano roll, tracker, and instrument controls
with a C++ sequence engine and SPC audio renderer.

## Features

- Compose and edit notes, instruments, track settings, and loops.
- Undo and redo edits; save editor sessions as `.gtb` files.
- Play, pause, seek, mute, and solo tracks.
- Import and export SPC, ASM, and MIDI; export WAV and MP3 audio.
- Review ASM instrument mappings before import; apply one mapping to every occurrence of a source program.
- Insert sequence data into a compatible ROM with byte-budget checks.
- Keep over-budget edits as ghost notes for later adjustment.

Use your own compatible input files. ROMs, SPC soundbanks, and audio conversion
executables are not supplied with the source distribution.

## Build and test

See [BUILDING.md](BUILDING.md) for Windows prerequisites, source layout, and
test commands. The desktop frontend is in [studio](studio/README.md).

## Limitations

The sequence format supports straight, dotted, and triplet note durations, not
every integer duration. Drag previews snap to supported lengths and report the
adjustment. Requests that cannot be represented exactly are rejected. A pending
dotted command can further restrict the next event. Supported tied-note edits
preserve their constituent segments. Edits across protected shared offsets or
control-flow boundaries can be rejected without partially changing the song.

ROM export supports the recognized Goof Troop U song loader in 512 KB to 4 MB
LoROM images, with or without a 512-byte copier header. Larger songs can require
ROM expansion. Existing expanded space is preserved, even when it looks empty;
new allocations use newly appended space. A song this editor relocates gets a
block sized for the whole ARAM song window ($0D20-$3FFF, 13,024 bytes), tagged
as the editor's own, so later exports of that slot grow in place instead of
expanding the ROM again. Unsupported loaders and layouts are rejected. Keep an
original backup and test exported ROMs in an emulator.

ASM import is data-only: it does not execute assembler code or import another
game's audio driver or samples. Review the target soundbank and map resolved
instrument values, not the hexadecimal suffixes in alias names. Invalid input
leaves the current song unchanged.
The qualified stock bank's `$17` mute instrument is preserved and identified
as intentional silence in the import dialog.

Data-only ASM import accepts numeric instrument aliases and the recognized
song-table relocation/upload headers, without manually adding dots to `db` or
`dw`. ROM patch directives in those headers are metadata only; importing ASM
does not patch a ROM. Other assembler expressions and includes are rejected.
The Goof Troop driver's two-byte `$1E`/`$1F` no-ops are preserved. Import checks
conditional branches across repeat-counter states. An external address in a
never-taken branch is retained with a warning; it can still prevent structural
edits. Missing initial instruments and unavailable programs require source
correction or explicit instrument mapping, not automatic substitution.

ROM import follows the supported loader's song-table pointers, including
relocated songs. Picker titles identify the original slot, not the title of
custom music in that slot. Preview uses the selected SPC soundbank; modified
drivers or custom ROM sample banks are not automatically imported.

Group moves and loop replacement are single undoable transactions. A failed
operation leaves the song unchanged. Save regularly: an engine crash cannot
recover unsaved song data.

## License and attribution

This is a modified derivative of [VGMTrans](https://github.com/vgmtrans/vgmtrans),
specialized for Capcom SNES music editing. See [LICENSE](LICENSE) for the zlib
license. Third-party components retain their own copyright notices and licenses
in their source directories and `licenses/`.
