// Engine client. Two backends:
//  - TauriEngine: talks to the bundled gtb-engine sidecar via a Tauri command,
//    plus native file dialogs and asset URLs for rendered audio.
//  - StaticEngine: loads a pre-exported snapshot (public/sample.json) read-only
//    for browser/dev preview where no engine process is available.

export interface Note {
  i: number; tick: number; len: number; dur: number;
  pitch: number; rest: boolean; program: number; loopRepeat: boolean;
}
export interface Setting { i: number; tick: number; type: string; value: number; value2: number; desc: string; }
export interface Loop { i: number; tick: number; slot: number; count: number; destTick: number; }
export interface Track {
  index: number; instrument: string; noteCount: number;
  notes: Note[]; settings: Setting[]; loops: Loop[];
  // trailing GOTO: the driver jumps from `tick` back to `destTick` forever
  songLoop?: { tick: number; destTick: number };
}
export interface Program { program: number; name: string; }
export interface SongState {
  source: string; writable: boolean; canUndo: boolean; canRedo: boolean;
  programs: Program[]; budget?: { used: number; total: number }; tracks: Track[];
  tempoBpm?: number; ppqn?: number;
}

export interface RomSlot { slot: number; size: number; }

export interface Engine {
  readonly readOnly: boolean;
  readonly canPlay: boolean;
  open(path: string): Promise<SongState>;
  send(cmd: string, params?: Record<string, unknown>): Promise<SongState>;
  render(seconds: number, mute: number[]): Promise<string | null>;
  renderNote(track: number, note: number): Promise<string | null>;
  // Raw request without state-refresh semantics (import/export commands).
  request(cmd: string, params?: Record<string, unknown>): Promise<Record<string, unknown>>;
  onEngineRestart: (() => Promise<void>) | null;
  listRomSongs(path: string): Promise<RomSlot[]>;
  wavUrl(path: string): string;
  openDialog(filters?: { name: string; extensions: string[] }[]): Promise<string | null>;
  saveDialog(filters?: { name: string; extensions: string[] }[], defaultName?: string): Promise<string | null>;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
const tauri = (window as any).__TAURI__;

async function invoke(request: Record<string, unknown>): Promise<any> {
  return tauri.core.invoke("engine_request", { request });
}

export class TauriEngine implements Engine {
  readonly readOnly = false;
  readonly canPlay = true;

  async open(path: string): Promise<SongState> {
    // Empty path = engine opens its bundled default soundbank song.
    return this.send("open", { path });
  }
  async send(cmd: string, params: Record<string, unknown> = {}): Promise<SongState> {
    const resp = await invoke({ cmd, ...params });
    if (!resp.ok) throw new Error(resp.error || "engine error");
    return resp.state as SongState;
  }
  // Cache-bust: render outputs reuse the same temp filename, and the webview
  // caches asset URLs — without the query param every preview replays stale audio.
  private fileUrl(path: string): string {
    return tauri.core.convertFileSrc(path) + "?v=" + Date.now();
  }
  async render(seconds: number, mute: number[]): Promise<string | null> {
    const resp = await invoke({ cmd: "render", seconds, mute });
    if (!resp.ok) throw new Error(resp.error || "render failed");
    return this.fileUrl(resp.wav);
  }
  async renderNote(track: number, note: number): Promise<string | null> {
    const resp = await invoke({ cmd: "renderNote", track, note });
    if (!resp.ok) return null;
    return this.fileUrl(resp.wav);
  }
  wavUrl(path: string): string { return this.fileUrl(path); }
  // Set by main.ts: reopen the current song after the sidecar dies and the
  // Rust shell respawns it (the fresh process has no song loaded).
  onEngineRestart: (() => Promise<void>) | null = null;

  async request(cmd: string, params: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    let resp;
    try {
      resp = await invoke({ cmd, ...params });
    } catch (err) {
      const msg = String(err);
      if (this.onEngineRestart && /pipe|stdin|engine|died|closed|broken/i.test(msg)) {
        await this.onEngineRestart();
        resp = await invoke({ cmd, ...params });
      } else {
        throw err instanceof Error ? err : new Error(msg);
      }
    }
    if (!resp.ok) throw new Error(resp.error || cmd + " failed");
    return resp;
  }
  async listRomSongs(path: string): Promise<RomSlot[]> {
    const resp = await this.request("listRomSongs", { path });
    return (resp.slots as RomSlot[]) ?? [];
  }
  // Call the dialog plugin through the injected global API — the npm module
  // path broke silently in the webview, so invoke the plugin commands directly.
  async openDialog(filters = [{ name: "SNES SPC", extensions: ["spc"] }]): Promise<string | null> {
    const sel = await tauri.core.invoke("plugin:dialog|open", {
      options: { multiple: false, directory: false, filters },
    });
    return (sel as string) || null;
  }
  async saveDialog(filters = [{ name: "SNES SPC", extensions: ["spc"] }], defaultName?: string): Promise<string | null> {
    const sel = await tauri.core.invoke("plugin:dialog|save", {
      options: { filters, defaultPath: defaultName },
    });
    return (sel as string) || null;
  }
}

export class StaticEngine implements Engine {
  readonly readOnly = true;
  readonly canPlay = false;
  private state: SongState | null = null;
  async open(_path: string): Promise<SongState> {
    const resp = await fetch("/sample.json").then((r) => r.json());
    this.state = resp.state as SongState;
    return this.state;
  }
  async send(_cmd: string): Promise<SongState> {
    if (!this.state) throw new Error("no song");
    return this.state;
  }
  async render(): Promise<string | null> { return null; }
  async renderNote(): Promise<string | null> { return null; }
  onEngineRestart: (() => Promise<void>) | null = null;
  async request(): Promise<Record<string, unknown>> { throw new Error("not available in preview"); }
  async listRomSongs(): Promise<RomSlot[]> { return []; }
  wavUrl(path: string): string { return path; }
  async openDialog(): Promise<string | null> { return null; }
  async saveDialog(): Promise<string | null> { return null; }
}

export function makeEngine(): Engine {
  return tauri ? new TauriEngine() : new StaticEngine();
}
