
    import { app } from "electron";
    import { createVrBridge } from "../../packages/electron-vr/dist/index.js";

    app.whenReady().then(() => {
      console.log("Runtime info:", createVrBridge().getRuntimeInfo());
      app.quit();
    });
  