import { contextBridge, ipcRenderer } from "electron";

contextBridge.exposeInMainWorld("overlayDemo", {
  getDiagnostics: () => ipcRenderer.invoke("overlay-demo:get-diagnostics")
});
