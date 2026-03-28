import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const demoAppDir = resolve(projectRoot, "apps", "demo-electron");
const artifactDir = resolve(projectRoot, "artifacts");
const electronBinary = require("electron");

function sleep(milliseconds) {
  return new Promise((resolvePromise) => setTimeout(resolvePromise, milliseconds));
}

async function waitFor(check, timeoutMs, description) {
  const startedAt = Date.now();

  while (Date.now() - startedAt < timeoutMs) {
    const value = await check();
    if (value) {
      return value;
    }

    await sleep(250);
  }

  throw new Error(`Timed out waiting for ${description}.`);
}

function hasMockPreviewWindow() {
  const result = spawnSync("xwininfo", ["-root", "-tree"], {
    cwd: projectRoot,
    encoding: "utf8"
  });

  if (result.status !== 0) {
    return false;
  }

  return result.stdout.includes("VR Mock Preview");
}

test("renders the native mock preview fallback", { skip: process.platform !== "linux" }, async () => {
  await mkdir(artifactDir, { recursive: true });

  const child = spawn(electronBinary, [demoAppDir, "--no-sandbox"], {
    cwd: demoAppDir,
    env: {
      ...process.env,
      CI: "1"
    },
    stdio: ["ignore", "pipe", "pipe"]
  });

  let combinedOutput = "";
  child.stdout.on("data", (chunk) => {
    combinedOutput += String(chunk);
  });
  child.stderr.on("data", (chunk) => {
    combinedOutput += String(chunk);
  });

  try {
    await waitFor(
      () => combinedOutput.includes("Overlay initialized with backend: mock"),
      20000,
      "mock backend initialization"
    );

    await waitFor(
      () => hasMockPreviewWindow(),
      20000,
      "native mock preview window"
    );

    await waitFor(
      () => combinedOutput.includes("using software bitmap upload for mock preview"),
      20000,
      "software mock preview fallback"
    );

    assert.match(combinedOutput, /VR runtime probe:/);
    assert.match(combinedOutput, /OpenVR runtime installed:/);
    assert.match(combinedOutput, /Overlay initialized with backend: mock/);
    assert.match(combinedOutput, /using software bitmap upload for mock preview/);
    assert.match(combinedOutput, /Overlay head placement update: true/);
    assert.match(combinedOutput, /Overlay size update: true/);
    assert.match(combinedOutput, /Overlay visibility update: true/);
  } finally {
    child.kill("SIGTERM");
    await Promise.race([
      new Promise((resolvePromise) => child.once("exit", resolvePromise)),
      sleep(3000)
    ]);

    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
    }

    await writeFile(resolve(artifactDir, "mock-preview.log"), combinedOutput, "utf8");
  }
});
