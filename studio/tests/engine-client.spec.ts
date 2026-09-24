import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  // Preview data is fetched after document load. Wait for its network response
  // under the startup budget before applying the ordinary UI assertion budget.
  const sample = page.waitForResponse(r => new URL(r.url()).pathname === '/sample.json', {timeout: 90_000});
  await page.goto('/');
  const response = await sample;
  expect(response.status()).toBe(200);
  await response.finished();
  await expect(page.locator('#song-title')).toContainText('Hamlet');
});

test('native engine forwards commands, dialog filters and cancellation', async ({ page }) => {
  const result = await page.evaluate(async () => {
    const calls: any[] = [];
    let answer: any = { ok: true, state: { marker: 42 } };
    (window as any).__TAURI__ = { core: {
      invoke: async (...args: any[]) => { calls.push(args); return answer; },
      convertFileSrc: (p: string) => 'asset:' + p,
    }};
    const { TauriEngine } = await import('/src/engine.ts?client-contract');
    const engine = new TauriEngine();
    const state = await engine.open('test.spc');
    answer = 'chosen.gtb';
    const chosen = await engine.saveDialog([{ name: 'Session', extensions: ['gtb'] }], 'song.gtb');
    answer = null;
    const cancelled = await engine.openDialog();
    answer = { ok: true, slots: [{slot: 2, size: 100}] };
    const slots = await engine.listRomSongs('test.smc');
    answer = {ok:true, wav:'test.wav'};
    const audio = await engine.render(3, [1, 4]);
    return { calls, state, chosen, cancelled, slots, audio };
  });
  expect(result.state).toEqual({ marker: 42 });
  expect(result.chosen).toBe('chosen.gtb');
  expect(result.cancelled).toBeNull();
  expect(result.slots).toEqual([{slot:2,size:100}]);
  expect(result.audio).toMatch(/^asset:test.wav\?v=\d+$/);
  expect(result.calls).toEqual([
    ['engine_request', {request:{cmd:'open',path:'test.spc'}}],
    ['plugin:dialog|save', {options:{filters:[{name:'Session',extensions:['gtb']}],defaultPath:'song.gtb'}}],
    ['plugin:dialog|open', {options:{multiple:false,directory:false,filters:[{name:'SNES SPC',extensions:['spc']}]}}],
    ['engine_request', {request:{cmd:'listRomSongs',path:'test.smc'}}],
    ['engine_request', {request:{cmd:'render',seconds:3,mute:[1,4]}}],
  ]);
});

test('engine errors surface and recovery is bounded to one retry', async ({ page }) => {
  const result = await page.evaluate(async () => {
    let mode = 'validation', calls = 0, recovered = 0;
    (window as any).__TAURI__ = {core:{invoke:async () => {
      calls++;
      if (mode === 'validation') return {ok:false,error:'invalid note'};
      if (mode === 'dead') throw 'engine restarted: engine closed';
      return {ok:true,state:{ready:true}};
    }}};
    const {TauriEngine} = await import('/src/engine.ts?client-errors');
    const e = new TauriEngine();
    const errors: string[] = [];
    for (const operation of [()=>e.send('setNote'),()=>e.request('setNote'),()=>e.render(1,[])]) {
      try { await operation(); } catch (err) { errors.push(String(err)); }
    }
    const preview = await e.renderNote(0, 1);
    mode = 'dead';
    e.onEngineRestart = async () => { recovered++; mode='ready'; };
    const recoveredState = await e.request('state');
    mode = 'dead';
    e.onEngineRestart = async () => { recovered++; };
    try { await e.request('state'); } catch (err) { errors.push(String(err)); }
    return {errors,preview,recoveredState,recovered,calls};
  });
  expect(result.errors).toEqual(['Error: invalid note','Error: invalid note','Error: invalid note','Error: engine restarted: engine closed']);
  expect(result.preview).toBeNull();
  expect(result.recoveredState).toEqual({ok:true,state:{ready:true}});
  expect(result.recovered).toBe(2);
  expect(result.calls).toBe(8);
});

test('send, render and preview recover too; a lost song is reported plainly until one is loaded', async ({ page }) => {
  const result = await page.evaluate(async () => {
    let dead = false;
    const sent: string[] = [];
    (window as any).__TAURI__ = {core:{invoke:async (_c: string, {request}: any) => {
      sent.push(request.cmd);
      if (dead) { dead = false; throw 'engine restarted: engine closed'; }
      return {ok:true,state:{cmd:request.cmd},wav:'x.wav'};
    }, convertFileSrc: (p: string) => 'asset:' + p}};
    const {TauriEngine} = await import('/src/engine.ts?client-recovery');
    const e = new TauriEngine();
    const reloads: string[] = [];
    e.onEngineRestart = async (cmd: string) => { reloads.push(cmd); await e.request('openSession', {path:'a.gtb'}); };
    dead = true; const edited = await e.send('insertNote', {track:0});
    dead = true; const rendered = await e.render(1, []);
    // loading a whole song needs no reload: it just retries on the fresh engine
    dead = true; const fresh = await e.send('new');
    // recovery refused (e.g. unsaved changes): later song commands explain, not "no song open"
    e.onEngineRestart = async () => { throw new Error('unsaved changes cannot be recovered'); };
    const errors: string[] = [];
    dead = true; try { await e.send('eraseNote'); } catch (err) { errors.push(String(err)); }
    try { await e.request('save'); } catch (err) { errors.push(String(err)); }
    const slots = await e.request('listRomSongs', {path:'r.smc'});
    const opened = await e.open('b.spc');
    const after = await e.send('state');
    return {edited, rendered, fresh, errors, slotsOk: slots.ok, opened, after, reloads, sent};
  });
  expect(result.edited).toEqual({cmd:'insertNote'});
  expect(result.rendered).toMatch(/^asset:x.wav\?v=\d+$/);
  expect(result.fresh).toEqual({cmd:'new'});
  expect(result.errors).toEqual(['Error: unsaved changes cannot be recovered', 'Error: unsaved changes cannot be recovered']);
  expect(result.slotsOk).toBe(true);
  expect(result.opened).toEqual({cmd:'open'});
  expect(result.after).toEqual({cmd:'state'});
  expect(result.reloads).toEqual(['insertNote', 'render']);
  expect(result.sent).toEqual([
    'insertNote', 'openSession', 'insertNote',
    'render', 'openSession', 'render',
    'new', 'new',
    'eraseNote',
    'listRomSongs', 'open', 'state',
  ]);
});

test('preview backend rejects file mutations and cannot render audio', async ({ page }) => {
  const result = await page.evaluate(async () => {
    const {StaticEngine} = await import('/src/engine.ts');
    const e = new StaticEngine();
    let error = '';
    try { await e.request('save'); } catch (err) { error=String(err); }
    const s = await e.open('');
    return {error, unchanged:(await e.send('eraseNote')).tracks.length===s.tracks.length,
      readOnly:e.readOnly,canPlay:e.canPlay,render:await e.render(),open:await e.openDialog(),save:await e.saveDialog()};
  });
  expect(result).toEqual({error:'Error: not available in preview',unchanged:true,readOnly:true,canPlay:false,render:null,open:null,save:null});
});
