
    import { app } from "electron";
    import { createVrBridge, getVRCompatibilityReport } from "../../packages/electron-vr/dist/index.js";

    app.whenReady().then(async () => {
      console.log("Runtime info:", createVrBridge().getRuntimeInfo());
      console.log("Compatibility report:", await getVRCompatibilityReport());
      app.quit();
    });
    app.on("window-all-closed", () => {
      app.quit();
    });
    process.on("unhandledRejection", (error) => {
      console.error("Unhandled rejection in runtime info smoke:", error);
      app.exit(1);
    });
  