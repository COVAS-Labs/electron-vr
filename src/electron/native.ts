import type { BrowserWindow, Rectangle } from "electron";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export type BackendKind = "none" | "openxr" | "openvr" | "mock";

export interface RuntimeInfo {
  platform: string;
  probeMode: "stub";
  openxrAvailable: boolean;
  openxrOverlayExtensionAvailable: boolean;
  openvrAvailable: boolean;
  selectedBackend: BackendKind;
}

export interface InitializeVROptions {
  name: string;
  width: number;
  height: number;
}

export interface LinuxTexturePlane {
  fd: number;
  stride: number;
  offset: number;
  size: number;
}

export interface LinuxTextureInfo {
  codedSize?: {
    width: number;
    height: number;
  };
  pixelFormat?: "rgba" | "bgra";
  modifier?: string;
  planes?: LinuxTexturePlane[];
}

export interface SoftwareFrameInfo {
  width: number;
  height: number;
  rgbaPixels: Buffer;
}

export interface SharedTextureInfo extends LinuxTextureInfo {
  sharedTextureHandle?: Buffer;
}

export interface SharedTexturePayload {
  textureInfo?: SharedTextureInfo;
  release(): void;
}

export interface NativeImageLike {
  getSize(): { width: number; height: number };
  toBitmap(): Buffer;
}

export interface AttachWindowOptions {
  frameRate?: number;
}

interface SharedTexturePaintEvent {
  texture?: SharedTexturePayload;
}

interface OffscreenWebContents {
  on(event: "paint", listener: (event: SharedTexturePaintEvent, dirty: Rectangle, result?: unknown) => void): void;
  removeListener(event: "paint", listener: (event: SharedTexturePaintEvent, dirty: Rectangle, result?: unknown) => void): void;
  setFrameRate(frameRate: number): void;
}

interface VrBridgeAddon {
  getRuntimeInfo(): RuntimeInfo;
  initializeVR(options: InitializeVROptions): boolean;
  submitFrameWindows(handle: Buffer | bigint): boolean;
  submitFrameLinux(textureInfo: LinuxTextureInfo | number): boolean;
  submitSoftwareFrame(frameInfo: SoftwareFrameInfo): boolean;
  shutdownVR(): void;
  isInitialized(): boolean;
  getLastError(): string | null;
}

let cachedAddon: VrBridgeAddon | null = null;

function loadVrBridgeAddon(): VrBridgeAddon {
  if (cachedAddon) {
    return cachedAddon;
  }

  const require = createRequire(import.meta.url);
  const currentDir = dirname(fileURLToPath(import.meta.url));
  const addonPath = resolve(currentDir, "..", "..", "build", "Release", "vr_bridge.node");

  cachedAddon = require(addonPath) as VrBridgeAddon;
  return cachedAddon;
}

function isSharedTexturePayload(value: unknown): value is SharedTexturePayload {
  if (!value || typeof value !== "object") {
    return false;
  }

  const candidate = value as { release?: unknown; textureInfo?: unknown };
  return typeof candidate.release === "function" || typeof candidate.textureInfo === "object";
}

function isNativeImageLike(value: unknown): value is NativeImageLike {
  if (!value || typeof value !== "object") {
    return false;
  }

  const candidate = value as { getSize?: unknown; toBitmap?: unknown };
  return typeof candidate.getSize === "function" && typeof candidate.toBitmap === "function";
}

function bgraToRgba(source: Buffer): Buffer {
  const result = Buffer.allocUnsafe(source.length);
  for (let index = 0; index < source.length; index += 4) {
    result[index] = source[index + 2];
    result[index + 1] = source[index + 1];
    result[index + 2] = source[index];
    result[index + 3] = source[index + 3];
  }
  return result;
}

function releaseTexture(texture: SharedTexturePayload | null): void {
  if (texture && typeof texture.release === "function") {
    texture.release();
  }
}

export class VrBridge {
  private readonly addon = loadVrBridgeAddon();
  private attachedWindow: BrowserWindow | null = null;
  private warnedAboutMissingSharedTexture = false;
  private warnedAboutSoftwareFallback = false;

  attachWindow(window: BrowserWindow, options: AttachWindowOptions = {}): void {
    this.detachWindow();

    const offscreenContents = window.webContents as unknown as OffscreenWebContents;
    offscreenContents.setFrameRate(options.frameRate ?? 60);
    offscreenContents.on("paint", this.onPaint);
    this.attachedWindow = window;
  }

  detachWindow(): void {
    if (!this.attachedWindow) {
      return;
    }

    const offscreenContents = this.attachedWindow.webContents as unknown as OffscreenWebContents;
    offscreenContents.removeListener("paint", this.onPaint);
    this.attachedWindow = null;
  }

  getRuntimeInfo(): RuntimeInfo {
    return this.addon.getRuntimeInfo();
  }

  getSelectedBackend(): BackendKind {
    return this.getRuntimeInfo().selectedBackend;
  }

  initialize(options: InitializeVROptions): boolean {
    return this.addon.initializeVR(options);
  }

  shutdown(): void {
    this.detachWindow();
    this.addon.shutdownVR();
  }

  isInitialized(): boolean {
    return this.addon.isInitialized();
  }

  getLastError(): string | null {
    return this.addon.getLastError();
  }

  private readonly onPaint = (event: SharedTexturePaintEvent, _dirty: Rectangle, paintResult?: unknown): void => {
    const nativeImage = isNativeImageLike(paintResult) ? paintResult : null;
    const texture = isSharedTexturePayload(event.texture)
      ? event.texture
      : isSharedTexturePayload(paintResult)
        ? paintResult
        : null;

    if (!texture) {
      if (this.getSelectedBackend() === "mock" && nativeImage) {
        const { width, height } = nativeImage.getSize();
        this.addon.submitSoftwareFrame({
          width,
          height,
          rgbaPixels: bgraToRgba(nativeImage.toBitmap())
        });

        if (!this.warnedAboutSoftwareFallback) {
          this.warnedAboutSoftwareFallback = true;
          console.warn("Shared texture payload was unavailable; using software bitmap upload for mock preview.");
        }
        return;
      }

      if (!this.warnedAboutMissingSharedTexture) {
        this.warnedAboutMissingSharedTexture = true;
        console.warn("Shared texture payload was unavailable on paint; Electron provided a bitmap instead.");
      }
      return;
    }

    try {
      const textureInfo = texture.textureInfo;
      const handle = textureInfo?.sharedTextureHandle;

      if (process.platform === "win32") {
        if (Buffer.isBuffer(handle)) {
          this.addon.submitFrameWindows(handle);
        }
        return;
      }

      if (process.platform === "linux") {
        if (textureInfo?.planes?.length) {
          this.addon.submitFrameLinux(textureInfo);
        } else if (!this.warnedAboutMissingSharedTexture) {
          this.warnedAboutMissingSharedTexture = true;
          console.warn("Shared texture metadata was unavailable on paint; preview submission was skipped.");
        }
      }
    } catch (error) {
      console.error("Error while forwarding frame to VR bridge:", error);
    } finally {
      releaseTexture(texture);
    }
  };
}

export function createVrBridge(): VrBridge {
  return new VrBridge();
}
