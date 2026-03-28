import { contextBridge } from "electron";

contextBridge.exposeInMainWorld("overlayDemo", {
  environment: {
    platform: process.platform,
    electron: process.versions.electron,
    chrome: process.versions.chrome
  }
});
