import { expect, test } from "@playwright/test";
import { readFileSync } from "node:fs";
import ts from "typescript";

const source = readFileSync(new URL("../src/main.ts", import.meta.url), "utf8");
const functions = source.slice(source.indexOf("const ROM_SONG_NAMES"), source.indexOf("function markDirty")) +
  source.slice(source.indexOf("function pickRomSlot("), source.indexOf("async function doImport("));
const compiled = ts.transpileModule(functions, {
  compilerOptions: { target: ts.ScriptTarget.ES2022 },
}).outputText;

test.beforeEach(async ({ page }) => {
  await page.setContent('<div id="modal-back"><div id="modal"></div></div>');
  await page.addScriptTag({ content: `
    const $ = id => document.getElementById(id);
    ${compiled}
    window.pick = need => {
      window.choice = undefined;
      pickRomSlot([{slot: 20, size: 2877}, {slot: 47, size: 128}], need)
        .then(choice => { window.choice = choice; });
    };
  ` });
});

test("custom ROM picker identifies slots without asserting a custom song title", async ({ page }) => {
  await page.evaluate(() => (window as any).pick(null));
  await expect(page.locator(".modal-sub")).toContainText("ROM's pointers");
  await expect(page.locator(".slot-row").first()).toContainText("Slot 0x14 (stock: Sea Robber)");
  await expect(page.locator(".slot-row").last()).toContainText("Slot 0x2F");
  await page.locator(".slot-row").first().click();
  await expect.poll(() => page.evaluate(() => (window as any).choice)).toBe(20);
});

test("oversized selection is explicit and cancellation changes no slot", async ({ page }) => {
  await page.evaluate(() => (window as any).pick(4000));
  await expect(page.locator(".modal-sub")).toContainText("when the ROM can expand");
  await expect(page.locator(".modal-sub")).not.toContainText("grows to 1 MB");
  await expect(page.locator(".slot-row").first()).toContainText("expands ROM");
  await page.locator(".modal-cancel").click();
  await expect.poll(() => page.evaluate(() => (window as any).choice)).toBeNull();
});
