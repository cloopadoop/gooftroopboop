import "./style.css";
import { makeEngine, type Engine, type SongState } from "./engine";
import { PianoRoll, TRACK_COLORS } from "./pianoroll";
import { AudioPlayer } from "./audio";

const engine: Engine = makeEngine();
const audio = new AudioPlayer();
let state: SongState | null = null;
let activeTrack = 0;
let playheadTick = 0;               // last known playhead position in ticks
let lastDragPreview = 0;            // throttle for audible drag ghosts
let dirty = false;                  // unsaved changes since last save/open/new
let lastPath: string | null = null; // for engine-crash recovery

function markDirty() {
  if (!dirty) { dirty = true; updateWindowTitle(); }
}
function clearDirty() {
  dirty = false; updateWindowTitle();
}
function updateWindowTitle() {
  const name = ($("song-title")?.textContent ?? "").trim() || "Goof Troop Boop";
  const t = `${dirty ? "• " : ""}${name} — Goof Troop Boop`;
  document.title = t;
  try { (window as any).__TAURI__?.window?.getCurrentWindow?.().setTitle(t); } catch { /* browser mode */ }
}
let clipboard: { track: number; tick: number; pitch: number; len: number }[] = [];
let eyedropOn = false;              // instrument-match tool armed
let eyedropRefs: { track: number; note: number }[] = [];

// Ghost notes: edits the engine rejected (over budget / no room) stay visible
// as red outlines with the reason, instead of vanishing. Legalize retries
// them after reclaiming bytes, then offers to discard what still won't fit.
export interface GhostNote { track: number; tick: number; pitch: number; len: number; reason: string; }
let ghosts: GhostNote[] = [];

// Discarded ghosts are restorable: Ctrl+Z restores the most recent discard
// batch if no engine edit happened after it.
let ghostTrash: GhostNote[][] = [];
let editsSinceDiscard = 0;

function discardGhosts(batch: GhostNote[]) {
  if (!batch.length) return;
  markDirty();
  ghostTrash.push(batch);
  editsSinceDiscard = 0;
}

function addGhost(g: GhostNote) {
  markDirty();
  ghosts.push(g);
  roll.setGhosts(ghosts);
  status(`No room — kept as a ghost (${g.reason}). ✨ Legalize retries it.`);
}

async function legalize() {
  if (engine.readOnly) return;
  try {
    const resp = await engine.request("optimize", { merge: true });
    state = resp.state as typeof state;
    const freed = (resp.bytesBefore as number) - (resp.bytesAfter as number);
    const mergedN = (resp.mergedTracks as number) ?? 0;
    if (freed || mergedN) { editsSinceDiscard++; markDirty(); }
    // retry ghosts now that bytes may be free
    const remaining: GhostNote[] = [];
    let placed = 0;
    for (const g of ghosts) {
      try {
        const r = await engine.request("insertNote", { track: g.track, tick: g.tick, pitch: g.pitch, len: g.len });
        state = r.state as typeof state;
        placed++;
        editsSinceDiscard++; markDirty();
      } catch { remaining.push(g); }
    }
    ghosts = remaining;
    roll.setGhosts(ghosts);
    renderDirty = true; render();
    const parts: string[] = [];
    if (freed > 0) parts.push(`reclaimed ${freed} bytes`);
    if (mergedN > 0) parts.push(`merged ${mergedN} track${mergedN > 1 ? "s" : ""}`);
    if (placed > 0) parts.push(`placed ${placed} ghost note${placed > 1 ? "s" : ""}`);
    if (ghosts.length && confirm(`${ghosts.length} note(s) still don't fit the song's byte budget. Discard them? (Ctrl+Z restores)`)) {
      discardGhosts(ghosts);
      ghosts = [];
      roll.setGhosts(ghosts);
      parts.push("discarded the rest");
    } else if (ghosts.length) {
      parts.push(`${ghosts.length} ghost(s) kept`);
    }
    status(parts.length ? `Legalize: ${parts.join(", ")}` : "Already optimal — nothing to do");
  } catch (e) {
    status("Optimize failed: " + (e as Error).message);
  }
}

// Paste copied notes at the playhead, preserving relative timing and tracks.
async function pasteClipboard() {
  if (engine.readOnly || !clipboard.length || !state) return;
  editsSinceDiscard++; markDirty();
  const minTick = Math.min(...clipboard.map((n) => n.tick));
  const base = Math.max(0, Math.round(playheadTick / 12) * 12);
  let placed = 0;
  for (const n of [...clipboard].sort((a, b) => a.tick - b.tick)) {
    try {
      const resp = await engine.request("insertNote", {
        track: n.track, tick: base + (n.tick - minTick), pitch: n.pitch, len: n.len,
      });
      state = resp.state as typeof state;
      placed++;
    } catch (e) {
      addGhost({ track: n.track, tick: base + (n.tick - minTick), pitch: n.pitch, len: n.len, reason: (e as Error).message });
    }
  }
  renderDirty = true; render();
  status(`Pasted ${placed} note${placed > 1 ? "s" : ""} at the playhead${ghosts.length ? ` (${ghosts.length} ghost${ghosts.length > 1 ? "s" : ""})` : ""}`);
}

async function refreshState() {
  try {
    const resp = await engine.request("state", {});
    state = resp.state as typeof state;
    renderDirty = true; render();
  } catch { /* no file open */ }
}
const hidden = new Set<number>();   // hidden on the roll (visual)
const muted = new Set<number>();    // muted in playback (audio)
const soloed = new Set<number>();   // soloed in playback (mutes everything else)
let selected: { track: number; note: number } | null = null;
let renderDirty = true;             // song changed since last audio render
let addMode = false;

const $ = (id: string) => document.getElementById(id)!;

function ticksPerSec(): number {
  const bpm = state?.tempoBpm ?? 120;
  const ppqn = state?.ppqn ?? 48;
  return (ppqn * bpm) / 60;
}

let pendingSeekSec = 0;  // seek target set before the first audio render

const roll = new PianoRoll($("roll") as HTMLCanvasElement, {
  onSelectNote(track, note) {
    if (eyedropOn) {
      // instrument-match: copy this note's program onto the armed selection
      eyedropOn = false;
      $("btn-eyedrop").classList.remove("on");
      const src = state?.tracks.find((t) => t.index === track)?.notes.find((n) => n.i === note);
      const refs = eyedropRefs; eyedropRefs = [];
      if (src && refs.length) {
        const name = state?.programs.find((p) => p.program === src.program)?.name ?? `program ${src.program}`;
        edit("setInstrument", { notes: refs, program: src.program })
          .then(() => status(`Applied ${name} to ${refs.length} note${refs.length > 1 ? "s" : ""}`));
      }
      return;
    }
    selected = { track, note };
    if (track !== activeTrack) { activeTrack = track; render(); }
    else renderInspector();
  },
  async onAddNote(track, tick, pitch, len) {
    if (engine.readOnly) return;
    try {
      editsSinceDiscard++; markDirty();
      const resp = await engine.request("insertNote", { track, tick, pitch, len });
      state = resp.state as typeof state;
      renderDirty = true; render();
      const added = state?.tracks.find((t) => t.index === track)?.notes.find((n) => !n.rest && n.tick === tick);
      if (added) previewNote(track, added.i);
    } catch (e) {
      addGhost({ track, tick, pitch, len, reason: (e as Error).message });
    }
  },
  async onMoveNote(track, note, tick, pitch) {
    if (engine.readOnly) return;
    // placeNote relocates across tracks when the drop spot is occupied
    try {
      editsSinceDiscard++; markDirty();
      const resp = await engine.request("placeNote", { track, note, tick, pitch });
      state = resp.state as typeof state;
      renderDirty = true;
      const newTrack = (resp.movedTrack as number) ?? track;
      if (newTrack !== track) {
        activeTrack = newTrack;
        status(`Note moved to track ${newTrack + 1} (spot on track ${track + 1} was occupied)`);
      }
      render();
      // replay the note at its new position
      const tr = state?.tracks.find((t) => t.index === newTrack);
      const moved = tr?.notes.find((n) => !n.rest && n.tick === tick && n.pitch === pitch)
        ?? tr?.notes.find((n) => !n.rest && n.tick === tick);
      if (moved) previewNote(newTrack, moved.i);
    } catch (e) {
      // keep the intended drop visible as a ghost instead of losing it
      const n = state?.tracks.find((t) => t.index === track)?.notes.find((x) => x.i === note);
      addGhost({ track, tick, pitch, len: n?.len ?? 24, reason: (e as Error).message });
    }
  },
  async onMoveNotes(items, dTick, dPitch) {
    if (engine.readOnly) return;
    // Group move: relocate one note at a time, re-resolving indices from the
    // fresh state after each edit. Order matters so group members don't
    // collide with each other mid-move: right-to-left when moving later,
    // left-to-right when moving earlier.
    const ordered = [...items].sort((a, b) => (dTick > 0 ? b.tick - a.tick : a.tick - b.tick));
    let moved = 0;
    for (const it of ordered) {
      const tr = state?.tracks.find((t) => t.index === it.track);
      const n = tr?.notes.find((x) => !x.rest && x.tick === it.tick && x.pitch === it.pitch);
      if (!n) continue;
      try {
        const resp = await engine.request("placeNote", {
          track: it.track, note: n.i,
          tick: Math.max(0, it.tick + dTick),
          pitch: Math.max(0, Math.min(127, it.pitch + dPitch)),
        });
        state = resp.state as typeof state;
        moved++;
        editsSinceDiscard++; markDirty();
      } catch (e) {
        status(`Moved ${moved}/${items.length} notes, then: ${(e as Error).message}`);
        renderDirty = true; render();
        return;
      }
    }
    renderDirty = true; render();
    status(`Moved ${moved} notes`);
  },
  onResizeNote(track, note, len) {
    const n = state?.tracks.find((t) => t.index === track)?.notes.find((x) => x.i === note);
    if (!n) return;
    edit("resizeNote", { track, note, len }).then(() => {
      const resized = state?.tracks.find((t) => t.index === track)?.notes.find((x) => x.tick === n.tick && !x.rest);
      if (resized) previewNote(track, resized.i);
    });
  },
  onPreview(track, note) { previewNote(track, note); },
  onPreviewEnd() { audio.stopPreview(); },
  onDragPreview(track, pitch) {
    // audible ghost while dragging: throttle so we don't queue a render per pixel
    const now = performance.now();
    if (now - lastDragPreview < 160) return;
    lastDragPreview = now;
    const tr = state?.tracks.find((t) => t.index === track);
    const program = tr?.notes.find((n) => !n.rest)?.program ?? 1;
    engine.request("previewKey", { pitch, program })
      .then((resp) => audio.preview(engine.wavUrl(resp.wav as string)))
      .catch(() => { /* best-effort */ });
  },
  async onLoopResize(track, loop, startTick, endTick, count) {
    if (engine.readOnly) return;
    // no in-place loop retarget in the engine: replace it atomically-ish
    let removed = false;
    try {
      const lp = state?.tracks.find((t) => t.index === track)?.loops[loop];
      const slot = (lp as { slot?: number } | undefined)?.slot ?? 0;
      await engine.request("removeLoop", { track, loop });
      removed = true;
      const resp = await engine.request("createLoop", { track, startTick, endTick, slot, count });
      state = resp.state as typeof state;
      editsSinceDiscard++; markDirty();
      renderDirty = true; render();
      status(`Loop now spans ticks ${startTick}–${endTick} (×${count})`);
    } catch (e) {
      if (removed) await engine.request("undo");
      status("Loop resize failed: " + (e as Error).message);
      refreshState();
    }
  },
  onCreateLoop(track, startTick, endTick) { edit("createLoop", { track, startTick, endTick, count: 2 }); },
  onRemoveGhost(index) {
    discardGhosts(ghosts.splice(index, 1));
    roll.setGhosts(ghosts);
    status("Ghost note discarded (Ctrl+Z restores)");
  },
  onSelectSetting(track, setting) {
    // marker clicked on the ruler: reveal and flash its row in the inspector
    if (track !== activeTrack) { activeTrack = track; render(); }
    const row = document.querySelector(`[data-set-row="${setting}"]`);
    if (row) {
      row.scrollIntoView({ block: "center", behavior: "smooth" });
      row.classList.add("flash");
      setTimeout(() => row.classList.remove("flash"), 1600);
      const s = state?.tracks.find((t) => t.index === track)?.settings.find((x) => x.i === setting);
      if (s) status(`${s.type} at tick ${s.tick} — edit its value in the panel on the right`);
    }
  },
  onSeek(tick, andPlay) {
    playheadTick = tick;
    const sec = tick / ticksPerSec();
    if (audio.duration > 0) {
      audio.seek(Math.min(sec, audio.duration));
    } else {
      pendingSeekSec = sec;
      roll.setPlayhead(tick);
    }
    if (andPlay) startPlayback(sec);
  },
  async onKeyPreview(pitch) {
    if (!engine.canPlay) return;
    // use the active track's instrument at the playhead (tracks can switch
    // programs mid-song, so the first note's program isn't always right)
    const tr = state?.tracks.find((t) => t.index === activeTrack);
    const sounding = tr?.notes.filter((n) => !n.rest && n.tick <= playheadTick).pop()
      ?? tr?.notes.find((n) => !n.rest);
    const program = sounding?.program ?? 1;
    try {
      const resp = await engine.request("previewKey", { pitch, program });
      audio.preview(engine.wavUrl(resp.wav as string));
    } catch (e) { status("Key preview failed: " + (e as Error).message); }
  },
});

async function boot() {
  try {
    state = await engine.open("");
    activeTrack = state.tracks.find((t) => t.noteCount > 0)?.index ?? 0;
    render();
    status(engine.readOnly ? "Preview mode (read-only) — launch the app for editing & playback" : "Ready");
  } catch (e) {
    $("song-title").textContent = "Failed to load: " + (e as Error).message;
  }
  wireGlobal();
}

function render() {
  if (!state) return;
  $("song-title").textContent = state.source + (engine.readOnly ? "  ·  preview (read-only)" : "");
  updateWindowTitle();
  renderBudget();
  renderChannels();
  roll.setState(state, activeTrack, hidden);
  renderInspector();
  renderTracker();
  updateTransport();
}

function renderBudget() {
  const b = state?.budget, el = $("budget");
  if (!b) { el.textContent = ""; return; }
  const pct = Math.round((100 * b.used) / b.total);
  el.textContent = `${b.used} / ${b.total} bytes · ${pct}%`;
  el.className = "budget" + (b.used > b.total ? " bad" : pct >= 90 ? " warn" : "");
}

function renderChannels() {
  const el = $("channels");
  el.innerHTML = "<h2>Channels</h2>";
  for (const tr of state!.tracks) {
    const color = TRACK_COLORS[tr.index % TRACK_COLORS.length];
    const div = document.createElement("div");
    div.className = "chan" + (tr.index === activeTrack ? " active" : "") + (hidden.has(tr.index) ? " hidden" : "");
    const label = tr.noteCount > 0 ? `${tr.instrument || "—"}` : "(no notes)";
    div.innerHTML = `
      <span class="swatch" style="background:${color}"></span>
      <div class="meta">
        <div class="name">Track ${tr.index + 1}</div>
        <div class="sub">${escapeHtml(label)}${tr.noteCount ? " · " + tr.noteCount + " notes" : ""}</div>
      </div>
      <div class="chan-btns">
        <button class="mini mute ${muted.has(tr.index) ? "on" : ""}" data-act="mute" title="Mute in playback">M</button>
        <button class="mini solo ${soloed.has(tr.index) ? "on" : ""}" data-act="solo" title="Solo in playback">S</button>
        <button class="mini eye" data-act="eye" title="Hide on roll">${hidden.has(tr.index) ? "🚫" : "👁"}</button>
      </div>`;
    div.addEventListener("click", (ev) => {
      const act = (ev.target as HTMLElement).closest("[data-act]")?.getAttribute("data-act");
      if (act === "eye") {
        hidden.has(tr.index) ? hidden.delete(tr.index) : hidden.add(tr.index);
      } else if (act === "mute") {
        muted.has(tr.index) ? muted.delete(tr.index) : muted.add(tr.index);
        renderDirty = true;
        if (audio.isPlaying) startPlayback(audio.current);
      } else if (act === "solo") {
        soloed.has(tr.index) ? soloed.delete(tr.index) : soloed.add(tr.index);
        renderDirty = true;
        if (audio.isPlaying) startPlayback(audio.current);
      } else {
        activeTrack = tr.index; selected = null;
      }
      render();
    });
    el.appendChild(div);
  }
}

function renderInspector() {
  const el = $("inspector");
  const trackPanel = trackPanelHtml();
  if (!selected || !state) {
    el.innerHTML = '<div class="inspector-empty">Select a note to edit</div>' + trackPanel;
    wireTrackPanel();
    return;
  }
  const tr = state.tracks.find((t) => t.index === selected!.track);
  const n = tr?.notes.find((x) => x.i === selected!.note);
  if (!n) {
    el.innerHTML = '<div class="inspector-empty">Select a note to edit</div>' + trackPanel;
    wireTrackPanel();
    return;
  }
  const progOpts = state.programs
    .map((p) => `<option value="${p.program}" ${p.program === n.program ? "selected" : ""}>${p.program}: ${escapeHtml(p.name)}</option>`)
    .join("");
  const ro = engine.readOnly ? "disabled" : "";
  el.innerHTML = `
    <h2>Note</h2>
    <div class="field"><label>Pitch (MIDI)</label><input id="in-pitch" type="number" min="0" max="127" value="${n.pitch}" ${ro}></div>
    <div class="field"><label>Length (ticks)</label><input id="in-len" type="number" min="1" value="${n.len}" ${ro}></div>
    <div class="field"><label>Instrument</label><select id="in-prog" ${ro}>${progOpts}</select></div>
    <div class="field"><label>Position</label><input type="text" value="tick ${n.tick} · track ${tr!.index + 1}" disabled></div>
    <div class="row-btns">
      <button id="btn-note-prev">Preview ▶</button>
      <button id="btn-note-rest" ${ro}>Make rest</button>
      <button id="btn-note-del" class="danger" ${ro}>Delete</button>
    </div>` + trackPanel;
  wireTrackPanel();
  $("btn-note-prev").addEventListener("click", () => previewNote(selected!.track, selected!.note));
  if (!engine.readOnly) {
    const apply = async (rest = false) => {
      const pitch = +(($("in-pitch") as HTMLInputElement).value);
      const len = +(($("in-len") as HTMLInputElement).value);
      await edit("setNote", { track: selected!.track, note: selected!.note, pitch, len, rest });
    };
    $("in-pitch").addEventListener("change", () => apply());
    $("in-len").addEventListener("change", () => apply());
    $("in-prog").addEventListener("change", async () => {
      const program = +(($("in-prog") as HTMLSelectElement).value);
      await edit("setInstrument", { notes: [{ track: selected!.track, note: selected!.note }], program });
    });
    $("btn-note-rest").addEventListener("click", () => apply(true));
    $("btn-note-del").addEventListener("click", async () => {
      await edit("eraseNote", { track: selected!.track, note: selected!.note });
      selected = null; renderInspector();
    });
  }
}

// Settings & loops of the active track (engine: addSetting/removeSetting/
// createLoop/updateLoopCount/removeLoop).
const SETTING_TYPES = ["tempo", "volume", "pan", "duration", "octave", "transpose", "lfo", "echo", "release"];

function trackPanelHtml(): string {
  if (!state) return "";
  const tr = state.tracks.find((t) => t.index === activeTrack);
  if (!tr) return "";
  const ro = engine.readOnly ? "disabled" : "";
  const settings = tr.settings.map((s) =>
    `<div class="evt-row" data-set-row="${s.i}"><span class="evt-desc" title="${escapeHtml(s.desc)}">${escapeHtml(s.type)} <em>at tick ${s.tick}</em></span>
     <input type="number" class="evt-count" data-set-value="${s.i}" min="0" max="65535" value="${s.value}" title="Value" ${ro}>
     ${s.value2 ? `<input type="number" class="evt-count" data-set-value2="${s.i}" min="0" max="255" value="${s.value2}" title="Second value" ${ro}>` : ""}
     <button class="mini danger" data-del-setting="${s.i}" title="Remove this setting" ${ro}>✕</button></div>`).join("");
  const loops = tr.loops.map((l) =>
    `<div class="evt-row"><span class="evt-desc">ticks ${l.destTick} – ${l.tick}, repeats</span>
     <input type="number" class="evt-count" data-loop-count="${l.i}" min="1" max="255" value="${l.count}" title="Repeat count" ${ro}>
     <button class="mini danger" data-del-loop="${l.i}" title="Remove this loop" ${ro}>✕</button></div>`).join("");
  return `
    <h2 style="margin-top:18px">Track ${activeTrack + 1} settings</h2>
    <div class="evt-list">${settings || '<div class="evt-empty">none</div>'}</div>
    <div class="add-row">
      <select id="add-setting-type" ${ro}>${SETTING_TYPES.map((t) => `<option>${t}</option>`).join("")}</select>
      <input id="add-setting-tick" type="number" min="0" placeholder="tick" ${ro}>
      <input id="add-setting-val" type="number" placeholder="value" ${ro}>
      <button id="btn-add-setting" ${ro}>+</button>
    </div>
    <h2 style="margin-top:18px">Loops</h2>
    <div class="evt-list">${loops || '<div class="evt-empty">none</div>'}</div>
    <div class="add-row">
      <input id="add-loop-start" type="number" min="0" placeholder="start tick" ${ro}>
      <input id="add-loop-end" type="number" min="0" placeholder="end tick" ${ro}>
      <input id="add-loop-count" type="number" min="1" max="255" value="2" ${ro}>
      <button id="btn-add-loop" ${ro}>+</button>
    </div>`;
}

function wireTrackPanel() {
  if (engine.readOnly || !state) return;
  const el = $("inspector");
  el.querySelectorAll("[data-del-setting]").forEach((b) =>
    b.addEventListener("click", () => edit("removeSetting", { track: activeTrack, setting: +b.getAttribute("data-del-setting")! })));
  // editable setting values (tempo/lfo/echo/volume/...)
  const settingChange = (inp: Element, key: "value" | "value2") => {
    const si = +(inp.getAttribute(`data-set-${key === "value" ? "value" : "value2"}`))!;
    const s = state?.tracks.find((t) => t.index === activeTrack)?.settings.find((x) => x.i === si);
    if (!s) return;
    const v = +(inp as HTMLInputElement).value;
    edit("updateSetting", {
      track: activeTrack, setting: si, type: s.type,
      value: key === "value" ? v : s.value,
      value2: key === "value2" ? v : (s.value2 ?? 0),
    });
  };
  el.querySelectorAll("[data-set-value]").forEach((inp) =>
    inp.addEventListener("change", () => settingChange(inp, "value")));
  el.querySelectorAll("[data-set-value2]").forEach((inp) =>
    inp.addEventListener("change", () => settingChange(inp, "value2")));
  el.querySelectorAll("[data-del-loop]").forEach((b) =>
    b.addEventListener("click", () => edit("removeLoop", { track: activeTrack, loop: +b.getAttribute("data-del-loop")! })));
  el.querySelectorAll("[data-loop-count]").forEach((inp) =>
    inp.addEventListener("change", () =>
      edit("updateLoopCount", { track: activeTrack, loop: +inp.getAttribute("data-loop-count")!, count: +(inp as HTMLInputElement).value })));
  document.getElementById("btn-add-setting")?.addEventListener("click", () => {
    const type = ($("add-setting-type") as HTMLSelectElement).value;
    const tick = +(($("add-setting-tick") as HTMLInputElement).value || 0);
    const value = +(($("add-setting-val") as HTMLInputElement).value || 0);
    edit("addSetting", { track: activeTrack, tick, type, value });
  });
  document.getElementById("btn-add-loop")?.addEventListener("click", () => {
    const startTick = +(($("add-loop-start") as HTMLInputElement).value || 0);
    const endTick = +(($("add-loop-end") as HTMLInputElement).value || 0);
    const count = +(($("add-loop-count") as HTMLInputElement).value || 2);
    if (endTick > startTick) edit("createLoop", { track: activeTrack, startTick, endTick, count });
    else status("Loop end tick must be after start tick");
  });
}

async function edit(cmd: string, params: Record<string, unknown>) {
  if (engine.readOnly) return;
  try {
    state = await engine.send(cmd, params);
    editsSinceDiscard++;
    markDirty();
    renderDirty = true;
    render();
  } catch (e) {
    status("Error: " + (e as Error).message);
    console.error(cmd, e);
  }
}

// ---- playback -------------------------------------------------------------
function muteMask(): number[] {
  if (soloed.size && state) {
    return state.tracks.map((t) => t.index).filter((i) => !soloed.has(i));
  }
  return [...muted];
}

// The song's GOTO loop: [destTick, tick) across tracks. Returns null when
// tracks disagree (independent channel loops have no global loop point).
function songLoopTicks(): { start: number; end: number } | null {
  const loops = (state?.tracks ?? [])
    .map((t) => t.songLoop)
    .filter((l): l is { tick: number; destTick: number } => !!l);
  if (!loops.length) return null;
  const end = Math.max(...loops.map((l) => l.tick));
  const start = Math.min(...loops.map((l) => l.destTick));
  const agree = loops.every((l) => l.tick === end && l.destTick === start);
  if (agree && end > start) return { start, end };
  // channels loop independently: the combined song repeats after the LCM of
  // the channel loop lengths (from the latest loop start). Cap it so a
  // pathological LCM doesn't demand a 20-minute render.
  const lens = loops.map((l) => l.tick - l.destTick).filter((x) => x > 0);
  if (!lens.length) return null;
  const gcd = (a: number, b: number): number => (b ? gcd(b, a % b) : a);
  const lcm = lens.reduce((a, b) => (a / gcd(a, b)) * b, 1);
  const lStart = Math.max(...loops.map((l) => l.destTick));
  const lEnd = lStart + lcm;
  return lEnd / ticksPerSec() <= 120 ? { start: lStart, end: lEnd } : null;
}

async function ensureRendered(): Promise<boolean> {
  if (!engine.canPlay) return false;
  if (!renderDirty && audio.duration > 0) return true;
  status("Rendering audio…");
  // render at least through the end of the song loop so loop playback has
  // real audio to cycle over
  const lp = songLoopTicks();
  const seconds = Math.max(45, lp ? Math.ceil(lp.end / ticksPerSec()) + 2 : 0);
  const url = await engine.render(seconds, muteMask());
  if (!url) { status("Render failed"); return false; }
  await audio.load(url);
  if (lp) audio.setLoopPoints(lp.start / ticksPerSec(), lp.end / ticksPerSec());
  else audio.setLoopPoints(0, 0);
  renderDirty = false;
  status("Ready");
  return true;
}

async function startPlayback(fromSec = 0) {
  if (!(await ensureRendered())) return;
  audio.play(fromSec);
  ($("btn-play") as HTMLButtonElement).textContent = "⏸";
}

async function previewNote(track: number, note: number) {
  if (!engine.canPlay) return;
  const url = await engine.renderNote(track, note);
  if (url) audio.preview(url);
}

audio.onTick = (t) => {
  playheadTick = t * ticksPerSec();
  roll.setPlayhead(playheadTick);
  highlightTrackerRow();
  const dur = audio.duration;
  ($("seek") as HTMLInputElement).value = String(dur ? (t / dur) * 1000 : 0);
  $("time").textContent = `${fmt(t)} / ${fmt(dur)}`;
};
audio.onEnded = () => {
  ($("btn-play") as HTMLButtonElement).textContent = "▶";
  roll.setPlayhead(-1);
};

function togglePlay() {
  if (audio.isPlaying) {
    audio.pause();
    ($("btn-play") as HTMLButtonElement).textContent = "▶";
  } else {
    const from = audio.duration > 0 ? audio.current : pendingSeekSec;
    pendingSeekSec = 0;
    startPlayback(from);
  }
}

// ---- transport / toolbar --------------------------------------------------
function updateTransport() {
  ($("btn-undo") as HTMLButtonElement).disabled = !state?.canUndo || engine.readOnly;
  ($("btn-redo") as HTMLButtonElement).disabled = !state?.canRedo || engine.readOnly;
  ($("btn-file") as HTMLButtonElement).disabled = engine.readOnly;
  for (const id of ["btn-add", "btn-optimize", "btn-eyedrop"]) ($(id) as HTMLButtonElement).disabled = engine.readOnly;
  for (const id of ["btn-play", "btn-stop", "btn-loop"]) ($(id) as HTMLButtonElement).disabled = !engine.canPlay;
}

let wired = false;
function wireGlobal() {
  if (wired) return; wired = true;
  // right panel resize via the splitter
  {
    const split = $("splitter");
    let dragging = false;
    split.addEventListener("mousedown", () => { dragging = true; split.classList.add("dragging"); });
    window.addEventListener("mousemove", (e) => {
      if (!dragging) return;
      const w = Math.max(200, Math.min(560, window.innerWidth - e.clientX));
      document.documentElement.style.setProperty("--inspector-w", w + "px");
    });
    window.addEventListener("mouseup", () => { dragging = false; split.classList.remove("dragging"); });
  }
  $("btn-undo").addEventListener("click", () => edit("undo", {}));
  $("btn-redo").addEventListener("click", () => edit("redo", {}));
  $("btn-play").addEventListener("click", togglePlay);
  $("btn-stop").addEventListener("click", () => { audio.stop(); ($("btn-play") as HTMLButtonElement).textContent = "▶"; roll.setPlayhead(-1); });
  $("btn-loop").addEventListener("click", () => {
    const on = !$("btn-loop").classList.contains("on");
    $("btn-loop").classList.toggle("on", on);
    audio.setLoop(on);
  });
  $("btn-add").addEventListener("click", () => setAddMode(!addMode));
  $("btn-zoomin").addEventListener("click", () => roll.zoomX(1.25));
  $("btn-zoomout").addEventListener("click", () => roll.zoomX(0.8));
  $("btn-optimize").addEventListener("click", legalize);
  $("btn-eyedrop").addEventListener("click", () => {
    if (eyedropOn) {
      eyedropOn = false; eyedropRefs = [];
      $("btn-eyedrop").classList.remove("on");
      status("Instrument match cancelled");
      return;
    }
    // clicking a note later clears the marquee, so capture the refs now
    eyedropRefs = roll.getSelectedRefs();
    if (!eyedropRefs.length) { status("Select notes first, then click 🖌, then click the note to copy from"); return; }
    eyedropOn = true;
    $("btn-eyedrop").classList.add("on");
    status(`Click a note to copy its instrument onto ${eyedropRefs.length} selected note${eyedropRefs.length > 1 ? "s" : ""}`);
  });
  // single hamburger file menu
  {
    const btn = $("btn-file"), menu = $("menu-file");
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      document.querySelectorAll(".menu.show").forEach((mm) => mm !== menu && mm.classList.remove("show"));
      menu.classList.toggle("show");
    });
    document.addEventListener("click", () => menu.classList.remove("show"));
    const close = () => menu.classList.remove("show");
    menu.querySelectorAll("[data-act]").forEach((b) =>
      b.addEventListener("click", () => {
        close();
        const act = b.getAttribute("data-act");
        if (act === "open") openSong();
        else if (act === "save") saveAs();
        else if (act === "new") newSong();
        else if (act === "quit") quitApp();
      }));
    menu.querySelectorAll("[data-imp]").forEach((b) =>
      b.addEventListener("click", () => { close(); doImport(b.getAttribute("data-imp")!); }));
    menu.querySelectorAll("[data-exp]").forEach((b) =>
      b.addEventListener("click", () => { close(); doExport(b.getAttribute("data-exp")!); }));
  }
  $("btn-tracker").addEventListener("click", () => setTracker(!trackerOn));
  ($("seek") as HTMLInputElement).addEventListener("input", (e) => {
    const frac = +(e.target as HTMLInputElement).value / 1000;
    audio.seek(frac * audio.duration);
  });
  window.addEventListener("keydown", onKey);
  // engine-crash recovery: the Rust shell respawns the sidecar; reopen the song
  engine.onEngineRestart = async () => {
    if (dirty || !lastPath) {
      throw new Error("Audio engine stopped. Reopen a saved session or start a new song; unsaved changes cannot be recovered.");
    }
    status("Audio engine restarted — recovering…");
    if (lastPath) {
      // send deliberately bypasses request's recovery callback: a missing or
      // repeatedly failing engine must not recurse indefinitely.
      state = await engine.send(lastPath.toLowerCase().endsWith(".gtb") ? "openSession" : "open", { path: lastPath });
      renderDirty = true; render();
      status("Audio engine restarted — saved song reloaded");
    }
  };
  // unsaved-changes guard on window close (X button): prevent the close
  // synchronously, then decide with a native dialog and quit via Rust.
  try {
    const w = (window as any).__TAURI__?.window;
    const win = w?.getCurrentWindow?.() ?? w?.getCurrent?.();
    win?.onCloseRequested?.(async (ev: { preventDefault(): void }) => {
      if (!dirty) return;  // no changes: let the close proceed
      ev.preventDefault();
      if (await askNative("You have unsaved changes. Quit anyway?")) {
        await (window as any).__TAURI__.core.invoke("quit_app");
      }
    });
  } catch { /* browser mode */ }
}

function setAddMode(on: boolean) {
  if (engine.readOnly) return;
  addMode = on;
  $("btn-add").classList.toggle("on", on);
  roll.setAddMode(on);
  status(on ? "Add-note mode: click the roll to place notes" : "Ready");
}

// Save is always Save As — the default song lives in the app bundle, so a
// pathless save would silently overwrite the bundled soundbank.
// Native yes/no dialog: window.confirm is unreliable in the webview
// (silently returns false inside close-request handling).
async function askNative(message: string, title = "Goof Troop Boop"): Promise<boolean> {
  try {
    const answer = await (window as any).__TAURI__.core.invoke("plugin:dialog|message", {
      message, title, kind: "warning", buttons: "YesNo",
    });
    return answer === "Yes";
  } catch {
    return confirm(message);
  }
}

// Quit from the menu: offer to save first, never lose work silently.
async function quitApp() {
  if (dirty) {
    if (await askNative("You have unsaved changes. Save before quitting?")) {
      await saveAs();
      if (dirty) return;  // save was cancelled or failed - stay open
    } else if (!(await askNative("Quit without saving?"))) {
      return;
    }
  }
  try {
    await (window as any).__TAURI__.core.invoke("quit_app");
  } catch (e) {
    status("Quit failed: " + (e as Error).message);
  }
}

async function saveAs() {
  try {
    const path = await engine.saveDialog(FILTERS.gtb, "song.gtb");
    if (!path) return;
    // .gtb sessions carry editor extras (ghost notes) the SPC can't hold;
    // Export -> SPC is the way to emit a bare SPC
    await engine.request("saveSession", { path, extra: { ghosts } });
    lastPath = path;
    clearDirty();
    status(`Saved session ${path}${ghosts.length ? ` (${ghosts.length} ghost note${ghosts.length > 1 ? "s" : ""} kept)` : ""}`);
  } catch (e) {
    status("Save failed: " + (e as Error).message);
  }
}

async function newSong() {
  if (dirty && !confirm("You have unsaved changes. Start a new song anyway?")) return;
    try {
      state = await engine.send("new", {});
      lastPath = null;
      ghosts = []; ghostTrash = []; roll.setGhosts(ghosts);
      afterSongLoad("New song — add notes with the ✎ tool");
    } catch (e) { status("New failed: " + (e as Error).message); }
}

async function openSong() {
  if (dirty && !confirm("You have unsaved changes. Open another song anyway?")) return;
  const path = await engine.openDialog(FILTERS.open);
  if (!path) return;
  try {
    if (path.toLowerCase().endsWith(".gtb")) {
      const resp = await engine.request("openSession", { path });
      state = resp.state as typeof state;
      lastPath = path; ghostTrash = [];
      const extra = resp.extra as { ghosts?: GhostNote[] } | undefined;
      ghosts = extra?.ghosts ?? [];
      roll.setGhosts(ghosts);
      afterSongLoad(`Opened session ${path}${ghosts.length ? ` (${ghosts.length} ghost note${ghosts.length > 1 ? "s" : ""})` : ""}`);
    } else {
      state = await engine.open(path);
      lastPath = path;
      ghosts = []; ghostTrash = []; roll.setGhosts(ghosts);
      afterSongLoad("Opened " + path);
    }
  } catch (e) {
    status("Open failed: " + (e as Error).message);
  }
}

function afterSongLoad(msg: string) {
  clearDirty();
  activeTrack = state!.tracks.find((t) => t.noteCount > 0)?.index ?? 0;
  selected = null; renderDirty = true; audio.stop();
  playheadTick = 0; pendingSeekSec = 0;
  roll.clearSelection();
  ($("btn-play") as HTMLButtonElement).textContent = "▶";
  roll.setPlayhead(-1);
  render();
  status(msg);
}

// ---- format matrix: import / export ----------------------------------------
const FILTERS: Record<string, { name: string; extensions: string[] }[]> = {
  gtb: [{ name: "Goof Troop Boop session", extensions: ["gtb"] }],
  spc: [{ name: "SNES SPC", extensions: ["spc"] }],
  open: [{ name: "GTB session / SNES SPC", extensions: ["gtb", "spc"] }],
  asm: [{ name: "SPC700 ASM", extensions: ["asm"] }],
  midi: [{ name: "MIDI", extensions: ["mid", "midi"] }],
  rom: [{ name: "SNES ROM", extensions: ["smc", "sfc"] }],
  wav: [{ name: "WAV audio", extensions: ["wav"] }],
  mp3: [{ name: "MP3 audio", extensions: ["mp3"] }],
};

// Conversion options for foreign MIDIs (ignored when the file carries our
// lossless GTB1 blob). Program maps: "5:8, 30:5" style.
function midiOptionsDialog(): Promise<Record<string, unknown> | null> {
  return new Promise((resolve) => {
    const back = $("modal-back"), modal = $("modal");
    modal.innerHTML = `<h3>MIDI import options</h3>
      <div class="modal-sub">Used when converting a generic MIDI. MIDIs exported by Goof Troop Boop restore losslessly and ignore these.</div>
      <div class="field"><label>Default instrument (0-255)</label><input id="mo-prog" type="number" min="0" max="255" value="1"></div>
      <div class="field"><label>Initial duration rate (0-255)</label><input id="mo-dur" type="number" min="0" max="255" value="180"></div>
      <div class="field"><label>Program map (from:to, comma-separated)</label><input id="mo-map" type="text" placeholder="e.g. 5:8, 30:5, 61:10, 81:6"></div>
      <div class="field"><label><input id="mo-nodef" type="checkbox" style="width:auto;height:auto;margin-right:6px">Disable built-in Goof Troop remap defaults</label></div>
      <div class="row-btns">
        <button id="mo-ok">Import</button>
        <button id="mo-cancel">Cancel</button>
      </div>`;
    $("mo-ok").addEventListener("click", () => {
      const options: Record<string, unknown> = {
        defaultProgram: +(($("mo-prog") as HTMLInputElement).value || 1),
        duration: +(($("mo-dur") as HTMLInputElement).value || 180),
        noDefaultMap: ($("mo-nodef") as HTMLInputElement).checked,
      };
      const mapText = ($("mo-map") as HTMLInputElement).value.trim();
      if (mapText) {
        const programMap = mapText.split(",").map((p) => {
          const [from, to] = p.split(":").map((x) => +x.trim());
          return { from, to };
        }).filter((m) => Number.isFinite(m.from) && Number.isFinite(m.to));
        if (programMap.length) options.programMap = programMap;
      }
      back.classList.remove("show");
      resolve(options);
    });
    $("mo-cancel").addEventListener("click", () => { back.classList.remove("show"); resolve(null); });
    back.classList.add("show");
  });
}

function pickRomSlot(slots: { slot: number; size: number }[], needBytes: number | null): Promise<number | null> {
  return new Promise((resolve) => {
    const back = $("modal-back"), modal = $("modal");
    modal.innerHTML = `<h3>Choose song slot</h3>
      <div class="modal-sub">${needBytes !== null ? `Sequence needs ${needBytes} bytes; slots too small are disabled.` : "Pick the song to open."}</div>`;
    for (const s of slots) {
      const b = document.createElement("button");
      const tooSmall = needBytes !== null && s.size < needBytes;
      b.className = "slot-row" + (tooSmall ? " too-small" : "");
      b.disabled = tooSmall;
      b.innerHTML = `<span>Slot 0x${s.slot.toString(16).toUpperCase().padStart(2, "0")}</span><span class="cap">${s.size} bytes</span>`;
      if (!tooSmall) b.addEventListener("click", () => { back.classList.remove("show"); resolve(s.slot); });
      modal.appendChild(b);
    }
    const cancel = document.createElement("button");
    cancel.className = "modal-cancel";
    cancel.textContent = "Cancel";
    cancel.addEventListener("click", () => { back.classList.remove("show"); resolve(null); });
    modal.appendChild(cancel);
    back.classList.add("show");
  });
}

async function doImport(fmt: string) {
  if (dirty && !confirm("You have unsaved changes. Import another song anyway?")) return;
  try {
    const path = await engine.openDialog(FILTERS[fmt]);
    if (!path) return;
    if (fmt === "spc") {
      state = await engine.open(path);
    } else if (fmt === "asm") {
      state = await engine.send("importAsm", { path });
    } else if (fmt === "midi") {
      const options = await midiOptionsDialog();
      if (!options) return;
      state = await engine.send("importMidi", { path, options });
    } else if (fmt === "rom") {
      const slots = await engine.listRomSongs(path);
      if (!slots.length) { status("No songs found in ROM"); return; }
      const slot = await pickRomSlot(slots, null);
      if (slot === null) return;
      state = await engine.send("openRom", { path, slot });
    }
    lastPath = fmt === "spc" ? path : null;
    ghosts = []; ghostTrash = []; roll.setGhosts(ghosts);
    afterSongLoad(`Imported ${fmt.toUpperCase()}: ${path}`);
  } catch (e) {
    status(`Import ${fmt.toUpperCase()} failed: ` + (e as Error).message);
  }
}

async function doExport(fmt: string) {
  try {
    const used = state?.budget?.used ?? null;
    if (fmt === "rom") {
      // Pick a template ROM, then a big-enough slot, then the destination.
      const rom = await engine.openDialog(FILTERS.rom);
      if (!rom) return;
      const slots = await engine.listRomSongs(rom);
      if (!slots.length) { status("No song slots found in ROM"); return; }
      const slot = await pickRomSlot(slots, used);
      if (slot === null) return;
      const path = await engine.saveDialog(FILTERS.rom, "gooftroop-custom.smc");
      if (!path) return;
      await engine.request("exportRom", { rom, slot, path });
      status(`Exported ROM (slot 0x${slot.toString(16).toUpperCase()}): ${path}`);
      return;
    }
    const path = await engine.saveDialog(FILTERS[fmt], "song." + (fmt === "midi" ? "mid" : fmt));
    if (!path) return;
    if (fmt === "spc") await engine.request("save", { path });
    else if (fmt === "asm") await engine.request("exportAsm", { path });
    else if (fmt === "midi") await engine.request("exportMidi", { path });
    else if (fmt === "wav") { status("Rendering WAV…"); await engine.request("render", { seconds: 60, mute: muteMask(), path }); }
    else if (fmt === "mp3") { status("Rendering MP3…"); await engine.request("renderMp3", { seconds: 60, mute: muteMask(), path }); }
    status(`Exported ${fmt.toUpperCase()}: ${path}`);
  } catch (e) {
    status(`Export ${fmt.toUpperCase()} failed: ` + (e as Error).message);
  }
}

// ---- tracker view -----------------------------------------------------------
// Classic tracker layout: one row per 16th (12 ticks), one column per track,
// cells show note name + octave. Click a cell to select that note.
const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
let trackerOn = false;

function noteName(pitch: number): string {
  return NOTE_NAMES[((pitch % 12) + 12) % 12] + (Math.floor(pitch / 12) - 1);
}

function renderTracker() {
  const el = $("tracker");
  if (!trackerOn || !state) { el.hidden = true; return; }
  el.hidden = false;
  const STEP = 12;
  let maxTick = 0;
  const grid = new Map<string, { pitch: number; i: number; len: number; program: number }>();
  for (const tr of state.tracks) {
    for (const n of tr.notes) {
      if (n.rest || n.pitch < 0) continue;
      maxTick = Math.max(maxTick, n.tick + n.len);
      grid.set(`${tr.index}:${Math.round(n.tick / STEP)}`, { pitch: n.pitch, i: n.i, len: n.len, program: n.program });
    }
  }
  const rows = Math.ceil(maxTick / STEP) + 1;
  let html = `<table><thead><tr><th class="tk-row">row</th>${state.tracks.map((t) =>
    `<th style="color:${TRACK_COLORS[t.index % TRACK_COLORS.length]}">T${t.index + 1}</th>`).join("")}</tr></thead><tbody>`;
  for (let r = 0; r < rows; r++) {
    const bar = (r * STEP) % 192 === 0;
    html += `<tr class="${bar ? "bar" : ""}" data-tkr="${r}"><td class="tk-row">${r.toString().padStart(3, "0")}</td>`;
    for (const tr of state.tracks) {
      const cell = grid.get(`${tr.index}:${r}`);
      const sel = selected && cell && selected.track === tr.index && selected.note === cell.i;
      html += cell
        ? `<td class="tk-note ${sel ? "sel" : ""}" data-tk="${tr.index}:${cell.i}">${noteName(cell.pitch)} ${cell.program.toString().padStart(2, "0")}</td>`
        : `<td class="tk-empty" data-tka="${tr.index}:${r}">···</td>`;
    }
    html += "</tr>";
  }
  el.innerHTML = html + "</tbody></table>";
  el.querySelectorAll("[data-tk]").forEach((td) =>
    td.addEventListener("click", () => {
      const [track, note] = td.getAttribute("data-tk")!.split(":").map(Number);
      selected = { track, note };
      roll.setSelected(track, note);
      if (track !== activeTrack) { activeTrack = track; render(); }
      else { renderInspector(); renderTracker(); }
      previewNote(track, note);
    }));
  // clicking an empty cell spawns a note there (C of the track's last octave, or C4)
  el.querySelectorAll("[data-tka]").forEach((td) =>
    td.addEventListener("click", () => {
      const [track, row] = td.getAttribute("data-tka")!.split(":").map(Number);
      const tr = state?.tracks.find((t) => t.index === track);
      const near = tr?.notes.filter((n) => !n.rest && n.tick <= row * STEP).pop()
        ?? tr?.notes.find((n) => !n.rest);
      const pitch = near ? near.pitch : 60;
      edit("insertNote", { track, tick: row * STEP, pitch, len: STEP });
    }));
  highlightTrackerRow();
}

// keep the tracker's current row in sync with the playhead
function highlightTrackerRow() {
  if (!trackerOn) return;
  const el = $("tracker");
  const row = Math.floor(playheadTick / 12);
  el.querySelectorAll("tr.cur").forEach((t) => t.classList.remove("cur"));
  const cur = el.querySelector(`[data-tkr="${row}"]`);
  if (cur) {
    cur.classList.add("cur");
    (cur as HTMLElement).scrollIntoView({ block: "nearest" });
  }
}

function setTracker(on: boolean) {
  trackerOn = on;
  $("btn-tracker").classList.toggle("on", on);
  ($("roll") as HTMLCanvasElement).style.visibility = on ? "hidden" : "visible";
  renderTracker();
}


function onKey(e: KeyboardEvent) {
  const tag = (e.target as HTMLElement).tagName;
  if (tag === "INPUT" || tag === "SELECT") return;
  if (e.code === "Space") { e.preventDefault(); togglePlay(); return; }
  if (e.key === "a" || e.key === "A") { setAddMode(!addMode); return; }
  if (e.key === "t" || e.key === "T") { setTracker(!trackerOn); return; }
  const key = e.key.toLowerCase();
  if ((e.ctrlKey || e.metaKey) && key === "z" && !e.shiftKey) {
    e.preventDefault();
    if (ghostTrash.length && editsSinceDiscard === 0) {
      const batch = ghostTrash.pop()!;
      ghosts.push(...batch);
      markDirty();
      roll.setGhosts(ghosts);
      status(`Restored ${batch.length} ghost note${batch.length > 1 ? "s" : ""}`);
      return;
    }
    edit("undo", {});
    return;
  }
  if ((e.ctrlKey || e.metaKey) && (key === "y" || (e.shiftKey && key === "z"))) { e.preventDefault(); edit("redo", {}); return; }
  if ((e.ctrlKey || e.metaKey) && e.key === "s") { e.preventDefault(); saveAs(); return; }
  if ((e.ctrlKey || e.metaKey) && e.key === "c") {
    const sel = roll.getSelectedNotes();
    if (sel.length) { clipboard = sel; status(`Copied ${sel.length} note${sel.length > 1 ? "s" : ""}`); }
    return;
  }
  if ((e.ctrlKey || e.metaKey) && e.key === "v") {
    e.preventDefault(); pasteClipboard();
    return;
  }
  if ((e.ctrlKey || e.metaKey) && (e.key === "=" || e.key === "+")) { e.preventDefault(); roll.zoomX(1.25); return; }
  if ((e.ctrlKey || e.metaKey) && e.key === "-") { e.preventDefault(); roll.zoomX(0.8); return; }
  if ((e.key === "Delete" || e.key === "Backspace") && selected && !engine.readOnly) {
    e.preventDefault(); edit("eraseNote", { track: selected.track, note: selected.note }); selected = null;
  }
}

function fmt(s: number): string {
  if (!isFinite(s)) s = 0;
  const m = Math.floor(s / 60), r = Math.floor(s % 60);
  return `${m}:${r.toString().padStart(2, "0")}`;
}
function status(msg: string) { $("status").textContent = msg; }
function escapeHtml(s: string): string {
  return s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]!));
}

boot();
