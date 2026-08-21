import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { runNativeOpenXRApiLayerCommand } from "./bridge.js";

export interface OpenXRApiLayerStatus {
  installed: boolean;
  enabled: boolean;
  registered: boolean | null;
  requiresUpdate: boolean;
  manifestPath: string;
  scope: string;
}

type OpenXRApiLayerCommand = "install" | "enable" | "disable" | "status" | "uninstall";

const PREBUILT_PACKAGES = {
  linux: "@covas-labs/electron-vr-prebuilt-linux-x64",
  win32: "@covas-labs/electron-vr-prebuilt-win32-x64"
} as const;

function assertSupportedPlatform(): void {
  if ((process.platform !== "linux" && process.platform !== "win32") || process.arch !== "x64") {
    throw new Error(`OpenXR API-layer management is not supported on ${process.platform}-${process.arch}.`);
  }
}

function resolvePrebuiltCli(): string | null {
  const packageName = PREBUILT_PACKAGES[process.platform as keyof typeof PREBUILT_PACKAGES];
  if (!packageName) return null;

  const requires = [
    createRequire(import.meta.url),
    createRequire(resolve(process.cwd(), "package.json"))
  ];
  for (const require of requires) {
    try {
      const packageEntry = require.resolve(packageName);
      const cli = resolve(dirname(packageEntry), "openxr-layer-cli.js");
      if (existsSync(cli)) return cli;
    } catch {
      // Try the next package resolution context.
    }
  }
  return null;
}

function resolveWindowsLayerAssets(): string {
  const currentDir = dirname(fileURLToPath(import.meta.url));
  const repositoryAssets = resolve(currentDir, "..", "..", "native-addon", "build", "Release");
  if (existsSync(resolve(repositoryAssets, "electron_vr_openxr_layer.dll"))) return repositoryAssets;

  const requires = [createRequire(import.meta.url), createRequire(resolve(process.cwd(), "package.json"))];
  for (const require of requires) {
    try {
      const directory = dirname(require.resolve(PREBUILT_PACKAGES.win32));
      if (existsSync(resolve(directory, "electron_vr_openxr_layer.dll"))) return directory;
    } catch {
      // Try the next package resolution context.
    }
  }
  throw new Error("The Windows OpenXR API-layer assets are unavailable.");
}

function resolveCli(): string {
  assertSupportedPlatform();

  const currentDir = dirname(fileURLToPath(import.meta.url));
  const repositoryCli = resolve(currentDir, "..", "..", "..", "tools", "openxr-layer-cli.mjs");
  if (existsSync(repositoryCli)) return repositoryCli;

  const prebuiltCli = resolvePrebuiltCli();
  if (prebuiltCli) return prebuiltCli;

  throw new Error(
    "The OpenXR API-layer utility is unavailable. Rebuild the repository native addon or install the platform prebuilt package."
  );
}

function runCommand(command: OpenXRApiLayerCommand): Promise<string> {
  const cli = resolveCli();
  return new Promise((resolvePromise, reject) => {
    const child = spawn(process.execPath, [cli, command], {
      env: {
        ...process.env,
        ELECTRON_RUN_AS_NODE: "1"
      },
      stdio: ["ignore", "pipe", "pipe"],
      windowsHide: true
    });
    let stdout = "";
    let stderr = "";
    child.stdout.on("data", (chunk) => {
      stdout += String(chunk);
    });
    child.stderr.on("data", (chunk) => {
      stderr += String(chunk);
    });
    child.once("error", reject);
    child.once("close", (code, signal) => {
      if (code === 0) {
        resolvePromise(stdout);
        return;
      }
      const details = [stdout.trim(), stderr.trim()].filter(Boolean).join("\n");
      reject(new Error(
        `OpenXR API-layer ${command} failed with ${signal ? `signal ${signal}` : `exit code ${code ?? "unknown"}`}${details ? `:\n${details}` : "."}`
      ));
    });
  });
}

export function parseOpenXRApiLayerStatus(output: string): OpenXRApiLayerStatus {
  const values = new Map<string, string>();
  for (const line of output.split(/\r?\n/)) {
    const separator = line.indexOf("=");
    if (separator > 0) values.set(line.slice(0, separator).trim(), line.slice(separator + 1).trim());
  }

  return {
    installed: values.get("installed") === "true",
    enabled: values.get("enabled") === "true",
    registered: values.has("registered") ? values.get("registered") === "true" : null,
    requiresUpdate: values.get("requires_update") === "true",
    manifestPath: values.get("manifest") ?? "",
    scope: values.get("scope") ?? "current-user"
  };
}

export async function getOpenXRApiLayerStatus(): Promise<OpenXRApiLayerStatus> {
  if (process.platform === "win32") {
    return runNativeOpenXRApiLayerCommand("status", resolveWindowsLayerAssets());
  }
  return parseOpenXRApiLayerStatus(await runCommand("status"));
}

async function runLifecycleCommand(command: Exclude<OpenXRApiLayerCommand, "status">): Promise<OpenXRApiLayerStatus> {
  if (process.platform === "win32") {
    return runNativeOpenXRApiLayerCommand(command, resolveWindowsLayerAssets());
  }
  await runCommand(command);
  return getOpenXRApiLayerStatus();
}

export function installOpenXRApiLayer(): Promise<OpenXRApiLayerStatus> {
  return runLifecycleCommand("install");
}

export function enableOpenXRApiLayer(): Promise<OpenXRApiLayerStatus> {
  return runLifecycleCommand("enable");
}

export function disableOpenXRApiLayer(): Promise<OpenXRApiLayerStatus> {
  return runLifecycleCommand("disable");
}

export function uninstallOpenXRApiLayer(): Promise<OpenXRApiLayerStatus> {
  return runLifecycleCommand("uninstall");
}
