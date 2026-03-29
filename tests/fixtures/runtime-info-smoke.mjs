
    import { app } from "electron";
    import { createVrBridge } from "../../packages/electron-vr/dist/index.js";

    app.whenReady().then(() => {
      console.log("Runtime info:", createVrBridge().getRuntimeInfo());
      app.quit();
    });
    app.on("window-all-closed", () => {
      app.quit();
    });
    process.on("unhandledRejection", (error) => {
      console.error("Unhandled rejection in runtime info smoke:", error);
      app.exit(1);
    });
