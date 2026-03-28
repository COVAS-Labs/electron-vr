import * as electron from "electron";
import { fileURLToPath } from "node:url";

import type { RuntimeInfo } from "./native.js";

const { BrowserWindow } = electron;

type SharedTextureModule = {
  importSharedTexture(options: {
    textureInfo: unknown;
    allReferencesReleased?: () => void;
  }): {
    release(): void;
  };
  sendSharedTexture(options: {
    frame: Electron.WebFrameMain;
    importedSharedTexture: {
      release(): void;
    };
  }): Promise<void>;
};

const sharedTexture = (electron as typeof electron & { sharedTexture?: SharedTextureModule }).sharedTexture;

export interface OffscreenSharedTexturePayload {
  textureInfo?: unknown;
  release(): void;
}

export class MockPreviewWindow {
  private window: Electron.BrowserWindow | null = null;
  private frameCount = 0;
  private pushInFlight = false;

  async init(runtimeInfo: RuntimeInfo): Promise<void> {
    if (!sharedTexture) {
      throw new Error("Electron sharedTexture API is unavailable in this runtime.");
    }

    if (this.window) {
      return;
    }

    this.window = new BrowserWindow({
      width: 1400,
      height: 900,
      show: true,
      title: "VR Mock Preview",
      autoHideMenuBar: true,
      backgroundColor: "#09111a",
      webPreferences: {
        preload: fileURLToPath(new URL("./mockPreviewPreload.js", import.meta.url)),
        contextIsolation: true,
        nodeIntegration: false,
        backgroundThrottling: false
      }
    });

    await this.window.loadURL(new URL("./ui/mock-preview.html", import.meta.url).toString());
    this.window.webContents.send("mock-preview-meta", {
      backend: runtimeInfo.selectedBackend,
      frameCount: this.frameCount,
      status: "ready"
    });
  }

  pushTexture(texture: OffscreenSharedTexturePayload): void {
    if (!this.window || !sharedTexture || this.pushInFlight) {
      texture.release();
      return;
    }

    if (!texture.textureInfo || !this.window.webContents.mainFrame) {
      texture.release();
      return;
    }

    this.pushInFlight = true;

    const imported = sharedTexture.importSharedTexture({
      textureInfo: texture.textureInfo,
      allReferencesReleased: () => {
        texture.release();
      }
    });

    void sharedTexture.sendSharedTexture({
      frame: this.window.webContents.mainFrame,
      importedSharedTexture: imported
    }).then(() => {
      this.frameCount += 1;
      this.window?.webContents.send("mock-preview-meta", {
        backend: "mock",
        frameCount: this.frameCount,
        status: "streaming"
      });
    }).catch((error: unknown) => {
      this.window?.webContents.send("mock-preview-meta", {
        backend: "mock",
        frameCount: this.frameCount,
        status: `error: ${error instanceof Error ? error.message : String(error)}`
      });
    }).finally(() => {
      imported.release();
      this.pushInFlight = false;
    });
  }

  destroy(): void {
    if (!this.window) {
      return;
    }

    this.window.close();
    this.window = null;
    this.frameCount = 0;
    this.pushInFlight = false;
  }
}
