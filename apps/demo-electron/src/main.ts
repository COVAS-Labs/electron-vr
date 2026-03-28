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
    windowOptions: {
      webPreferences: {
        preload: preloadPath
      }
    }
  });

  const runtimeInfo = overlay.getRuntimeInfo();
  console.log("VR runtime probe:", runtimeInfo);

  const success = await overlay.init();
  if (!success) {
    console.error("Overlay init failed.");
    app.quit();
    return;
  }

  console.log(`Overlay initialized with backend: ${overlay.getSelectedBackend()}`);
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
