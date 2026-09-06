import { expect, test } from "@playwright/test";

async function openPreview(page: import("@playwright/test").Page) {
  const errors: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") {
      errors.push(message.text());
    }
  });
  page.on("pageerror", (error) => errors.push(error.message));
  await page.goto("/");
  await expect(page.locator("#song-title")).toContainText("08 Hamlet.spc");
  return errors;
}

test("loads the sample song in an explicitly read-only shell", async ({ page }) => {
  const errors = await openPreview(page);

  await expect(page).toHaveTitle(/08 Hamlet\.spc.*Goof Troop Boop/);
  await expect(page.locator("#btn-file")).toBeDisabled();
  await expect(page.locator("#btn-play")).toBeDisabled();
  for (const id of ['btn-add', 'btn-optimize', 'btn-eyedrop']) {
    await expect(page.locator('#' + id)).toBeDisabled();
  }
  await page.keyboard.press('a');
  await expect(page.locator('#btn-add')).not.toHaveClass(/\bon\b/);
  await expect(page.locator("#status")).toContainText("Preview mode (read-only)");
  await expect(page.locator("#channels .chan")).toHaveCount(8);
  await expect(page.locator("#channels .chan").nth(2)).toContainText("320 notes");
  await expect(page.locator("#budget")).toContainText("971 / 971 bytes");

  expect(errors).toEqual([]);
});

test("renders a non-empty piano roll and switches channel presentation state", async ({ page }) => {
  const errors = await openPreview(page);
  const canvas = page.locator("#roll");

  const canvasStats = await canvas.evaluate((element: HTMLCanvasElement) => {
    const context = element.getContext("2d");
    if (!context || !element.width || !element.height) {
      return { opaque: 0, colors: 0 };
    }
    const pixels = context.getImageData(0, 0, element.width, element.height).data;
    const colors = new Set<string>();
    let opaque = 0;
    for (let i = 0; i < pixels.length; i += 4) {
      if (pixels[i + 3]) {
        opaque += 1;
        if (colors.size < 100) {
          colors.add(`${pixels[i]},${pixels[i + 1]},${pixels[i + 2]},${pixels[i + 3]}`);
        }
      }
    }
    return { opaque, colors: colors.size };
  });
  expect(canvasStats.opaque).toBeGreaterThan(10_000);
  expect(canvasStats.colors).toBeGreaterThan(10);

  const trackFour = page.locator("#channels .chan").nth(3);
  await trackFour.click();
  await expect(trackFour).toHaveClass(/active/);
  await expect(page.locator("#inspector")).toContainText("Track 4 settings");

  await trackFour.locator('[data-act="mute"]').click();
  await expect(trackFour.locator('[data-act="mute"]')).toHaveClass(/on/);
  await trackFour.locator('[data-act="solo"]').click();
  await expect(trackFour.locator('[data-act="solo"]')).toHaveClass(/on/);
  await trackFour.locator('[data-act="eye"]').click();
  await expect(trackFour).toHaveClass(/hidden/);
  await expect(trackFour.locator('[data-act="eye"]')).toContainText("🚫");

  expect(errors).toEqual([]);
});

test("toggles the tracker through both the button and keyboard shortcut", async ({ page }) => {
  const errors = await openPreview(page);
  const tracker = page.locator("#tracker");
  const canvas = page.locator("#roll");

  await page.locator("#btn-tracker").click();
  await expect(tracker).toBeVisible();
  await expect(canvas).toHaveCSS("visibility", "hidden");
  expect(await tracker.locator("tbody tr").count()).toBeGreaterThan(300);
  await expect(tracker.locator("thead th")).toHaveCount(9);

  await page.keyboard.press("T");
  await expect(tracker).toBeHidden();
  await expect(canvas).toHaveCSS("visibility", "visible");

  expect(errors).toEqual([]);
});
