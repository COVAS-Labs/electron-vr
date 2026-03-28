import { app } from "electron";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const addon = require("@covas-labs/vr-bridge-prebuilt-electron-linux-x64");

app.whenReady().then(() => {
  console.log("Electron prebuilt runtime info:", addon.getRuntimeInfo());
  app.quit();
});
