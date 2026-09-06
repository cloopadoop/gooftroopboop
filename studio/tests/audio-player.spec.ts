import { test, expect } from '@playwright/test';

test('transport scheduling, pause, seek, loop wrapping and natural end', async ({ page }) => {
  await page.goto('/');
  const result = await page.evaluate(async () => {
    const sources: any[] = [], ticks: number[] = [];
    let ended=0, raf: (()=>void) | null=null;
    const ctx: any = {currentTime:0,state:'running',outputLatency:0.1,destination:{},
      decodeAudioData:async()=>({duration:10}),
      createBufferSource:()=>{
        const source:any={connect(){},disconnect(){},stop(){this.stopped=true;},start(...args:any[]){this.startArgs=args;}};
        sources.push(source);return source;
      }};
    (window as any).AudioContext=function(){return ctx;};
    window.fetch=async()=>({arrayBuffer:async()=>new ArrayBuffer(0)}) as any;
    window.requestAnimationFrame=(cb:any)=>{raf=cb;return 1;};
    window.cancelAnimationFrame=()=>{raf=null;};
    const {AudioPlayer}=await import('/src/audio.ts');
    const p=new AudioPlayer();p.onTick=(t:number)=>ticks.push(t);p.onEnded=()=>ended++;
    await p.load('test.wav');p.play(2);
    const scheduled=sources[0].startArgs;
    ctx.currentTime=1.03;raf!();
    const heard=ticks.at(-1);p.pause();const paused=p.current;
    p.seek(99);const clamped=p.current;
    p.setLoopPoints(2,4);p.setLoop(true);p.play(3);
    ctx.currentTime=4.06;raf!();const loopTick=ticks.at(-1);
    p.pause();const pausedLoop=p.current;
    p.play();const resumeOffset=sources.at(-1).startArgs[1];
    p.setLoop(false);sources.at(-1).onended();
    const afterEnd={playing:p.isPlaying,current:p.current,ended};
    p.stop();
    return {scheduled,heard,paused,clamped,loopTick,pausedLoop,resumeOffset,afterEnd,lastTick:ticks.at(-1),
      loopStart:sources[1].loopStart,loopEnd:sources[1].loopEnd,stopped:sources[0].stopped};
  });
  expect(result.scheduled).toEqual([.03,2]);
  expect(result.heard).toBeCloseTo(2.9);
  expect(result.paused).toBeCloseTo(3);
  expect(result.clamped).toBe(10);
  expect(result.loopTick).toBeCloseTo(3.9);
  expect(result.pausedLoop).toBeCloseTo(2);
  expect(result.resumeOffset).toBeCloseTo(2);
  expect(result.afterEnd).toEqual({playing:false,current:0,ended:1});
  expect(result.lastTick).toBe(0);
  expect(result.loopStart).toBe(2);expect(result.loopEnd).toBe(4);expect(result.stopped).toBe(true);
});

test('real WebAudio decodes a WAV and replaces note previews', async ({ page }) => {
  await page.goto('/');
  const result = await page.evaluate(async () => {
    const {AudioPlayer}=await import('/src/audio.ts');
    const bytes=new ArrayBuffer(44+1600), d=new DataView(bytes);
    const text=(offset:number,s:string)=>[...s].forEach((c,i)=>d.setUint8(offset+i,c.charCodeAt(0)));
    text(0,'RIFF');d.setUint32(4,1636,true);text(8,'WAVE');text(12,'fmt ');
    d.setUint32(16,16,true);d.setUint16(20,1,true);d.setUint16(22,1,true);d.setUint32(24,8000,true);
    d.setUint32(28,16000,true);d.setUint16(32,2,true);d.setUint16(34,16,true);text(36,'data');d.setUint32(40,1600,true);
    for(let i=0;i<800;i++)d.setInt16(44+i*2,Math.sin(i*Math.PI*.11)*8000,true);
    const url=URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}));
    const p=new AudioPlayer();await p.load(url);await p.preview(url);await p.preview(url);p.stopPreview();
    p.play();p.stop();URL.revokeObjectURL(url);
    return {duration:p.duration,playing:p.isPlaying};
  });
  expect(result.duration).toBeCloseTo(.1,2);expect(result.playing).toBe(false);
});
