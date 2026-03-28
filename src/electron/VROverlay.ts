import { app, BrowserWindow } from "electron";
import { fileURLToPath } from "node:url";

import { createVrBridge, type BackendKind, type RuntimeInfo } from "./native.js";

interface VROverlayOptions {
  name?: string;
  width?: number;
  height?: number;
  url?: string;
}

export class VROverlay {
  readonly width: number;
  readonly height: number;
  readonly url: string;
  readonly overlayName: string;

  private readonly vrBridge = createVrBridge();
  private window: BrowserWindow | null = null;
  private initialized = false;

  constructor(options: VROverlayOptions = {}) {
    this.width = options.width ?? 1024;
    this.height = options.height ?? 1024;
    this.url = options.url ?? "about:blank";
    this.overlayName = options.name ?? "Electron_VR_Overlay";
  }

  getRuntimeInfo(): RuntimeInfo {
    return this.vrBridge.getRuntimeInfo();
  }

  getSelectedBackend(): BackendKind {
    return this.vrBridge.getSelectedBackend();
  }

  isInitialized(): boolean {
    return this.initialized && this.vrBridge.isInitialized();
  }

  async init(): Promise<boolean> {
    await app.whenReady();

    const initialized = this.vrBridge.initialize({
      name: this.overlayName,
      width: this.width,
      height: this.height
    });

    if (!initialized) {
      console.error("Failed to initialize VR bridge:", this.vrBridge.getLastError());
      return false;
    }

    this.window = new BrowserWindow({
      width: this.width,
      height: this.height,
      show: false,
      frame: false,
      transparent: true,
      webPreferences: {
        preload: fileURLToPath(new URL("./preload.js", import.meta.url)),
        offscreen: {
          useSharedTexture: true
        },
        contextIsolation: true,
        nodeIntegration: false,
        backgroundThrottling: false
      }
    });

    this.vrBridge.attachWindow(this.window, { frameRate: 60 });
    await this.window.loadURL(this.url);

    this.initialized = true;
    return true;
  }

  destroy(): void {
    this.vrBridge.shutdown();

    if (this.window) {
      this.window.close();
      this.window = null;
    }

    this.initialized = false;
  }
}
