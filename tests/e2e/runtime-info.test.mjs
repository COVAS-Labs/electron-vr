import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
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

test("runtime probe exposes OpenVR runtime installation details", async () => {
  await mkdir(artifactDir, { recursive: true });

  const scriptPath = resolve(artifactDir, "runtime-info-smoke.mjs");
  await writeFile(scriptPath, `
    import { app } from "electron";
    import { createVrBridge } from "../packages/electron-vr/dist/bridge.js";

    app.whenReady().then(() => {
      console.log("Runtime info:", createVrBridge().getRuntimeInfo());
      app.quit();
    });
  `, "utf8");

  const electronArgs = [scriptPath];
  if (process.platform === "linux") {
    electronArgs.push("--no-sandbox");
  }

  const child = spawn(electronBinary, electronArgs, {
    cwd: projectRoot,
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
    await waitFor(() => combinedOutput.includes("Runtime info:"), 20000, "runtime info logging");

    assert.match(combinedOutput, /Runtime info:/);
    assert.match(combinedOutput, /openvrAvailable/i);
    assert.match(combinedOutput, /openvrRuntimeInstalled/i);
    assert.match(combinedOutput, /openvrRuntimePath/i);
  } finally {
    child.kill("SIGTERM");
    await Promise.race([
      new Promise((resolvePromise) => child.once("exit", resolvePromise)),
      sleep(3000)
    ]);

    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
    }

    await writeFile(resolve(artifactDir, `runtime-info-${process.platform}.log`), combinedOutput, "utf8");
  }
});
