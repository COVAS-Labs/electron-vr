import { app } from "electron";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { VROverlay } from "@covas-labs/electron-vr";

const currentDir = dirname(fileURLToPath(import.meta.url));
const overlayUrl = pathToFileURL(join(currentDir, "ui", "index.html"))
  .toString();
const preloadPath = fileURLToPath(new URL("./preload.js", import.meta.url));

let overlay: VROverlay | null = null;

app.commandLine.appendSwitch("enable-features", "SharedImages");

app.on("ready", async () => {
  console.log("Using Electron shared texture overlay path.");

  overlay = new VROverlay({
    name: "Status_HUD",
    width: 1280,
    height: 720,
    url: overlayUrl,
    sizeMeters: 1.1,
    curvature: {
      horizontal: 2.5,
      vertical: 4.0,
    },
    placement: {
      mode: "head",
      position: { x: 0, y: 0, z: -0.8 },
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

  const curved = overlay.setCurvature({
    horizontal: 2.0,
    vertical: 3.5,
  });
  console.log(`Overlay curvature update: ${curved}`);

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
