import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { access, mkdir, writeFile } from "node:fs/promises";
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

const metaSimulatorManifest = "/Applications/MetaXRSimulator.app/Contents/Resources/MetaXRSimulator/meta_openxr_simulator.json";

async function hasMacMetaSimulator() {
  if (process.platform !== "darwin") return false;
  try {
    await access(metaSimulatorManifest);
    return true;
  } catch {
    return false;
  }
}

test("boots the demo app with OpenXR forced", {
  skip: process.platform !== "linux" && !(await hasMacMetaSimulator())
}, async () => {
  await mkdir(artifactDir, { recursive: true });

  const electronArgs = [demoAppDir];
  if (process.platform === "linux") electronArgs.push("--no-sandbox");
  const child = spawn(electronBinary, electronArgs, {
    cwd: demoAppDir,
    env: {
      ...process.env,
      CI: "1",
      ELECTRON_VR_ENABLE_OPENXR: "1",
      ELECTRON_VR_DISABLE_OPENXR: "0",
      ...(process.platform === "darwin" ? { XR_RUNTIME_JSON: metaSimulatorManifest } : {})
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
      await waitFor(() => combinedOutput.includes("selectedBackend: 'openxr'"), 20000, "OpenXR runtime selection logging");
      await waitFor(() => combinedOutput.includes("Overlay initialized with backend: openxr"), 20000, "OpenXR overlay initialization");
      await waitFor(() => combinedOutput.includes("Overlay head placement update: true"), 20000, "OpenXR placement update logging");
      await waitFor(() => combinedOutput.includes("Overlay size update: true"), 20000, "OpenXR size update logging");
      await waitFor(() => combinedOutput.includes("Overlay visibility update: true"), 20000, "OpenXR visibility update logging");
      if (process.platform === "darwin") {
        await waitFor(() => combinedOutput.includes("OpenXR submitted first macOS Metal"), 30000, "macOS Metal OpenXR frame submission");
      }
    } catch {
      throw new Error(buildProcessDebugMessage("forced OpenXR smoke logging", combinedOutput, exitCode, signalCode, spawnError));
    }

    assert.match(combinedOutput, /selectedBackend: 'openxr'/);
    assert.match(combinedOutput, /Overlay initialized with backend: openxr/);
    assert.match(combinedOutput, /Overlay head placement update: true/);
    assert.match(combinedOutput, /Overlay size update: true/);
    assert.match(combinedOutput, /Overlay visibility update: true/);
    assert.doesNotMatch(combinedOutput, /Failed to initialize VR bridge/);
    assert.doesNotMatch(combinedOutput, /UnhandledPromiseRejection|uncaught exception|Error while forwarding frame to VR bridge/i);
    if (process.platform === "darwin") {
      assert.match(combinedOutput, /openxrOverlayExtensionAvailable:\s*false/i);
      assert.match(combinedOutput, /openxrMacosMetalBindingAvailable:\s*true/i);
      assert.match(combinedOutput, /OpenXR initialized macOS Metal session mode: standard/);
      assert.match(combinedOutput, /OpenXR submitted first macOS Metal (quad|cylinder) layer/);
    }
  } finally {
    child.kill("SIGTERM");
    await Promise.race([
      new Promise((resolvePromise) => child.once("exit", resolvePromise)),
      sleep(3000)
    ]);

    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
    }

    await writeFile(resolve(artifactDir, `demo-smoke-openxr-${process.platform}.log`), combinedOutput, "utf8");
  }
});
