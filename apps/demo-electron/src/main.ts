import { app, ipcMain } from "electron";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { VROverlay } from "@covas-labs/electron-vr";

const currentDir = dirname(fileURLToPath(import.meta.url));
const overlayUrl = pathToFileURL(join(currentDir, "ui", "index.html"))
  .toString();
const preloadPath = fileURLToPath(new URL("./preload.js", import.meta.url));

let overlay: VROverlay | null = null;
let diagnosticsTimer: ReturnType<typeof setInterval> | null = null;

app.commandLine.appendSwitch("enable-features", "SharedImages");

function getTransportLabel(info: ReturnType<VROverlay["getRuntimeInfo"]>): string {
  if (info.selectedBackend === "openxr" && info.openxrMode === "api-layer") {
    if (info.openxrHostGraphicsApi === "d3d11" && info.openxrProtocolVersion >= 3) {
      return "Direct Electron texture lease";
    }
    return info.openxrHostGraphicsApi === "d3d12"
      ? "Shared D3D11 ring + D3D12 fence"
      : "OpenXR API-layer shared texture";
  }
  if (info.selectedBackend === "openxr") return "Direct OpenXR shared texture";
  if (info.selectedBackend === "openvr") return process.platform === "linux"
    ? "OpenVR Vulkan / DMA-BUF"
    : "OpenVR shared texture";
  return info.selectedBackend === "mock" ? "Native mock preview" : "No frame transport";
}

ipcMain.handle("overlay-demo:get-diagnostics", () => {
  const info = overlay?.getRuntimeInfo();
  if (!info) return null;
  return {
    platform: info.platform,
    electron: process.versions.electron,
    chrome: process.versions.chrome,
    backend: info.selectedBackend,
    mode: info.openxrMode,
    runtime: info.openxrRuntimeName || (info.selectedBackend === "openvr" ? "OpenVR runtime" : "None"),
    hostApplication: info.openxrHostApplicationName || info.openvrSceneApplicationName || "No host detected",
    graphicsApi: info.openxrHostGraphicsApi || (info.selectedBackend === "openvr" ? "OpenVR" : "None"),
    transport: getTransportLabel(info),
    connected: info.openxrCompanionConnected || info.selectedBackend === "openvr",
    protocol: info.openxrProtocolVersion,
    submitted: info.openxrSubmittedFrameSequence,
    consumed: info.openxrConsumedFrameSequence
  };
});

app.on("ready", async () => {
  console.log("Using Electron shared texture overlay path.");

  overlay = new VROverlay({
    name: "Status_HUD",
    width: 1280,
    height: 720,
    url: overlayUrl,
    sizeMeters: 1.1,
    placement: {
      mode: "world",
      position: { x: 0, y: 1, z: -1.6 },
      rotation: { x: 0, y: 0, z: 0, w: 1 },
    },
    windowOptions: {
      transparent: true,
      backgroundColor: "#00000000",
      webPreferences: {
        preload: preloadPath,
      },
    },
  });

  const runtimeInfo = overlay.getRuntimeInfo();
  console.log("VR runtime probe:", runtimeInfo);

  const success = await overlay.init();
  if (!success) {
    console.error("Overlay init failed.");
    app.quit();
    return;
  }

  console.log(
    `Overlay initialized with backend: ${overlay.getSelectedBackend()}`,
  );

  const moved = overlay.setPlacement({
    mode: "head",
    position: { x: 0, y: 0, z: -0.8 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });
  console.log(`Overlay head placement update: ${moved}`);

  const resized = overlay.setSizeMeters(1.1);
  console.log(`Overlay size update: ${resized}`);

  const visible = overlay.setVisible(true);
  console.log(`Overlay visibility update: ${visible}`);
  diagnosticsTimer = setInterval(() => {
    const info = overlay?.getRuntimeInfo();
    if (info) {
      console.log(
        `VR transport: connected=${info.openxrCompanionConnected} host=${info.openxrHostGraphicsApi || "none"} submitted=${info.openxrSubmittedFrameSequence} consumed=${info.openxrConsumedFrameSequence}`
      );
    }
  }, 2000);
});

app.on("window-all-closed", () => {
  overlay?.destroy();
  overlay = null;
  app.quit();
});

app.on("before-quit", () => {
  if (diagnosticsTimer) clearInterval(diagnosticsTimer);
  diagnosticsTimer = null;
  overlay?.destroy();
  overlay = null;
});
