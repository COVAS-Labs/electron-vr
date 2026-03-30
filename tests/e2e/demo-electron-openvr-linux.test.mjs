import assert from "node:assert/strict";
import { spawn } from "node:child_process";
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

test("boots the demo app with Linux OpenVR forced by disabling OpenXR", { skip: process.platform !== "linux" }, async (t) => {
  await mkdir(artifactDir, { recursive: true });

  const electronArgs = [demoAppDir, "--no-sandbox"];
  const child = spawn(electronBinary, electronArgs, {
    cwd: demoAppDir,
    env: {
      ...process.env,
      CI: "1",
      ELECTRON_VR_DISABLE_OPENXR: "1"
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
      await waitFor(() => combinedOutput.includes("VR runtime probe:"), 20000, "runtime probe logging");
      await waitFor(() => combinedOutput.includes("openvrRuntimeInstalled:"), 20000, "OpenVR install logging");
    } catch {
      throw new Error(buildProcessDebugMessage("forced OpenVR smoke logging", combinedOutput, exitCode, signalCode, spawnError));
    }

    if (!/selectedBackend: 'openvr'/i.test(combinedOutput)) {
      t.skip("Linux OpenVR runtime is not available in this environment.");
      return;
    }

    try {
      await waitFor(
        () => combinedOutput.includes("Overlay initialized with backend: openvr") ||
          /Failed to initialize OpenVR: Invalid Application Type/i.test(combinedOutput) ||
          /Unsupported application type:\s*Overlay/i.test(combinedOutput),
        20000,
        "OpenVR initialization result"
      );
    } catch {
      throw new Error(buildProcessDebugMessage("forced OpenVR initialization result", combinedOutput, exitCode, signalCode, spawnError));
    }

    if (/Failed to initialize OpenVR: Invalid Application Type/i.test(combinedOutput) || /Unsupported application type:\s*Overlay/i.test(combinedOutput)) {
      t.skip("Detected Linux OpenVR runtime does not expose overlay app support in this environment.");
      return;
    }

    try {
      await waitFor(() => combinedOutput.includes("Overlay head placement update: true"), 20000, "OpenVR placement update logging");
      await waitFor(() => combinedOutput.includes("Overlay size update: true"), 20000, "OpenVR size update logging");
      await waitFor(() => combinedOutput.includes("Overlay curvature update: true"), 20000, "OpenVR curvature update logging");
      await waitFor(() => combinedOutput.includes("Overlay visibility update: true"), 20000, "OpenVR visibility update logging");
    } catch {
      throw new Error(buildProcessDebugMessage("forced OpenVR overlay logging", combinedOutput, exitCode, signalCode, spawnError));
    }

    assert.match(combinedOutput, /selectedBackend: 'openvr'/);
    assert.match(combinedOutput, /Overlay initialized with backend: openvr/);
    assert.match(combinedOutput, /Overlay head placement update: true/);
    assert.match(combinedOutput, /Overlay size update: true/);
    assert.match(combinedOutput, /Overlay curvature update: true/);
    assert.match(combinedOutput, /Overlay visibility update: true/);
    assert.doesNotMatch(combinedOutput, /Failed to initialize VR bridge/);
    assert.doesNotMatch(combinedOutput, /UnhandledPromiseRejection|uncaught exception|Error while forwarding frame to VR bridge/i);
  } finally {
    child.kill("SIGTERM");
    await Promise.race([
      new Promise((resolvePromise) => child.once("exit", resolvePromise)),
      sleep(3000)
    ]);

    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
    }

    await writeFile(resolve(artifactDir, "demo-smoke-openvr-linux.log"), combinedOutput, "utf8");
  }
});
