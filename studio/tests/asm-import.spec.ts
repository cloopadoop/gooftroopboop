import { expect, test } from "@playwright/test";
import { readFileSync } from "node:fs";
import ts from "typescript";

// Exercise the production import/dialog functions without app startup or audio.
const source = readFileSync(new URL("../src/main.ts", import.meta.url), "utf8");
const ownedFunctions = source.slice(source.indexOf("interface AsmReview"), source.indexOf("function midiOptionsDialog")) +
  source.slice(source.indexOf("async function doImport("), source.indexOf("async function doExport("));
const compiled = ts.transpileModule(ownedFunctions, { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText;


test("source repair is reviewed before an explicit import", async ({ page }) => {
  await page.locator("#asm-repair").click();
  await expect(page.locator("#modal")).toContainText("Review source-backed repairs");
  await expect(page.locator("#asm-repair-changes")).toContainText("1 byte changes; 3 exact source anchors");
  await expect(page.locator("#asm-repair-changes")).toContainText("$15: $2D → $8");
  expect(await page.evaluate(() => (window as any).asmCalls.some((r: any) => r.cmd === "importAsm"))).toBe(false);
  await page.locator("#asm-repair-ok").click();
  await page.locator("#asm-source-99").selectOption("8");
  await page.locator("#asm-ok").click();
  await expect(page.locator("#status")).toContainText("Imported ASM");
  expect(await page.evaluate(() => (window as any).asmCalls.find((r: any) => r.cmd === "importAsm").asm)).toBe("; repaired synthetic text");
});

test("cancelling source repair preserves the current song", async ({ page }) => {
  await page.locator("#asm-repair").click();
  await page.locator("#asm-repair-cancel").click();
  await expect(page.locator("#modal-back")).not.toHaveClass(/show/);
  await expect(page.locator("#song-title")).toContainText("Hamlet");
  expect(await page.evaluate(() => (window as any).asmCalls.some((r: any) => r.cmd === "importAsm"))).toBe(false);
});

test("unmatched source stays rejected with the song unchanged", async ({ page }) => {
  await page.evaluate(() => { (window as any).asmFailRepair = true; });
  await page.locator("#asm-repair").click();
  await expect(page.locator("#status")).toContainText("no unambiguous source match");
  await expect(page.locator("#song-title")).toContainText("Hamlet");
  expect(await page.evaluate(() => (window as any).asmCalls.some((r: any) => r.cmd === "importAsm"))).toBe(false);
});

test("a structurally blocked ASM offers a cancellable source-ROM route", async ({ page }) => {
  await page.locator("#asm-cancel").click();
  await page.evaluate(() => { (window as any).asmFailInspect = true; void (window as any).doImport("asm"); });
  await expect(page.locator("#modal")).toContainText("ASM needs repair");
  await expect(page.locator("#asm-repair-message")).toContainText("broken flow");
  await page.locator("#asm-repair-cancel").click();
  await expect(page.locator("#song-title")).toContainText("Hamlet");
  expect(await page.evaluate(() => (window as any).asmCalls.some((r: any) => r.cmd === "importAsm"))).toBe(false);
});


test.beforeEach(async ({ page }) => {
  await page.setContent(`<div id="song-title">08 Hamlet.spc</div><div id="status"></div>
    <div id="modal-back"><div id="modal"></div></div>`);
  await page.addScriptTag({ content: `
    const $ = (id) => document.getElementById(id);
    const status = (message) => { $("status").textContent = message; };
    let state = {source: "08 Hamlet.spc", programs: [{program: 1, name: "Drum"}, {program: 8, name: "Piano"}]};
    let dirty = false, romContext = null, lastPath = null, ghosts = [], ghostTrash = [];
    const roll = {setGhosts() {}};
    const FILTERS = {asm: [], rom: []};
    function afterSongLoad(message) { $("song-title").textContent = state.source; status(message); }
    window.asmCalls = [];
    window.asmFailImport = false;
    window.asmFailInspect = false;
    window.asmFailRepair = false;
    const engine = {
      async openDialog() { return "synthetic.asm"; },
      async request(cmd, args) {
        window.asmCalls.push({cmd, ...args});
        if (cmd === "inspectAsm" && window.asmFailInspect) throw new Error("broken flow");
        if (cmd === "inspectAsmRepair" && window.asmFailRepair) throw new Error("no unambiguous source match");
        return {asm: cmd === "inspectAsmRepair" ? "; repaired synthetic text" : "; reviewed synthetic text",
          repairReport: cmd === "inspectAsmRepair" ? {exactAnchors: 3, changes: [
            {offset: 21, before: 45, after: 8, rule: "source-instrument-alias", sourceOffset: 533}
          ]} : undefined, asmReport: {
          targetBank: "test bank <not markup>", sourcePrograms: [1,99], targetPrograms: [1,8,23],
          silentPrograms: [23],
          noteCommands: 3, warnings: ["Valid programs do not guarantee audible output; audition after import."]
        }};
      },
      async send(cmd, args) {
        window.asmCalls.push({cmd, ...args});
        if (window.asmFailImport) throw new Error("synthetic bank validation failure");
        return {...state, source: "synthetic.asm"};
      }
    };
    ${compiled}
    doImport("asm");
  ` });
  await expect(page.locator("#asm-ok")).toBeVisible();
});

test("review shows resolved programs and cancellation preserves the song", async ({ page }) => {
  await expect(page.locator('#asm-source-1 option[value="23"]')).toContainText("Mute (intentional silence)");
  await expect(page.locator("#modal")).toContainText("source program 1, not 16");
  await expect(page.locator("#asm-bank")).toHaveText("Target bank: test bank <not markup>. 3 note commands.");
  await expect(page.locator("#asm-source-1")).toHaveValue("1");
  await expect(page.locator("#asm-source-99")).toHaveValue("");
  await page.locator("#asm-cancel").click();
  await expect(page.locator("#modal-back")).not.toHaveClass(/show/);
  await expect(page.locator("#song-title")).toContainText("Hamlet");
  expect(await page.evaluate(() => (window as any).asmCalls.filter((r: any) => r.cmd === "importAsm"))).toEqual([]);
});

test("unmapped rows block import; bulk mapping sends reviewed text and every source", async ({ page }) => {
  await page.locator("#asm-ok").click();
  await expect(page.locator("#asm-error")).toContainText("every source program");
  await page.locator("#asm-all").selectOption("8");
  await page.locator("#asm-apply-all").click();
  await expect(page.locator("#asm-source-1")).toHaveValue("8");
  await expect(page.locator("#asm-source-99")).toHaveValue("8");
  await page.locator("#asm-ok").click();
  await expect(page.locator("#status")).toContainText("Imported ASM: synthetic.asm", { timeout: 15_000 });
  await expect(page.locator("#song-title")).toHaveText("synthetic.asm");
  expect(await page.evaluate(() => (window as any).asmCalls.find((r: any) => r.cmd === "importAsm"))).toEqual({
    cmd: "importAsm", asm: "; reviewed synthetic text",
    programMap: [{ from: 1, to: 8 }, { from: 99, to: 8 }],
  });
});

test("individual targets stay independent and engine rejection preserves visible song", async ({ page }) => {
  await page.evaluate(() => { (window as any).asmFailImport = true; });
  await page.locator("#asm-source-99").selectOption("8");
  await page.locator("#asm-ok").click();
  await expect(page.locator("#status")).toContainText("synthetic bank validation failure");
  await expect(page.locator("#song-title")).toContainText("Hamlet");
  expect(await page.evaluate(() => (window as any).asmCalls.find((r: any) => r.cmd === "importAsm").programMap)).toEqual([
    { from: 1, to: 1 }, { from: 99, to: 8 },
  ]);
});
