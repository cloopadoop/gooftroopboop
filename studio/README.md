# Goof Troop Studio

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

## Running from source

Install dependencies with `npm ci`. `npm run dev` opens a read-only browser
preview. The desktop build requires the C++ engine and Qt Core runtime in
`src-tauri/bin/`, plus your own compatible `soundbank.spc`.
Run `npm run tauri build -- --no-bundle` to build the desktop executable.

## Tests

`npm test` runs browser and component tests. `npm run test:tauri` exercises the
actual desktop application; `npm run test:release` repeats that suite against
the release executable. Native tests require Python, WebView2, `tauri-driver`,
and a matching `msedgedriver`. Supply a local ROM with `GTB_TEST_ROM`.

Save regularly. Unsaved data cannot be recovered after a sidecar crash.
