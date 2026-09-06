import type { Note, SongState } from "./engine";

export const TRACK_COLORS = [
  "#6fd1c2", "#6ea8fe", "#c58cf0", "#f2a4c9",
  "#f5c451", "#9be26b", "#f28f6b", "#8fd0f5",
];
const TICKS_PER_BAR = 192; // PPQN 48, 4/4
const KEY_W = 66;
const HEADER_H = 26;
const SCROLL_H = 14;  // bottom overview strip: song minimap + viewport thumb
const MIN_PITCH = 12;
const MAX_PITCH = 108;

export interface RollCallbacks {
  onSelectNote(track: number, note: number): void;
  onAddNote(track: number, tick: number, pitch: number, len: number): void;
  onMoveNote(track: number, note: number, tick: number, pitch: number): void;
  onMoveNotes(items: { track: number; tick: number; pitch: number }[], dTick: number, dPitch: number): void;
  onDragPreview(track: number, pitch: number): void;
  onPreviewEnd(): void;
  onLoopResize(track: number, loop: number, startTick: number, endTick: number, count: number): void;
  onSelectSetting(track: number, setting: number): void;
  onRemoveGhost(index: number): void;
  onResizeNote(track: number, note: number, len: number): void;
  onPreview(track: number, note: number): void;
  onCreateLoop(track: number, startTick: number, endTick: number): void;
  onSeek(tick: number, andPlay: boolean): void;
  onKeyPreview(pitch: number): void;
}

interface Hit { track: number; note: number; x: number; y: number; w: number; h: number; }

export class PianoRoll {
  private ctx: CanvasRenderingContext2D;
  private state: SongState | null = null;
  private activeTrack = 0;
  private hidden = new Set<number>();
  private selected: { track: number; note: number } | null = null;
  private hits: Hit[] = [];
  private scrollX = 0;
  private scrollY = 0;
  private dpr = 1;
  private pxPerTick = 0.5;
  private semiH = 12;
  private playheadTick = -1;
  private addMode = false;

  // drag state (mode: move the note or resize via its right edge)
  private drag: { note: Note; track: number; startX: number; startY: number; moved: boolean; mode: "move" | "resize"; group: boolean } | null = null;
  // shift+drag on the bar ruler sweeps out a new loop on the active track
  private loopDrag: { startTick: number; curTick: number } | null = null;
  // drag on empty grid sweeps out a marquee; the notes inside become a group
  private marquee: { x0: number; y0: number; x1: number; y1: number } | null = null;
  private multiSel: { track: number; note: number }[] = [];
  // dragging the bottom overview strip pans the viewport
  private scrollbarDrag = false;
  // meta lanes under the ruler: collapsible, scroll/zoom-synced
  private lanesOpen: Record<string, boolean> = { loops: true, tempo: true, volume: true };
  private selLoop: number | null = null;
  private loopEdgeDrag: { loop: number; edge: "start" | "end"; curX: number } | null = null;
  // ghost of the note being dragged, at its would-be drop position
  private dragGhost: { tick: number; pitch: number; len: number } | null = null;
  // add-mode: drag out the new note's length before committing it
  private addDrag: { track: number; tick: number; pitch: number } | null = null;
  private glissando = false;
  private lastGlissPitch = -1;
  // clickable setting markers (tempo/lfo/echo triangles on the ruler)
  private settingHits: { x: number; setting: number }[] = [];
  private laneMarkerHits: { lane: string; x: number; track: number; setting: number }[] = [];
  // rejected edits shown as red outlines until legalized or discarded
  private ghosts: { track: number; tick: number; pitch: number; len: number; reason: string }[] = [];
  private ghostHits: { x: number; y: number; w: number; h: number; i: number }[] = [];

  constructor(private canvas: HTMLCanvasElement, private cb: RollCallbacks) {
    this.ctx = canvas.getContext("2d")!;
    canvas.addEventListener("mousedown", (e) => this.onDown(e));
    window.addEventListener("mousemove", (e) => this.onMove(e));
    window.addEventListener("mouseup", (e) => this.onUp(e));
    canvas.addEventListener("wheel", (e) => this.onWheel(e), { passive: false });
    canvas.addEventListener("contextmenu", (e) => {
      e.preventDefault();
      const { x, y } = this.localXY(e);
      if (x < KEY_W || y < HEADER_H) return;
      // right-click: set the playhead there and play
      this.cb.onSeek(Math.max(0, this.tickForX(x)), true);
    });
    new ResizeObserver(() => this.resize()).observe(canvas.parentElement!);
  }

  setState(s: SongState, activeTrack: number, hidden: Set<number>) {
    this.state = s; this.activeTrack = activeTrack; this.hidden = hidden;
    this.resize();
  }
  setSelected(track: number, note: number) { this.selected = { track, note }; this.draw(); }
  clearSelection() { this.selected = null; this.multiSel = []; this.selLoop = null; this.draw(); }
  // marquee group refs if present, else the single selected note ref
  getSelectedRefs(): { track: number; note: number }[] {
    return this.multiSel.length ? [...this.multiSel] : (this.selected ? [this.selected] : []);
  }
  // marquee group if present, else the single selected note
  getSelectedNotes(): { track: number; tick: number; pitch: number; len: number }[] {
    const refs = this.multiSel.length ? this.multiSel : (this.selected ? [this.selected] : []);
    const out: { track: number; tick: number; pitch: number; len: number }[] = [];
    for (const r of refs) {
      const tr = this.state?.tracks.find((t) => t.index === r.track);
      const n = tr?.notes.find((nn) => nn.i === r.note);
      if (n && !n.rest) out.push({ track: r.track, tick: n.tick, pitch: n.pitch, len: n.len });
    }
    return out;
  }
  setGhosts(g: { track: number; tick: number; pitch: number; len: number; reason: string }[]) {
    this.ghosts = g; this.draw();
  }
  setAddMode(on: boolean) { this.addMode = on; this.canvas.style.cursor = on ? "crosshair" : "default"; }
  setPlayhead(tick: number) {
    // Auto-follow: when the playhead runs off the right edge, jump the camera
    // ahead — but only if it was visible just before (a manual scroll away
    // "detaches" the camera until the playhead is back in view).
    const prev = this.playheadTick;
    this.playheadTick = tick;
    if (tick > prev && prev >= 0) {
      const W = this.W();
      const margin = 30;
      const xPrev = this.xForTick(prev);
      const xNow = this.xForTick(tick);
      if (xNow > W - margin && xPrev >= KEY_W && xPrev <= W) {
        const maxX = Math.max(0, this.maxTick() * this.pxPerTick - (W - KEY_W));
        this.scrollX = clamp(tick * this.pxPerTick - (W - KEY_W) * 0.15, 0, maxX);
      }
    }
    this.draw();
  }
  zoomX(factor: number) { this.pxPerTick = clamp(this.pxPerTick * factor, 0.12, 4); this.draw(); }
  zoomY(factor: number) { this.semiH = clamp(this.semiH * factor, 6, 26); this.draw(); }
  resetZoom() { this.pxPerTick = 0.5; this.semiH = 12; this.scrollX = 0; this.scrollY = 0; this.draw(); }

  private H() { return this.canvas.height / this.dpr; }
  private W() { return this.canvas.width / this.dpr; }
  private lanes(): { key: string; h: number }[] {
    return [
      { key: "loops", h: this.lanesOpen.loops ? 22 : 8 },
      { key: "tempo", h: this.lanesOpen.tempo ? 16 : 8 },
      { key: "volume", h: this.lanesOpen.volume ? 16 : 8 },
    ];
  }
  private laneH() { return this.lanes().reduce((a, l) => a + l.h, 0); }
  private laneTop(key: string) {
    let y = HEADER_H;
    for (const l of this.lanes()) { if (l.key === key) return y; y += l.h; }
    return y;
  }
  private laneAt(y: number): string | null {
    let top = HEADER_H;
    for (const l of this.lanes()) { if (y > top && y <= top + l.h) return l.key; top += l.h; }
    return null;
  }
  private gridTop() { return HEADER_H + this.laneH(); }
  private yForPitch(p: number) { return this.gridTop() + (MAX_PITCH - p) * this.semiH - this.scrollY; }
  private pitchForY(y: number) { return Math.round(MAX_PITCH - (y - this.gridTop() + this.scrollY) / this.semiH); }
  private xForTick(t: number) { return KEY_W + t * this.pxPerTick - this.scrollX; }
  private tickForX(x: number) { return Math.max(0, (x - KEY_W + this.scrollX) / this.pxPerTick); }

  private maxTick(): number {
    let m = TICKS_PER_BAR * 8;
    if (this.state) for (const tr of this.state.tracks) for (const n of tr.notes) m = Math.max(m, n.tick + n.len);
    return m + TICKS_PER_BAR;
  }

  private resize() {
    const rect = this.canvas.parentElement!.getBoundingClientRect();
    this.dpr = window.devicePixelRatio || 1;
    this.canvas.width = Math.max(1, Math.floor(rect.width * this.dpr));
    this.canvas.height = Math.max(1, Math.floor(rect.height * this.dpr));
    this.draw();
  }

  private onWheel(e: WheelEvent) {
    e.preventDefault();
    if (e.ctrlKey) { this.zoomX(e.deltaY < 0 ? 1.12 : 0.89); return; }
    const maxX = Math.max(0, this.maxTick() * this.pxPerTick - (this.W() - KEY_W));
    if (e.shiftKey) {
      this.scrollX = clamp(this.scrollX + e.deltaY, 0, maxX);
    } else {
      // trackpads report horizontal pans as deltaX; honor both axes
      if (e.deltaX) this.scrollX = clamp(this.scrollX + e.deltaX, 0, maxX);
      const maxY = Math.max(0, (MAX_PITCH - MIN_PITCH) * this.semiH - (this.H() - this.gridTop()));
      this.scrollY = clamp(this.scrollY + e.deltaY, 0, maxY);
    }
    this.draw();
  }

  private activeLoops() {
    return this.state?.tracks.find((t) => t.index === this.activeTrack)?.loops ?? [];
  }

  private onLaneDown(x: number) {
    const loops = this.activeLoops();
    for (let i = 0; i < loops.length; i++) {
      const lp = loops[i];
      if (lp.destTick === undefined || lp.destTick === null) continue;
      const x0 = this.xForTick(Math.min(lp.destTick, lp.tick));
      const x1 = this.xForTick(Math.max(lp.destTick, lp.tick));
      if (Math.abs(x - x0) <= 5) { this.selLoop = i; this.loopEdgeDrag = { loop: i, edge: "start", curX: x }; this.draw(); return; }
      if (Math.abs(x - x1) <= 5) { this.selLoop = i; this.loopEdgeDrag = { loop: i, edge: "end", curX: x }; this.draw(); return; }
      if (x > x0 && x < x1) { this.selLoop = this.selLoop === i ? null : i; this.draw(); return; }
    }
    this.selLoop = null;
    this.draw();
  }

  // map an x inside the overview strip to a viewport position (thumb centered)
  private scrollToOverviewX(x: number) {
    const viewPx = this.W() - KEY_W;
    const totalPx = Math.max(1, this.maxTick() * this.pxPerTick);
    const frac = clamp((x - KEY_W) / viewPx, 0, 1);
    this.scrollX = clamp(frac * totalPx - viewPx / 2, 0, Math.max(0, totalPx - viewPx));
    this.draw();
  }

  private localXY(e: MouseEvent) {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  private hitAt(x: number, y: number): Hit | null {
    // reverse order: the active track draws last, so its notes win overlaps
    for (let i = this.hits.length - 1; i >= 0; i--) {
      const h = this.hits[i];
      if (x >= h.x && x <= h.x + h.w && y >= h.y && y <= h.y + h.h) return h;
    }
    return null;
  }

  private onDown(e: MouseEvent) {
    if (e.button === 2) return;  // contextmenu handler owns right-click
    const { x, y } = this.localXY(e);
    if (x < KEY_W) {
      if (y > HEADER_H && y <= this.gridTop()) {
        // lane gutter: collapse/expand the clicked lane
        const key = this.laneAt(y);
        if (key) { this.lanesOpen[key] = !this.lanesOpen[key]; this.draw(); }
      } else if (y > this.gridTop()) {
        // piano key: sound it, and glissando while the mouse slides
        this.glissando = true;
        this.lastGlissPitch = clamp(this.pitchForY(y), 0, 127);
        this.cb.onKeyPreview(this.lastGlissPitch);
      }
      return;
    }
    if (y > HEADER_H && y <= this.gridTop()) {
      const key = this.laneAt(y);
      if (key === "loops" && this.lanesOpen.loops) this.onLaneDown(x);
      else if (key && this.lanesOpen[key]) {
        // tempo/volume lanes: click a marker to reveal + edit it
        const near = this.laneMarkerHits
          .filter((h) => h.lane === key)
          .find((h) => Math.abs(h.x - x) <= 6);
        if (near) this.cb.onSelectSetting(near.track, near.setting);
      }
      return;
    }
    if (y <= HEADER_H) {
      if (e.shiftKey) {
        const t = Math.round(this.tickForX(x) / 12) * 12;
        this.loopDrag = { startTick: t, curTick: t };
      } else {
        // setting markers (tempo/lfo/echo triangles) are clickable
        const near = this.settingHits.find((h) => Math.abs(h.x - x) <= 6);
        if (near) { this.cb.onSelectSetting(this.activeTrack, near.setting); return; }
        this.cb.onSeek(Math.max(0, this.tickForX(x)), false);
      }
      return;
    }
    if (y >= this.H() - SCROLL_H && x >= KEY_W) {
      this.scrollbarDrag = true;
      this.scrollToOverviewX(x);
      return;
    }
    // clicking a ghost removes it
    const ghost = this.ghostHits.find((g) => x >= g.x && x <= g.x + g.w && y >= g.y && y <= g.y + g.h);
    if (ghost) { this.cb.onRemoveGhost(ghost.i); return; }
    const hit = this.hitAt(x, y);
    if (hit) {
      const inGroup = this.multiSel.some((s) => s.track === hit.track && s.note === hit.note);
      if (!inGroup) this.multiSel = [];
      this.selected = { track: hit.track, note: hit.note };
      this.cb.onSelectNote(hit.track, hit.note);
      if (!inGroup) this.cb.onPreview(hit.track, hit.note);
      const tr = this.state!.tracks.find((t) => t.index === hit.track)!;
      const note = tr.notes.find((n) => n.i === hit.note)!;
      // Grabbing within 6px of the right edge resizes; anywhere else moves.
      const mode = x >= hit.x + hit.w - 6 && !inGroup ? "resize" : "move";
      this.drag = { note, track: hit.track, startX: x, startY: y, moved: false, mode, group: inGroup };
      this.draw();
    } else if (this.addMode) {
      // press starts the note; dragging right sets its length (mouseup commits)
      const tick = Math.round(this.tickForX(x) / 12) * 12;
      const pitch = clamp(this.pitchForY(y), 0, 127);
      this.addDrag = { track: this.activeTrack, tick, pitch };
      this.dragGhost = { tick, pitch, len: 24 };
      this.draw();
    } else {
      // empty space: drag = marquee select, plain click = seek (resolved on mouseup)
      this.marquee = { x0: x, y0: y, x1: x, y1: y };
    }
  }

  private onMove(e: MouseEvent) {
    const { x, y } = this.localXY(e);
    if (this.loopDrag) {
      this.loopDrag.curTick = Math.round(this.tickForX(x) / 12) * 12;
      this.draw();
      return;
    }
    if (this.scrollbarDrag) {
      this.scrollToOverviewX(x);
      return;
    }
    if (this.glissando) {
      if (x < KEY_W) {
        const p = clamp(this.pitchForY(y), 0, 127);
        if (p !== this.lastGlissPitch) { this.lastGlissPitch = p; this.cb.onKeyPreview(p); }
      }
      return;
    }
    if (this.loopEdgeDrag) {
      this.loopEdgeDrag.curX = x;
      this.draw();
      return;
    }
    if (this.addDrag) {
      const raw = Math.round((this.tickForX(x) - this.addDrag.tick) / 6) * 6;
      const len = isFinite(raw) ? Math.max(6, raw) : 24;
      this.dragGhost = { tick: this.addDrag.tick, pitch: this.addDrag.pitch, len };
      this.draw();
      return;
    }
    if (this.marquee) {
      this.marquee.x1 = x; this.marquee.y1 = y;
      this.draw();
      return;
    }
    if (!this.drag) {
      // hover cursor: resize handle near right edges; ghost reason as tooltip
      const g = this.ghostHits.find((gh) => x >= gh.x && x <= gh.x + gh.w && y >= gh.y && y <= gh.y + gh.h);
      this.canvas.title = g ? `Doesn't fit: ${this.ghosts[g.i]?.reason ?? ""} (click to discard, ✨ to retry)` : "";
      const hit = this.hitAt(x, y);
      if (!this.addMode) {
        this.canvas.style.cursor = g ? "pointer" : hit && x >= hit.x + hit.w - 6 ? "ew-resize" : "default";
      }
      return;
    }
    if (Math.abs(x - this.drag.startX) > 3 || Math.abs(y - this.drag.startY) > 3) this.drag.moved = true;
    if (this.drag.moved && this.drag.mode === "move" && !this.drag.note.rest) {
      // ghost the would-be drop position; sound it when the pitch changes
      const d = this.drag;
      const tick = Math.max(0, Math.round(this.tickForX(x - (d.startX - this.xForTick(d.note.tick))) / 12) * 12);
      const pitch = clamp(this.pitchForY(y), 0, 127);
      if (!this.dragGhost || this.dragGhost.tick !== tick || this.dragGhost.pitch !== pitch) {
        if (this.dragGhost?.pitch !== pitch) this.cb.onDragPreview(d.track, pitch);
        this.dragGhost = { tick, pitch, len: d.note.len };
        this.draw();
      }
    }
  }

  private onUp(e: MouseEvent) {
    if (this.loopDrag) {
      const ld = this.loopDrag; this.loopDrag = null;
      const a = Math.min(ld.startTick, ld.curTick), b = Math.max(ld.startTick, ld.curTick);
      if (b - a >= 12) this.cb.onCreateLoop(this.activeTrack, a, b);
      else this.draw();
      return;
    }
    if (this.glissando) {
      this.glissando = false;
      this.cb.onPreviewEnd();
      return;
    }
    if (this.loopEdgeDrag) {
      const ld = this.loopEdgeDrag; this.loopEdgeDrag = null;
      const loops = this.activeLoops();
      const lp = loops[ld.loop];
      if (lp) {
        const t = Math.max(0, Math.round(this.tickForX(ld.curX) / 12) * 12);
        let a = Math.min(lp.destTick!, lp.tick), b = Math.max(lp.destTick!, lp.tick);
        if (ld.edge === "start") a = Math.min(t, b - 12); else b = Math.max(t, a + 12);
        if (a !== Math.min(lp.destTick!, lp.tick) || b !== Math.max(lp.destTick!, lp.tick)) {
          this.cb.onLoopResize(this.activeTrack, ld.loop, a, b, lp.count);
        }
      }
      this.draw();
      return;
    }
    if (this.addDrag) {
      const ad = this.addDrag; this.addDrag = null;
      const len = this.dragGhost?.len ?? 24;
      this.dragGhost = null;
      this.cb.onAddNote(ad.track, ad.tick, ad.pitch, len);
      return;
    }
    if (this.scrollbarDrag) { this.scrollbarDrag = false; return; }
    if (this.marquee) {
      const m = this.marquee; this.marquee = null;
      const w = Math.abs(m.x1 - m.x0), h = Math.abs(m.y1 - m.y0);
      if (w < 4 && h < 4) {
        // plain click on empty space: seek
        this.multiSel = [];
        this.cb.onSeek(Math.max(0, this.tickForX(m.x0)), false);
      } else {
        const [lx, hx] = [Math.min(m.x0, m.x1), Math.max(m.x0, m.x1)];
        const [ly, hy] = [Math.min(m.y0, m.y1), Math.max(m.y0, m.y1)];
        this.multiSel = this.hits
          .filter((t) => t.x + t.w >= lx && t.x <= hx && t.y + t.h >= ly && t.y <= hy)
          .map((t) => ({ track: t.track, note: t.note }));
      }
      this.draw();
      return;
    }
    if (!this.drag) return;
    const d = this.drag; this.drag = null;
    this.dragGhost = null;
    // preview sounds only while the mouse is held on the note
    this.cb.onPreviewEnd();
    if (!d.moved || d.note.rest) { this.draw(); return; }
    const { x, y } = this.localXY(e);
    if (d.mode === "resize") {
      const endTick = this.tickForX(x);
      const len = Math.max(1, Math.round((endTick - d.note.tick) / 6) * 6);
      if (len !== d.note.len) this.cb.onResizeNote(d.track, d.note.i, len);
      return;
    }
    const tick = Math.max(0, Math.round(this.tickForX(x - (d.startX - this.xForTick(d.note.tick))) / 12) * 12);
    const pitch = clamp(this.pitchForY(y), 0, 127);
    if (d.group && this.multiSel.length > 1) {
      // move the whole marquee group by the same delta
      const dTick = tick - d.note.tick;
      const dPitch = pitch - d.note.pitch;
      if (dTick === 0 && dPitch === 0) return;
      const items: { track: number; tick: number; pitch: number }[] = [];
      for (const s of this.multiSel) {
        const tr = this.state!.tracks.find((t) => t.index === s.track);
        const n = tr?.notes.find((nn) => nn.i === s.note);
        if (n && !n.rest) items.push({ track: s.track, tick: n.tick, pitch: n.pitch });
      }
      this.multiSel = [];
      this.cb.onMoveNotes(items, dTick, dPitch);
      return;
    }
    this.cb.onMoveNote(d.track, d.note.i, tick, pitch);
  }

  private draw() {
    const ctx = this.ctx, W = this.W(), H = this.H();
    ctx.save();
    ctx.scale(this.dpr, this.dpr);
    ctx.clearRect(0, 0, W, H);
    this.hits = [];

    const gridTop = this.gridTop();
    // pitch rows
    for (let p = MIN_PITCH; p <= MAX_PITCH; p++) {
      const y = this.yForPitch(p);
      if (y + this.semiH < gridTop || y > H) continue;
      const black = [1, 3, 6, 8, 10].includes(((p % 12) + 12) % 12);
      ctx.fillStyle = black ? "#171a21" : "#1a1d25";
      ctx.fillRect(KEY_W, y, W - KEY_W, this.semiH);
      if (p % 12 === 0) { ctx.strokeStyle = "#2a2f3b"; ctx.beginPath(); ctx.moveTo(KEY_W, y); ctx.lineTo(W, y); ctx.stroke(); }
    }

    // bar grid + header
    const maxT = this.maxTick();
    ctx.textBaseline = "middle";
    ctx.font = "11px -apple-system, Segoe UI, sans-serif";
    for (let t = 0; t <= maxT; t += TICKS_PER_BAR / 4) {
      const x = this.xForTick(t);
      if (x < KEY_W - 1 || x > W) continue;
      const bar = t % TICKS_PER_BAR === 0;
      ctx.strokeStyle = bar ? "#2f3542" : "#212734";
      ctx.beginPath(); ctx.moveTo(x, gridTop); ctx.lineTo(x, H); ctx.stroke();
      if (bar) { ctx.fillStyle = "#788198"; ctx.fillText(String(t / TICKS_PER_BAR + 1), x + 5, HEADER_H / 2); }
    }

    // selected loop: highlight the body and each repetition pass in the grid
    if (this.selLoop !== null) {
      const lp = this.activeLoops()[this.selLoop];
      if (lp && (lp.destTick || lp.destTick === 0)) {
        const a = Math.min(lp.destTick, lp.tick), b = Math.max(lp.destTick, lp.tick);
        const len = b - a;
        for (let k = 0; k < Math.max(1, lp.count); k++) {
          const x0 = this.xForTick(a + k * len), x1 = this.xForTick(a + (k + 1) * len);
          if (x1 < KEY_W || x0 > W) continue;
          const vx0 = Math.max(KEY_W, x0);
          ctx.fillStyle = k === 0 ? "rgba(110,168,254,0.13)" : "rgba(110,168,254,0.07)";
          ctx.fillRect(vx0, gridTop, x1 - vx0, H - gridTop - SCROLL_H);
          ctx.strokeStyle = "rgba(110,168,254,0.55)";
          ctx.beginPath(); ctx.moveTo(x1, gridTop); ctx.lineTo(x1, H - SCROLL_H); ctx.stroke();
        }
      }
    }

    // notes
    const order = this.state
      ? [...this.state.tracks].sort((a, b) => (a.index === this.activeTrack ? 1 : 0) - (b.index === this.activeTrack ? 1 : 0))
      : [];
    for (const tr of order) {
      if (this.hidden.has(tr.index)) continue;
      const color = TRACK_COLORS[tr.index % TRACK_COLORS.length];
      const active = tr.index === this.activeTrack;
      for (const n of tr.notes) if (!n.rest && n.pitch >= 0) this.drawNote(tr.index, n, color, active);
    }

    // settings lane (active track): labeled markers on the header strip
    this.settingHits = [];
    if (this.state) {
      const tr = this.state.tracks.find((t) => t.index === this.activeTrack);
      if (tr) {
        ctx.font = "9px -apple-system, Segoe UI, sans-serif";
        let lastX = -1e9;
        for (let si = 0; si < tr.settings.length; si++) {
          const s = tr.settings[si];
          const x = this.xForTick(s.tick);
          if (x < KEY_W || x > W) continue;
          ctx.fillStyle = s.type === "tempo" ? "#6ea8fe" : "#f5c451";
          ctx.beginPath(); ctx.moveTo(x, HEADER_H); ctx.lineTo(x - 3, HEADER_H - 6); ctx.lineTo(x + 3, HEADER_H - 6); ctx.closePath(); ctx.fill();
          if (x - lastX > 30) {  // avoid label pile-ups
            ctx.fillText(`${s.type} ${s.value}`, x + 4, HEADER_H - 7);
            lastX = x;
          }
          this.settingHits.push({ x, setting: s.i });
        }
      }
    }

    // loop-creation sweep (shift+drag on the ruler)
    if (this.loopDrag) {
      const x0 = this.xForTick(Math.min(this.loopDrag.startTick, this.loopDrag.curTick));
      const x1 = this.xForTick(Math.max(this.loopDrag.startTick, this.loopDrag.curTick));
      ctx.fillStyle = "rgba(245,196,81,0.18)";
      ctx.fillRect(Math.max(KEY_W, x0), HEADER_H, x1 - Math.max(KEY_W, x0), H - HEADER_H);
      ctx.strokeStyle = "#f5c451";
      ctx.strokeRect(Math.max(KEY_W, x0), HEADER_H, x1 - Math.max(KEY_W, x0), H - HEADER_H);
    }

    // marquee selection box
    if (this.marquee) {
      const m = this.marquee;
      ctx.fillStyle = "rgba(110,168,254,0.10)";
      ctx.strokeStyle = "rgba(110,168,254,0.7)";
      const rx = Math.min(m.x0, m.x1), ry = Math.min(m.y0, m.y1);
      ctx.fillRect(rx, ry, Math.abs(m.x1 - m.x0), Math.abs(m.y1 - m.y0));
      ctx.strokeRect(rx, ry, Math.abs(m.x1 - m.x0), Math.abs(m.y1 - m.y0));
    }

    // playhead
    if (this.playheadTick >= 0) {
      const x = this.xForTick(this.playheadTick);
      if (x >= KEY_W && x <= W) {
        ctx.strokeStyle = "#ff5d6c"; ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.moveTo(x, HEADER_H - 8); ctx.lineTo(x, H); ctx.stroke(); ctx.lineWidth = 1;
      }
    }

    // ghost of the dragged note at its would-be drop position
    if (this.dragGhost) {
      const g = this.dragGhost;
      const gx = this.xForTick(g.tick), gy = this.yForPitch(g.pitch);
      const gw = Math.max(3, g.len * this.pxPerTick - 1);
      ctx.globalAlpha = 0.8;
      ctx.setLineDash([4, 3]);
      ctx.strokeStyle = "#fff";
      roundRect(ctx, Math.max(KEY_W, gx), gy + 0.75, gw, this.semiH - 1.5, 3);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
    }

    // rejected edits: red dashed outlines with an X, hover shows the reason
    this.ghostHits = [];
    for (let i = 0; i < this.ghosts.length; i++) {
      const g = this.ghosts[i];
      const gx = this.xForTick(g.tick), gy = this.yForPitch(g.pitch);
      const gw = Math.max(6, g.len * this.pxPerTick - 1), gh = this.semiH - 1.5;
      if (gx + gw < KEY_W || gx > W || gy + gh < this.gridTop() || gy > H) continue;
      ctx.setLineDash([3, 3]);
      ctx.strokeStyle = "#ff5d6c";
      ctx.lineWidth = 1.5;
      roundRect(ctx, Math.max(KEY_W, gx), gy + 0.75, gw, gh, 3);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.lineWidth = 1;
      ctx.fillStyle = "#ff5d6c";
      ctx.font = "9px -apple-system, Segoe UI, sans-serif";
      ctx.fillText("✕", Math.max(KEY_W, gx) + 2, gy + gh / 2 + 1);
      this.ghostHits.push({ x: gx, y: gy, w: gw, h: gh, i });
    }

    this.drawLane(W);
    this.drawOverview(W, H);
    this.drawKeyboard(H);
    ctx.restore();
  }

  // meta lanes under the ruler: loops / tempo / volume
  private drawLane(W: number) {
    const ctx = this.ctx;
    this.laneMarkerHits = [];
    for (const lane of this.lanes()) {
      const y0 = this.laneTop(lane.key), lh = lane.h;
      ctx.fillStyle = "#10131a";
      ctx.fillRect(KEY_W, y0, W - KEY_W, lh);
      ctx.strokeStyle = "#232838";
      ctx.beginPath(); ctx.moveTo(KEY_W, y0 + lh - 0.5); ctx.lineTo(W, y0 + lh - 0.5); ctx.stroke();
      if (!this.lanesOpen[lane.key]) continue;
      ctx.font = "10px -apple-system, Segoe UI, sans-serif";
      if (lane.key === "loops") this.drawLoopsLane(W, y0, lh);
      else this.drawSettingLane(W, y0, lh, lane.key);
    }
  }

  // tempo shows every track's markers (it is effectively global); volume
  // shows the active track's. Click a marker to edit it in the inspector.
  private drawSettingLane(W: number, y0: number, lh: number, type: string) {
    const ctx = this.ctx;
    if (!this.state) return;
    let lastX = -1e9;
    for (const tr of this.state.tracks) {
      if (type === "volume" && tr.index !== this.activeTrack) continue;
      for (const st of tr.settings) {
        if (st.type !== type) continue;
        const x = this.xForTick(st.tick);
        if (x < KEY_W || x > W) continue;
        const active = tr.index === this.activeTrack;
        ctx.fillStyle = type === "tempo" ? (active ? "#6ea8fe" : "rgba(110,168,254,0.5)") : "#f5c451";
        ctx.beginPath();
        ctx.moveTo(x, y0 + 3); ctx.lineTo(x + 4, y0 + lh / 2); ctx.lineTo(x, y0 + lh - 3); ctx.lineTo(x - 4, y0 + lh / 2);
        ctx.closePath(); ctx.fill();
        if (x - lastX > 34) {
          ctx.fillStyle = "#cfd8ea";
          ctx.fillText(String(st.value), x + 6, y0 + lh / 2);
          lastX = x;
        }
        this.laneMarkerHits.push({ lane: type, x, track: tr.index, setting: st.i });
      }
    }
  }

  private drawLoopsLane(W: number, y0: number, lh: number) {
    const ctx = this.ctx;
    // song loop (trailing GOTO): green end-marker - the driver replays from
    // destTick forever once it reaches this point
    const sl = this.state?.tracks.find((t) => t.index === this.activeTrack)?.songLoop;
    if (sl) {
      const xe = this.xForTick(sl.tick), xs = this.xForTick(sl.destTick);
      if (xe >= KEY_W && xe <= W) {
        ctx.fillStyle = "#7dd87d";
        ctx.fillRect(xe - 1.5, y0 + 1, 3, lh - 2);
        ctx.fillText("⟲ song loops", Math.max(KEY_W, Math.min(xe + 5, W - 70)), y0 + lh / 2);
      }
      if (xs >= KEY_W && xs <= W) {
        ctx.fillStyle = "rgba(125,216,125,0.7)";
        ctx.fillRect(xs - 1, y0 + 1, 2, lh - 2);
      }
    }
    const loops = this.activeLoops();
    for (let i = 0; i < loops.length; i++) {
      const lp = loops[i];
      if (lp.destTick === undefined || lp.destTick === null) continue;
      let x0 = this.xForTick(Math.min(lp.destTick, lp.tick));
      let x1 = this.xForTick(Math.max(lp.destTick, lp.tick));
      if (this.loopEdgeDrag?.loop === i) {
        if (this.loopEdgeDrag.edge === "start") x0 = this.loopEdgeDrag.curX;
        else x1 = this.loopEdgeDrag.curX;
      }
      if (x1 < KEY_W || x0 > W) continue;
      const sel = this.selLoop === i;
      ctx.fillStyle = sel ? "rgba(110,168,254,0.55)" : "rgba(110,168,254,0.25)";
      roundRect(ctx, Math.max(KEY_W, x0), y0 + 3, Math.max(8, x1 - Math.max(KEY_W, x0)), lh - 6, 4);
      ctx.fill();
      ctx.fillStyle = sel ? "#cfe0ff" : "rgba(160,185,230,0.8)";
      ctx.fillRect(x0 - 1.5, y0 + 2, 3, lh - 4);
      ctx.fillRect(x1 - 1.5, y0 + 2, 3, lh - 4);
      if (x1 - x0 > 44) {
        ctx.fillStyle = sel ? "#fff" : "#cfd8ea";
        ctx.fillText(`↻ ×${lp.count}`, Math.max(KEY_W, x0) + 6, y0 + lh / 2);
      }
    }
  }


  // bottom strip: whole-song note minimap, viewport thumb, playhead tick
  private drawOverview(W: number, H: number) {
    const ctx = this.ctx;
    const y0 = H - SCROLL_H;
    const viewPx = W - KEY_W;
    const total = Math.max(1, this.maxTick());
    ctx.fillStyle = "#0c0e13";
    ctx.fillRect(KEY_W, y0, viewPx, SCROLL_H);
    if (this.state) {
      for (const tr of this.state.tracks) {
        if (this.hidden.has(tr.index)) continue;
        const active = tr.index === this.activeTrack;
        ctx.fillStyle = active ? "#e8896a" : "rgba(130,140,165,0.45)";
        for (const n of tr.notes) {
          if (n.rest) continue;
          const nx = KEY_W + (n.tick / total) * viewPx;
          const nw = Math.max(1, (n.len / total) * viewPx);
          // pitch squeezed into the strip so the minimap shows contour
          const ny = y0 + 2 + (1 - (n.pitch - MIN_PITCH) / (MAX_PITCH - MIN_PITCH)) * (SCROLL_H - 5);
          ctx.fillRect(nx, ny, nw, 1.5);
        }
      }
    }
    // viewport thumb
    const totalPx = Math.max(1, total * this.pxPerTick);
    const tx = KEY_W + (this.scrollX / totalPx) * viewPx;
    const tw = Math.max(12, (viewPx / totalPx) * viewPx);
    ctx.fillStyle = "rgba(110,168,254,0.22)";
    ctx.fillRect(tx, y0, Math.min(tw, viewPx), SCROLL_H);
    ctx.strokeStyle = "rgba(110,168,254,0.85)";
    ctx.strokeRect(tx + 0.5, y0 + 0.5, Math.min(tw, viewPx) - 1, SCROLL_H - 1);
    // playhead position in the song
    if (this.playheadTick >= 0) {
      const px = KEY_W + (this.playheadTick / total) * viewPx;
      ctx.fillStyle = "#ff5d6c";
      ctx.fillRect(px, y0, 1.5, SCROLL_H);
    }
  }

  private drawNote(track: number, n: Note, color: string, active: boolean) {
    const ctx = this.ctx;
    const x = this.xForTick(n.tick), w = Math.max(3, n.len * this.pxPerTick - 1);
    const y = this.yForPitch(n.pitch), h = this.semiH - 1.5;
    if (x + w < KEY_W || x > this.W() || y + h < HEADER_H || y > this.H()) return;
    const sel = (this.selected && this.selected.track === track && this.selected.note === n.i)
      || this.multiSel.some((s) => s.track === track && s.note === n.i);
    const vx = Math.max(KEY_W, x), vw = w - (x < KEY_W ? KEY_W - x : 0);
    if (n.loopRepeat) {
      // repeated pass of a loop: a dim plate behind a dimmer note marks it
      // as an echo of the first pass, scoped to the note itself
      ctx.globalAlpha = active ? 0.45 : 0.18;
      ctx.fillStyle = "#3a4152";
      roundRect(ctx, vx - 2, y - 1.25, vw + 4, h + 4, 5); ctx.fill();
      ctx.globalAlpha = active ? 0.55 : 0.2;
    } else {
      ctx.globalAlpha = active ? 1 : 0.3;
    }
    ctx.fillStyle = color;
    roundRect(ctx, vx, y + 0.75, vw, h, 3); ctx.fill();
    if (sel) { ctx.globalAlpha = 1; ctx.strokeStyle = "#fff"; ctx.lineWidth = 2; roundRect(ctx, vx, y + 0.75, vw, h, 3); ctx.stroke(); ctx.lineWidth = 1; }
    ctx.globalAlpha = 1;
    // every visible note is clickable; overlaps resolve to the topmost drawn
    this.hits.push({ track, note: n.i, x, y, w, h });
  }

  private drawKeyboard(H: number) {
    const ctx = this.ctx;
    ctx.fillStyle = "#0f1116"; ctx.fillRect(0, HEADER_H, KEY_W, H - HEADER_H);
    // lane gutter: per-lane collapse/expand toggles
    ctx.font = "9px -apple-system, Segoe UI, sans-serif";
    for (const lane of this.lanes()) {
      const y0 = this.laneTop(lane.key);
      ctx.fillStyle = "#8a93a8";
      ctx.fillText((this.lanesOpen[lane.key] ? "▾ " : "▸ ") + lane.key, 8, y0 + lane.h / 2);
    }
    for (let p = MIN_PITCH; p <= MAX_PITCH; p++) {
      const y = this.yForPitch(p);
      if (y + this.semiH < this.gridTop() || y > H) continue;
      const black = [1, 3, 6, 8, 10].includes(((p % 12) + 12) % 12);
      ctx.fillStyle = black ? "#191c24" : "#e8ebf2";
      ctx.fillRect(6, y + 0.25, KEY_W - 6, this.semiH - 0.5);
      if (p % 12 === 0) { ctx.fillStyle = "#5a6376"; ctx.font = "10px -apple-system, Segoe UI, sans-serif"; ctx.fillText("C" + (p / 12 - 1), KEY_W - 22, y + this.semiH / 2); }
    }
    ctx.fillStyle = "#0f1116"; ctx.fillRect(0, 0, KEY_W, HEADER_H);
    ctx.strokeStyle = "#2f3542"; ctx.beginPath(); ctx.moveTo(KEY_W, 0); ctx.lineTo(KEY_W, H); ctx.stroke();
  }
}

function roundRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number) {
  r = Math.min(r, w / 2, h / 2);
  ctx.beginPath(); ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
}
function clamp(v: number, lo: number, hi: number) { return Math.max(lo, Math.min(hi, v)); }
