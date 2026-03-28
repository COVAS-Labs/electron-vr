import { app } from "electron";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { VROverlay } from "@covas-labs/electron-vr";

const currentDir = dirname(fileURLToPath(import.meta.url));
const overlayUrl = pathToFileURL(join(currentDir, "ui", "index.html")).toString();
const preloadPath = fileURLToPath(new URL("./preload.js", import.meta.url));

let overlay: VROverlay | null = null;

app.commandLine.appendSwitch("enable-features", "SharedImages");

app.on("ready", async () => {
  overlay = new VROverlay({
    name: "Status_HUD",
    width: 1280,
    height: 720,
    url: overlayUrl,
    sizeMeters: 0.9,
    placement: {
      mode: "head",
      position: { x: 0, y: 0, z: -1.1 },
      rotation: { x: 0, y: 0, z: 0, w: 1 }
    },
    windowOptions: {
      backgroundColor: "#00000000",
      webPreferences: {
        preload: preloadPath
      }
    }
  });

  const runtimeInfo = overlay.getRuntimeInfo();
  console.log("VR runtime probe:", runtimeInfo);
  console.log(`OpenVR runtime installed: ${runtimeInfo.openvrRuntimeInstalled}`);
  if (runtimeInfo.openvrRuntimePath) {
    console.log(`OpenVR runtime path: ${runtimeInfo.openvrRuntimePath}`);
  }

  const success = await overlay.init();
  if (!success) {
    console.error("Overlay init failed.");
    app.quit();
    return;
  }

  console.log(`Overlay initialized with backend: ${overlay.getSelectedBackend()}`);

  const moved = overlay.setPlacement({
    mode: "world",
    position: { x: 0, y: 1.4, z: -2 },
    rotation: { x: 0, y: 0, z: 0, w: 1 }
  });
  console.log(`Overlay world placement update: ${moved}`);

  const resized = overlay.setSizeMeters(1.1);
  console.log(`Overlay size update: ${resized}`);

  const visible = overlay.setVisible(true);
  console.log(`Overlay visibility update: ${visible}`);
});

app.on("window-all-closed", () => {
  overlay?.destroy();
  overlay = null;
  app.quit();
});

app.on("before-quit", () => {
  overlay?.destroy();
  overlay = null;
});
