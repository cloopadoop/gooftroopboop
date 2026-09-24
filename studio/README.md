# Goof Troop Studio

### Track and channel numbers

The channel list shows both the stable editor track number and its ROM/ASM
channel: Track 1 is CH 7, Track 2 is CH 6, through Track 8 being CH 0.
Existing sessions, selections, mute/solo controls and track colors keep their
original track numbering. Use CH when comparing a song with its ASM header.

Group moves and loop resizing each use one Undo operation. If any part of a
group move or loop replacement fails, the entire operation is cancelled.

Songs loaded from a ROM or session can contain unlisted instrument numbers.
The channel list warns about these references without silently replacing them.
Session save/reopen preserves them. ASM import separately checks the target
sound bank and may require an explicit mapping. A successful render alone does
not prove that an unlisted instrument matches the original arrangement.

The desktop interface for Goof Troop Boop: a piano-roll music editor with
tracker, instrument inspector, playback controls, and file conversion menus.

## Editing

Open a compatible SPC or saved `.gtb` session. Select a track, then add or
select notes in the piano roll. The inspector changes note and track settings.
Use Ctrl+Z to undo and Ctrl+Shift+Z to redo. Save as `.gtb` to retain editor
extras such as ghost notes; export SPC for the game-compatible sequence.

The budget meter shows the sequence allocation. Ghost notes represent edits
that could not fit and must be adjusted before they become part of the song.
Use playback, mute, and solo to audition your changes before exporting.

## Repairing extracted ASM

Import ASM to review its instrument mappings. If the file has damaged command
boundaries or addresses, choose **Compare with source ROM** (or **Choose source
ROM** when structural validation fails). Select the original game's local ROM.
The editor proposes changes only when it can establish source-byte evidence.

Review the displayed byte changes, then review the instruments before importing
the repaired copy. Cancel leaves the open song unchanged. Original ASM and ROM
files are never overwritten or uploaded. A source match does not prove musical
fidelity: audition the imported result and save it under a new filename.

Older Capcom driver variants have different command semantics and are not
eligible for this repair profile. An unknown or ambiguous source remains
rejected rather than being guessed.

## Running from source

Install dependencies with `npm ci`. `npm run dev` opens a read-only browser
preview. The desktop build requires the C++ engine, Qt Core and the VC++
runtime DLLs in `src-tauri/bin/` (`populate-sidecars.cmd` copies exactly that
set), plus your own compatible `soundbank.spc`.
Run `npm run tauri build -- --no-bundle` to build the desktop executable.

## Tests

`npm test` runs browser and component tests. `npm run test:tauri` exercises the
actual desktop application; `npm run test:release` repeats that suite against
the release executable. Native tests require Python, WebView2, `tauri-driver`,
and a matching `msedgedriver`. Supply a local ROM with `GTB_TEST_ROM`.

Save regularly. Unsaved data cannot be recovered after a sidecar crash.
