import { defineConfig } from "@playwright/test";

// The ASM dialog fixture needs a browser, but no application/dev server.
export default defineConfig({
  testDir: ".",
  testMatch: "asm-import.spec.ts",
  workers: 1,
  outputDir: "../output/playwright/asm-results",
  reporter: [["line"]],
  use: { browserName: "chromium", screenshot: "only-on-failure", trace: "retain-on-failure" },
});
