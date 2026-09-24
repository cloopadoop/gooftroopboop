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
  // Tempo changes over the song, sorted by tick, first entry at tick 0. Older
  // engines omit it; tempoBpm then applies to the whole song.
  tempoMap?: { tick: number; bpm: number }[];
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
  // Called with the failed command after the sidecar died and the Rust shell
  // will start a fresh (empty) one: reload the song or throw to explain why not.
  onEngineRestart: ((cmd: string) => Promise<void>) | null;
  listRomSongs(path: string): Promise<RomSlot[]>;
  wavUrl(path: string): string;
  openDialog(filters?: { name: string; extensions: string[] }[]): Promise<string | null>;
  saveDialog(filters?: { name: string; extensions: string[] }[], defaultName?: string): Promise<string | null>;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
const tauri = (window as any).__TAURI__;

async function invoke(request: Record<string, unknown>): Promise<any> {
  try {
    return await tauri.core.invoke("engine_request", { request });
  } catch (err) {
    // Tauri rejects with the Rust error string; normalize to Error.
    throw err instanceof Error ? err : new Error(String(err));
  }
}

// The Rust shell prefixes transport failures with this once it has dropped the
// dead sidecar; the next request goes to a fresh engine with no song loaded.
export const ENGINE_RESTARTED = "engine restarted";
export function isEngineRestart(err: unknown): boolean {
  return (err instanceof Error ? err.message : String(err)).startsWith(ENGINE_RESTARTED);
}
// Commands that load a whole new song: on a fresh engine they simply retry.
const LOADS_SONG = new Set(["open", "openSession", "openRom", "new", "importMidi", "importAsm"]);
// Commands that never touch the open song.
const SONG_FREE = new Set(["listRomSongs", "inspectAsm", "inspectAsmRepair"]);

export class TauriEngine implements Engine {
  readonly readOnly = false;
  readonly canPlay = true;
  // Set when a restart lost the in-memory song and it could not be reloaded:
  // song commands fail with this plain explanation until a song is loaded,
  // instead of the fresh engine's bare "no song open".
  private songLost: string | null = null;

  async open(path: string): Promise<SongState> {
    // Empty path = engine opens its bundled default soundbank song.
    return this.send("open", { path });
  }
  async send(cmd: string, params: Record<string, unknown> = {}): Promise<SongState> {
    const resp = await this.call(cmd, params);
    if (!resp.ok) throw new Error(resp.error || "engine error");
    return resp.state as SongState;
  }

  // Every engine command goes through here so a crashed sidecar is recovered
  // (at most one retry) no matter which entry point hit it.
  private async call(cmd: string, params: Record<string, unknown>): Promise<any> {
    const needsSong = !LOADS_SONG.has(cmd) && !SONG_FREE.has(cmd);
    if (needsSong && this.songLost) throw new Error(this.songLost);
    let resp;
    try {
      resp = await invoke({ cmd, ...params });
    } catch (err) {
      if (!isEngineRestart(err)) throw err;
      if (needsSong) {
        try {
          if (!this.onEngineRestart) throw new Error("The audio engine restarted and the song in memory was lost. Reopen a saved file or start a new song.");
          await this.onEngineRestart(cmd);
        } catch (why) {
          this.songLost = (why as Error).message;
          throw why;
        }
      }
      try {
        resp = await invoke({ cmd, ...params });
      } catch (again) {
        if (needsSong && isEngineRestart(again)) {
          this.songLost = "The audio engine stopped again right after restarting. Reopen the song to continue.";
        }
        throw again;
      }
    }
    if (LOADS_SONG.has(cmd) && resp?.ok) this.songLost = null;
    return resp;
  }
  // Cache-bust: render outputs reuse the same temp filename, and the webview
  // caches asset URLs — without the query param every preview replays stale audio.
  private fileUrl(path: string): string {
    return tauri.core.convertFileSrc(path) + "?v=" + Date.now();
  }
  async render(seconds: number, mute: number[]): Promise<string | null> {
    const resp = await this.call("render", { seconds, mute });
    if (!resp.ok) throw new Error(resp.error || "render failed");
    return this.fileUrl(resp.wav);
  }
  async renderNote(track: number, note: number): Promise<string | null> {
    const resp = await this.call("renderNote", { track, note });
    if (!resp.ok) return null;
    return this.fileUrl(resp.wav);
  }
  wavUrl(path: string): string { return this.fileUrl(path); }
  // Set by main.ts: reopen the current song after the sidecar dies and the
  // Rust shell respawns it (the fresh process has no song loaded).
  onEngineRestart: ((cmd: string) => Promise<void>) | null = null;

  async request(cmd: string, params: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    const resp = await this.call(cmd, params);
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
  onEngineRestart: ((cmd: string) => Promise<void>) | null = null;
  async request(): Promise<Record<string, unknown>> { throw new Error("not available in preview"); }
  async listRomSongs(): Promise<RomSlot[]> { return []; }
  wavUrl(path: string): string { return path; }
  async openDialog(): Promise<string | null> { return null; }
  async saveDialog(): Promise<string | null> { return null; }
}

export function makeEngine(): Engine {
  return tauri ? new TauriEngine() : new StaticEngine();
}
