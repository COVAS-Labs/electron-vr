import assert from "node:assert/strict";
import { mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import test from "node:test";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

import { _electron as electron } from "playwright";

const require = createRequire(import.meta.url);
const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const artifactDir = resolve(projectRoot, "artifacts");
const electronBinary = require("electron");
const supportsSharedTexturePreview = process.platform === "darwin" && process.arch === "arm64";

async function waitForPreviewWindow(electronApp, timeoutMs = 20000) {
  const startedAt = Date.now();

  while (Date.now() - startedAt < timeoutMs) {
    const pages = electronApp.windows();
    for (const page of pages) {
      const title = await page.title().catch(() => "");
      if (title === "VR Mock Preview") {
        return page;
      }
    }

    await new Promise((resolvePromise) => setTimeout(resolvePromise, 250));
  }

  throw new Error("Timed out waiting for the mock preview window.");
}

test("renders shared texture in the mock preview fallback", { skip: !supportsSharedTexturePreview }, async () => {
  await mkdir(artifactDir, { recursive: true });

  const electronApp = await electron.launch({
    executablePath: electronBinary,
    args: [projectRoot, "--no-sandbox"],
    env: {
      ...process.env,
      CI: "1"
    }
  });

  try {
    const previewPage = await waitForPreviewWindow(electronApp);
    await previewPage.waitForSelector("#backend-value", { timeout: 20000 });
    await previewPage.waitForFunction(() => document.body.dataset.previewReady === "true", undefined, {
      timeout: 20000
    });

    const backend = await previewPage.textContent("#backend-value");
    const frameCount = Number(await previewPage.textContent("#frame-count-value"));
    const snapshotSize = Number(await previewPage.textContent("#snapshot-value"));
    const status = await previewPage.textContent("#status-value");

    await previewPage.screenshot({ path: resolve(artifactDir, "mock-preview.png"), fullPage: true });

    assert.equal(backend?.trim(), "mock");
    assert.ok(frameCount > 0, `expected rendered frame count > 0, received ${frameCount}`);
    assert.ok(snapshotSize > 1000, `expected non-trivial canvas snapshot size, received ${snapshotSize}`);
    assert.match(status ?? "", /ready|streaming/);
  } finally {
    await electronApp.close();
  }
});
