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

function buildProcessDebugMessage(description, combinedOutput, exitCode, signalCode, spawnError) {
  const trimmedOutput = combinedOutput.trim();
  return [
    `Timed out waiting for ${description}.`,
    `exitCode=${exitCode === null ? "null" : String(exitCode)}`,
    `signalCode=${signalCode === null ? "null" : String(signalCode)}`,
    spawnError ? `spawnError=${spawnError.message}` : null,
    "Captured output:",
    trimmedOutput.length > 0 ? trimmedOutput : "<no output captured>"
  ].filter(Boolean).join("\n\n");
}

async function runRuntimeInfoProbe(extraEnv = {}) {
  await mkdir(artifactDir, { recursive: true });

  const scriptPath = resolve(projectRoot, "tests", "fixtures", "runtime-info-smoke.mjs");
  await writeFile(scriptPath, `
    import { app } from "electron";
    import { createVrBridge } from "../../packages/electron-vr/dist/index.js";

    app.whenReady().then(() => {
      console.log("Runtime info:", createVrBridge().getRuntimeInfo());
      app.quit();
    });
    app.on("window-all-closed", () => {
      app.quit();
    });
    process.on("unhandledRejection", (error) => {
      console.error("Unhandled rejection in runtime info smoke:", error);
      app.exit(1);
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
      ...extraEnv,
      CI: "1"
    },
    stdio: ["ignore", "pipe", "pipe"]
  });

  let combinedOutput = "";
  let exitCode = null;
  let signalCode = null;
  let spawnError = null;
  child.stdout.on("data", (chunk) => {
    combinedOutput += String(chunk);
  });
  child.stderr.on("data", (chunk) => {
    combinedOutput += String(chunk);
  });
  child.on("exit", (code, signal) => {
    exitCode = code;
    signalCode = signal;
  });
  child.on("error", (error) => {
    spawnError = error;
  });
  child.on("close", (code, signal) => {
    exitCode = code;
    signalCode = signal;
  });

  try {
    try {
      await waitFor(() => combinedOutput.includes("Runtime info:"), 20000, "runtime info logging");
    } catch {
      throw new Error(buildProcessDebugMessage("runtime info logging", combinedOutput, exitCode, signalCode, spawnError));
    }

    return combinedOutput;
  } finally {
    child.kill("SIGTERM");
    await Promise.race([
      new Promise((resolvePromise) => child.once("exit", resolvePromise)),
      sleep(3000)
    ]);

    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
    }
  }
}

test("runtime probe exposes OpenVR runtime installation details", async () => {
  const combinedOutput = await runRuntimeInfoProbe();
  try {
    assert.match(combinedOutput, /Runtime info:/);
    assert.match(combinedOutput, /openxrAvailable/i);
    assert.match(combinedOutput, /openxrOverlayExtensionAvailable/i);
    assert.match(combinedOutput, /openxrWindowsD3D11BindingAvailable/i);
    if (process.platform === "linux" && /openxrAvailable:\s*true/i.test(combinedOutput) && /openxrOverlayExtensionAvailable:\s*true/i.test(combinedOutput) && /openxrLinuxEglBindingAvailable:\s*true/i.test(combinedOutput)) {
      assert.match(combinedOutput, /selectedBackend:\s*'openxr'/i);
    }
    if (process.platform === "win32" && /openxrAvailable:\s*true/i.test(combinedOutput) && /openxrOverlayExtensionAvailable:\s*true/i.test(combinedOutput) && /openxrWindowsD3D11BindingAvailable:\s*true/i.test(combinedOutput)) {
      assert.match(combinedOutput, /selectedBackend:\s*'(openvr|mock)'/i);
    }
    assert.match(combinedOutput, /openvrAvailable/i);
    assert.match(combinedOutput, /openvrRuntimeInstalled/i);
    assert.match(combinedOutput, /openvrRuntimePath/i);
  } finally {
    await writeFile(resolve(artifactDir, `runtime-info-${process.platform}.log`), combinedOutput, "utf8");
  }
});

test("linux runtime probe falls back to OpenVR when OpenXR is disabled", { skip: process.platform !== "linux" }, async () => {
  const combinedOutput = await runRuntimeInfoProbe({
    ELECTRON_VR_DISABLE_OPENXR: "1"
  });

  await writeFile(resolve(artifactDir, "runtime-info-linux-openvr-fallback.log"), combinedOutput, "utf8");

  assert.match(combinedOutput, /Runtime info:/);
  assert.match(combinedOutput, /probeMode: '.*openxr-disabled-by-env.*'/i);

  const hasOpenVRRuntime = /openvrRuntimeInstalled:\s*true/i.test(combinedOutput) && /openvrAvailable:\s*true/i.test(combinedOutput);
  if (hasOpenVRRuntime) {
    assert.match(combinedOutput, /selectedBackend:\s*'openvr'/i);
    return;
  }

  assert.match(combinedOutput, /selectedBackend:\s*'mock'/i);
});
