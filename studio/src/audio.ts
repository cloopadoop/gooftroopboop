// WebAudio playback for engine-rendered WAV. Handles full-song transport
// (play/pause/stop/seek) plus fire-and-forget note previews.

export class AudioPlayer {
  private ctx: AudioContext;
  private buffer: AudioBuffer | null = null;
  private src: AudioBufferSourceNode | null = null;
  private startedAt = 0; // ctx time when playback (re)started
  private offset = 0; // seconds into buffer at start
  private playing = false;
  private loop = false;
  // song loop points in seconds (from the sequence's GOTO commands); when
  // set, looping cycles [loopStart, loopEnd) instead of the whole buffer
  private loopStart = 0;
  private loopEnd = 0;
  onTick: ((seconds: number, duration: number) => void) | null = null;
  onEnded: (() => void) | null = null;
  private raf = 0;

  constructor() {
    this.ctx = new AudioContext();
  }

  get duration(): number { return this.buffer?.duration ?? 0; }
  get isPlaying(): boolean { return this.playing; }
  get current(): number {
    if (!this.playing) return this.offset;
    // before the scheduled start instant, nothing has played yet
    const raw = this.offset + Math.max(0, this.ctx.currentTime - this.startedAt);
    if (!this.loop) return raw;
    if (this.loopEnd > this.loopStart && this.loopEnd <= this.duration && raw >= this.loopEnd) {
      return this.loopStart + (raw - this.loopStart) % (this.loopEnd - this.loopStart);
    }
    return this.loopEnd > this.loopStart ? raw : (this.duration ? raw % this.duration : raw);
  }
  // What the listener is hearing right now: playback position minus the
  // audio pipeline's output latency. This is what the playhead must show,
  // otherwise the cursor leads the sound by the device/driver buffer.
  private get heard(): number {
    const lat = (this.ctx as AudioContext & { outputLatency?: number }).outputLatency
      ?? this.ctx.baseLatency ?? 0;
    // Wrap only after subtracting latency, otherwise the cursor jumps outside
    // the loop for one output buffer at each boundary.
    return this.offset + (this.playing ? Math.max(0, this.ctx.currentTime - this.startedAt) : 0) - lat;
  }
  setLoop(on: boolean) { this.loop = on; if (this.src) this.applyLoop(this.src); }
  setLoopPoints(startSec: number, endSec: number) {
    this.loopStart = Math.max(0, startSec);
    this.loopEnd = Math.max(0, endSec);
    if (this.src) this.applyLoop(this.src);
  }
  private applyLoop(src: AudioBufferSourceNode) {
    src.loop = this.loop;
    if (this.loop && this.loopEnd > this.loopStart && this.buffer &&
        this.loopEnd <= this.buffer.duration) {
      src.loopStart = this.loopStart;
      src.loopEnd = this.loopEnd;
    } else {
      src.loopStart = 0;
      src.loopEnd = 0;
    }
  }

  async load(url: string): Promise<void> {
    const data = await fetch(url).then((r) => r.arrayBuffer());
    this.buffer = await this.ctx.decodeAudioData(data);
    this.offset = 0;
  }

  play(fromSeconds?: number) {
    if (!this.buffer) return;
    this.stopSource();
    if (fromSeconds !== undefined) this.offset = Math.max(0, Math.min(fromSeconds, this.duration));
    const src = this.ctx.createBufferSource();
    src.buffer = this.buffer;
    this.applyLoop(src);
    src.connect(this.ctx.destination);
    src.onended = () => {
      if (!this.loop && this.playing) {
        this.playing = false;
        this.offset = 0;
        cancelAnimationFrame(this.raf);
        this.onEnded?.();
      }
    };
    // schedule at a definite instant so position math is exact, instead of
    // start(0) which begins "sometime soon" after currentTime
    const when = this.ctx.currentTime + 0.03;
    src.start(when, this.offset);
    this.src = src;
    this.startedAt = when;
    this.playing = true;
    if (this.ctx.state === "suspended") this.ctx.resume();
    this.tickLoop();
  }

  pause() {
    if (!this.playing) return;
    this.offset = this.current;
    this.stopSource();
    this.playing = false;
    cancelAnimationFrame(this.raf);
  }

  stop() {
    this.stopSource();
    this.playing = false;
    this.offset = 0;
    cancelAnimationFrame(this.raf);
    this.onTick?.(0, this.duration);
  }

  seek(seconds: number) {
    const wasPlaying = this.playing;
    this.offset = Math.max(0, Math.min(seconds, this.duration));
    if (wasPlaying) this.play(this.offset);
    else this.onTick?.(this.offset, this.duration);
  }

  private stopSource() {
    if (this.src) {
      try { this.src.onended = null; this.src.stop(); } catch { /* already stopped */ }
      this.src.disconnect();
      this.src = null;
    }
  }

  private tickLoop = () => {
    if (!this.playing) return;
    let t = Math.max(0, this.heard);
    if (this.loop && this.loopEnd > this.loopStart && t > this.loopEnd) {
      t = this.loopStart + (t - this.loopStart) % (this.loopEnd - this.loopStart);
    } else if (this.loop && this.duration > 0) {
      t = t % this.duration;
    }
    this.onTick?.(t, this.duration);
    this.raf = requestAnimationFrame(this.tickLoop);
  };

  private previewSrc: AudioBufferSourceNode | null = null;
  private previewGain: GainNode | null = null;

  // One-shot note preview from a rendered WAV url. A new preview replaces the
  // previous one; stopPreview() cuts it early (e.g. mouse released).
  async preview(url: string) {
    const data = await fetch(url).then((r) => r.arrayBuffer());
    const buf = await this.ctx.decodeAudioData(data);
    this.stopPreview();
    const s = this.ctx.createBufferSource();
    s.buffer = buf;
    // through a gain node so an early cut can release without clicking
    const g = this.ctx.createGain();
    s.connect(g); g.connect(this.ctx.destination);
    if (this.ctx.state === "suspended") await this.ctx.resume();
    s.start();
    this.previewSrc = s; this.previewGain = g;
  }

  stopPreview() {
    const s = this.previewSrc, g = this.previewGain;
    this.previewSrc = null; this.previewGain = null;
    if (!s || !g) return;
    g.gain.setValueAtTime(g.gain.value, this.ctx.currentTime);
    g.gain.linearRampToValueAtTime(0, this.ctx.currentTime + 0.05);
    setTimeout(() => { try { s.stop(); } catch { /* ended */ } }, 80);
  }
}
