import { app, BrowserWindow } from "electron";
import type { BrowserWindowConstructorOptions } from "electron";

import { createVrBridge, type BackendKind, type OverlayPlacement, type RuntimeInfo, type Quat, type Vec3 } from "./bridge.js";
import { assertSizeMeters, normalizePlacement } from "./overlayOptions.js";

type OverlayWindowOptions = Omit<BrowserWindowConstructorOptions, "width" | "height" | "webPreferences"> & {
  webPreferences?: BrowserWindowConstructorOptions["webPreferences"];
};

export interface VROverlayOptions {
  name?: string;
  width?: number;
  height?: number;
  url?: string;
  frameRate?: number;
  windowOptions?: OverlayWindowOptions;
  sizeMeters?: number;
  visible?: boolean;
  placement?: {
    mode: "head" | "world";
    position?: Vec3;
    rotation?: Quat;
  };
}

export interface ExistingWindowVROverlayOptions {
  name?: string;
  frameRate?: number;
  sizeMeters?: number;
  visible?: boolean;
  placement?: VROverlayOptions["placement"];
}

interface OffscreenWebContentsCheck {
  isOffscreen?(): boolean;
}

function mergeWindowOptions(
  width: number,
  height: number,
  windowOptions?: OverlayWindowOptions
): BrowserWindowConstructorOptions {
  const requestedOffscreen = windowOptions?.webPreferences?.offscreen;
  const normalizedOffscreen = typeof requestedOffscreen === "object"
    ? { useSharedTexture: true, ...requestedOffscreen }
    : { useSharedTexture: true };

  return {
    width,
    height,
    show: false,
    frame: false,
    transparent: true,
    backgroundColor: "#00000000",
    ...windowOptions,
    webPreferences: {
      offscreen: normalizedOffscreen,
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,
      ...windowOptions?.webPreferences
    }
  };
}

function isOffscreenWindow(window: BrowserWindow): boolean {
  const webContents = window.webContents as typeof window.webContents & OffscreenWebContentsCheck;
  return typeof webContents.isOffscreen === "function" ? webContents.isOffscreen() : false;
}

export class VROverlay {
  readonly width: number;
  readonly height: number;
  readonly url: string;
  readonly overlayName: string;
  readonly frameRate: number;
  readonly windowOptions?: VROverlayOptions["windowOptions"];
  private sizeMeters: number;
  private visible: boolean;
  private placement: OverlayPlacement;

  private readonly vrBridge = createVrBridge();
  private window: BrowserWindow | null = null;
  private ownsWindow = false;
  private initialized = false;

  constructor(options: VROverlayOptions = {}) {
    this.width = options.width ?? 1024;
    this.height = options.height ?? 1024;
    this.url = options.url ?? "about:blank";
    this.overlayName = options.name ?? "Electron_VR_Overlay";
    this.frameRate = options.frameRate ?? 60;
    this.windowOptions = options.windowOptions;
    this.sizeMeters = options.sizeMeters ?? 1.0;
    assertSizeMeters(this.sizeMeters);
    this.visible = options.visible ?? true;
    this.placement = normalizePlacement(options.placement);
  }

  static getRuntimeInfo(): RuntimeInfo {
    return createVrBridge().getRuntimeInfo();
  }

  static isAvailable(runtimeInfo: RuntimeInfo = VROverlay.getRuntimeInfo()): boolean {
    return runtimeInfo.selectedBackend !== "none";
  }

  static hasRealVRRuntime(runtimeInfo: RuntimeInfo = VROverlay.getRuntimeInfo()): boolean {
    return runtimeInfo.selectedBackend === "openxr" || runtimeInfo.selectedBackend === "openvr";
  }

  static async openWindow(
    window: BrowserWindow,
    options: ExistingWindowVROverlayOptions = {}
  ): Promise<VROverlay | null> {
    const [width, height] = window.getContentSize();
    const overlay = new VROverlay({
      name: options.name,
      width,
      height,
      frameRate: options.frameRate,
      sizeMeters: options.sizeMeters,
      visible: options.visible,
      placement: options.placement
    });

    return await overlay.initWithWindow(window) ? overlay : null;
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

    if (this.initialized) {
      return true;
    }

    const window = new BrowserWindow(mergeWindowOptions(this.width, this.height, this.windowOptions));
    this.ownsWindow = true;
    this.window = window;
    window.once("closed", this.onWindowClosed);

    const initialized = this.initializeBridgeForWindow(window, this.width, this.height);
    if (!initialized) {
      window.removeListener("closed", this.onWindowClosed);
      this.window = null;
      this.ownsWindow = false;
      window.close();
      return false;
    }

    await window.loadURL(this.url);
    this.initialized = true;
    return true;
  }

  async initWithWindow(window: BrowserWindow): Promise<boolean> {
    await app.whenReady();

    if (this.initialized) {
      return true;
    }

    if (!isOffscreenWindow(window)) {
      console.error(
        "Existing BrowserWindow is not offscreen-enabled. Create it with offscreen rendering enabled before moving it into VR."
      );
      return false;
    }

    const [width, height] = window.getContentSize();
    this.window = window;
    this.ownsWindow = false;
    window.once("closed", this.onWindowClosed);

    const initialized = this.initializeBridgeForWindow(window, width, height);
    if (!initialized) {
      window.removeListener("closed", this.onWindowClosed);
      this.window = null;
      return false;
    }

    this.initialized = true;
    return true;
  }

  destroy(): void {
    this.vrBridge.shutdown();

    if (this.window) {
      this.window.removeListener("closed", this.onWindowClosed);
      if (this.ownsWindow && !this.window.isDestroyed()) {
        this.window.close();
      }
      this.window = null;
    }

    this.ownsWindow = false;
    this.initialized = false;
  }

  setPlacement(placement: VROverlayOptions["placement"]): boolean {
    const normalizedPlacement = normalizePlacement(placement);
    this.placement = normalizedPlacement;

    if (!this.isInitialized()) {
      return true;
    }

    const success = this.vrBridge.setOverlayPlacement(normalizedPlacement);
    if (!success) {
      console.error("Failed to update VR overlay placement:", this.vrBridge.getLastError());
    }
    return success;
  }

  setVisible(visible: boolean): boolean {
    this.visible = visible;
    if (!this.isInitialized()) {
      return true;
    }

    const success = this.vrBridge.setOverlayVisible(visible);
    if (!success) {
      console.error("Failed to update VR overlay visibility:", this.vrBridge.getLastError());
    }
    return success;
  }

  setSizeMeters(sizeMeters: number): boolean {
    assertSizeMeters(sizeMeters);
    this.sizeMeters = sizeMeters;
    if (!this.isInitialized()) {
      return true;
    }

    const success = this.vrBridge.setOverlaySizeMeters(sizeMeters);
    if (!success) {
      console.error("Failed to update VR overlay size:", this.vrBridge.getLastError());
    }
    return success;
  }

  private initializeBridgeForWindow(window: BrowserWindow, width: number, height: number): boolean {
    const initialized = this.vrBridge.initialize({
      name: this.overlayName,
      width,
      height,
      sizeMeters: this.sizeMeters,
      visible: this.visible,
      placement: this.placement
    });

    if (!initialized) {
      console.error("Failed to initialize VR bridge:", this.vrBridge.getLastError());
      return false;
    }

    this.vrBridge.attachWindow(window, { frameRate: this.frameRate });
    return true;
  }

  private readonly onWindowClosed = (): void => {
    this.vrBridge.shutdown();
    this.window = null;
    this.ownsWindow = false;
    this.initialized = false;
  };
}
