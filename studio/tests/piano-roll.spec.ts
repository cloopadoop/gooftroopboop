import {test, expect, type Page} from '@playwright/test';

// Exercise the real canvas component with deterministic song data. Callbacks
// are the component boundary; native tests separately cover engine mutations.
test.beforeEach(async ({page})=>{
  await page.goto('/');
  await expect(page.locator('#song-title')).toContainText('Hamlet');
  await page.evaluate(async()=>{
    const {PianoRoll}=await import('/src/pianoroll.ts');
    const host=document.createElement('div');
    host.style.cssText='position:fixed;inset:0;width:900px;height:600px;z-index:9999';
    const canvas=document.createElement('canvas');canvas.id='test-roll';host.append(canvas);document.body.append(host);
    const calls:any[]=[];const w=window as any;w.rollCalls=calls;
    const callbacks=new Proxy({}, {get:(_t,name)=> (...args:any[])=>calls.push({name,args})});
    const roll=new PianoRoll(canvas,callbacks);
    const note=(i:number,tick:number,pitch:number)=>({i,tick,pitch,len:48,dur:48,program:1,rest:false,loopRepeat:false});
    roll.setState({source:'synthetic',writable:true,canUndo:false,canRedo:false,programs:[],tracks:[
      {index:0,instrument:'test',noteCount:2,notes:[note(0,48,96),note(1,144,92)],
        settings:[{i:2,tick:240,type:'tempo',value:100,value2:0,desc:'tempo'}],
        loops:[{i:3,slot:0,destTick:48,tick:192,count:2}]},
      {index:1,instrument:'test',noteCount:1,notes:[note(0,288,88)],settings:[],loops:[]}
    ]},0,new Set());
    w.testRoll=roll;
  });
});

async function point(page:Page,tick:number,pitch:number){
  return page.evaluate(({tick,pitch})=>{
    const r=(window as any).testRoll;
    return {x:r.xForTick(tick),y:r.yForPitch(pitch)+3};
  },{tick,pitch});
}
async function drag(page:Page,a:{x:number,y:number},b:{x:number,y:number}){
  await page.mouse.move(a.x,a.y);await page.mouse.down();await page.mouse.move(b.x,b.y,{steps:4});await page.mouse.up();
}
async function calls(page:Page,name:string){return page.evaluate(name=>(window as any).rollCalls.filter((c:any)=>c.name===name).map((c:any)=>c.args),name);}

test('select, preview and move note with snapped time and pitch',async({page})=>{
  const p=await point(page,54,96);
  await drag(page,p,{x:p.x+24,y:p.y-24});
  expect(await calls(page,'onSelectNote')).toEqual([[0,0]]);
  expect(await calls(page,'onPreview')).toEqual([[0,0]]);
  expect(await calls(page,'onMoveNote')).toEqual([[0,0,96,98]]);
  expect(await calls(page,'onPreviewEnd')).toHaveLength(1);
});

test('right edge resizes without moving the note',async({page})=>{
  const p=await point(page,94,96);
  await drag(page,p,{x:p.x+13,y:p.y});
  expect(await calls(page,'onResizeNote')).toEqual([[0,0,72]]);
  expect(await calls(page,'onMoveNote')).toEqual([]);
});

test('add-note drag encodes duration and active channel',async({page})=>{
  await page.evaluate(()=>(window as any).testRoll.setAddMode(true));
  const p=await point(page,384,100);
  await drag(page,p,{x:p.x+36,y:p.y});
  expect(await calls(page,'onAddNote')).toEqual([[0,384,100,72]]);
});

test('marquee selection copies notes and moves them as a group',async({page})=>{
  const a=await point(page,24,98),b=await point(page,204,90);
  await drag(page,a,b);
  const selected=await page.evaluate(()=>(window as any).testRoll.getSelectedNotes());
  expect(selected).toEqual([{track:0,tick:48,pitch:96,len:48},{track:0,tick:144,pitch:92,len:48}]);
  const p=await point(page,54,96);await drag(page,p,{x:p.x+24,y:p.y-12});
  expect(await calls(page,'onMoveNotes')).toEqual([[selected.map(({len,...note})=>note),48,1]]);
});

test('ruler loop creation and loop-edge resizing',async({page})=>{
  await page.keyboard.down('Shift');
  await drag(page,{x:186,y:12},{x:234,y:12});
  await page.keyboard.up('Shift');
  expect(await calls(page,'onCreateLoop')).toEqual([[0,240,336]]);
  await drag(page,{x:162,y:35},{x:186,y:35});
  expect(await calls(page,'onLoopResize')).toEqual([[0,0,48,240,2]]);
});

test('piano glissando previews pitches and stops on release',async({page})=>{
  const p=await point(page,0,96);
  await drag(page,{x:30,y:p.y},{x:30,y:p.y-24});
  const pitches=(await calls(page,'onKeyPreview')).flat();
  expect(pitches[0]).toBe(96);expect(pitches.at(-1)).toBe(98);
  expect(await calls(page,'onPreviewEnd')).toEqual([[]]);
});

test('ghost hover explains rejection and click requests discard',async({page})=>{
  await page.evaluate(()=>(window as any).testRoll.setGhosts([{track:0,tick:384,pitch:96,len:48,reason:'over budget'}]));
  const p=await point(page,390,96);await page.mouse.move(p.x,p.y);
  await expect(page.locator('#test-roll')).toHaveAttribute('title',/over budget/);
  await page.mouse.click(p.x,p.y);
  expect(await calls(page,'onRemoveGhost')).toEqual([[0]]);
});

test('seek, play-from-position, setting selection and lane collapse',async({page})=>{
  await page.mouse.click(210,12);
  const p=await point(page,384,100);await page.mouse.click(p.x,p.y,{button:'right'});
  expect(await calls(page,'onSeek')).toEqual([[288,false],[384,true]]);
  await page.mouse.click(186,12);
  expect(await calls(page,'onSelectSetting')).toEqual([[0,2]]);
  const before=await point(page,384,100);
  await page.mouse.click(20,35);
  const after=await point(page,384,100);
  expect(after.y).toBe(before.y-14);
});

test('zoom, wheel pan, overview and hidden tracks affect hit geometry',async({page})=>{
  const before=await point(page,384,100);
  await page.evaluate(()=>(window as any).testRoll.zoomX(2));
  const after=await point(page,384,100);expect(after.x-66).toBe((before.x-66)*2);
  await page.mouse.move(500,300);await page.keyboard.down('Shift');await page.mouse.wheel(0,200);await page.keyboard.up('Shift');
  await expect.poll(async()=> (await point(page,384,100)).x).toBeLessThan(after.x);
  await page.mouse.click(700,594);
  const panned=await point(page,384,100);expect(panned.x).toBeLessThan(after.x);
  const hidden=await page.evaluate(()=>{
    const r=(window as any).testRoll;r.resetZoom();r.setState(r.state,0,new Set([1]));
    return r.hits.map((h:any)=>h.track);
  });
  expect(hidden).toEqual([0,0]);
});
