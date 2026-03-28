import { app, BrowserWindow } from "electron";

app.commandLine.appendSwitch("enable-features", "SharedImages");

app.whenReady().then(async () => {
  const win = new BrowserWindow({
    width: 640,
    height: 360,
    show: false,
    webPreferences: {
      offscreen: {
        useSharedTexture: true
      },
      backgroundThrottling: false
    }
  });

  win.webContents.setFrameRate(1);
  win.webContents.on("paint", (event, dirty, image) => {
    const texture = image ?? event.texture;
    const info = texture?.textureInfo;
    console.log("PAINT_DIRTY", dirty);
    console.log("TEXTURE_KEYS", texture ? Object.keys(texture) : null);
    console.log("TEXTURE_INFO", JSON.stringify(info, null, 2));
    if (texture?.textureInfo?.sharedTextureHandle?.nativePixmap?.planes) {
      console.log("PLANES", texture.textureInfo.sharedTextureHandle.nativePixmap.planes);
    }
    texture?.release?.();
    setTimeout(() => app.quit(), 250);
  });

  await win.loadURL(`data:text/html,<!doctype html><html><body style="margin:0;background:transparent;color:white"><div style="width:100vw;height:100vh;background:linear-gradient(45deg, red, blue)"></div></body></html>`);
});
