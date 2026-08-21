import type { BrowserWindow, Rectangle } from "electron";
import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export type BackendKind = "none" | "openxr" | "openvr" | "mock";
export type OpenXRMode = "none" | "overlay-session" | "api-layer" | "standard-test-session";

export interface RuntimeInfo {
  platform: string;
  probeMode: string;
  openxrAvailable: boolean;
  openxrOverlayExtensionAvailable: boolean;
  openxrLinuxEglBindingAvailable: boolean;
  openxrLinuxOpenGlesBindingAvailable: boolean;
  openxrWindowsD3D11BindingAvailable: boolean;
  openxrWindowsD3D12BindingAvailable: boolean;
  openxrMacosMetalBindingAvailable: boolean;
  openxrRuntimeName: string;
  openxrRuntimeManifestPath: string;
  openxrRuntimeLibraryPath: string;
  openxrLoaderPath: string;
  openxrSessionState: string;
  openxrSessionRunning: boolean;
  openxrMode: OpenXRMode;
  openxrApiLayerInstalled: boolean;
  openxrApiLayerEnabled: boolean;
  openxrApiLayerManifestPath: string;
  openxrCompanionConnected: boolean;
  openxrHostDetected: boolean;
  openxrHostProcessId: number;
  openxrHostApplicationName: string;
  openxrHostGraphicsApi: string;
  openxrHostAdapterLuid: string;
  openxrProtocolVersion: number;
  openvrAvailable: boolean;
  openvrRuntimeInstalled: boolean;
  openvrRuntimePath: string;
  openvrSceneApplicationState: string;
  openvrSceneProcessId: number;
  openvrSceneApplicationKey: string;
  openvrSceneApplicationName: string;
  openvrSceneApplicationBinaryPath: string;
  selectedBackend: BackendKind;
}

export interface Vec3 {
  x: number;
  y: number;
  z: number;
}

export interface Quat {
  x: number;
  y: number;
  z: number;
  w: number;
}

export type PlacementMode = "head" | "world";

export interface OverlayPlacement {
  mode: PlacementMode;
  position: Vec3;
  rotation: Quat;
}

export interface InitializeVROptions {
  name: string;
  width: number;
  height: number;
  sizeMeters: number;
  curvature?: number;
  visible: boolean;
  placement: OverlayPlacement;
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
  invalidate(): void;
  capturePage(rect?: Rectangle): Promise<NativeImageLike>;
}

interface VrBridgeAddon {
  getRuntimeInfo(): RuntimeInfo;
  initializeVR(options: InitializeVROptions): boolean;
  submitSharedTexture(texture: Buffer | bigint | LinuxTextureInfo | number): boolean;
  submitSoftwareFrame(frameInfo: SoftwareFrameInfo): boolean;
  setOverlayPlacement(placement: OverlayPlacement): boolean;
  setOverlayVisible(visible: boolean): boolean;
  setOverlaySizeMeters(sizeMeters: number): boolean;
  setOverlayCurvature(curvature: number): boolean;
  shutdownVR(): void;
  isInitialized(): boolean;
  getLastError(): string | null;
}

let cachedAddon: VrBridgeAddon | null = null;

const PREBUILT_ADDON_PACKAGES = {
  linux: {
    x64: "@covas-labs/electron-vr-prebuilt-linux-x64"
  },
  win32: {
    x64: "@covas-labs/electron-vr-prebuilt-win32-x64"
  }
} as const;

function resolvePrebuiltPackageName(): string | null {
  const packagesForPlatform = PREBUILT_ADDON_PACKAGES[process.platform as keyof typeof PREBUILT_ADDON_PACKAGES];
  if (!packagesForPlatform) {
    return null;
  }

  return packagesForPlatform[process.arch as keyof typeof packagesForPlatform] ?? null;
}

function prependLibrarySearchPath(directory: string): void {
  if (!existsSync(directory)) {
    return;
  }

  if (process.platform === "win32") {
    const currentPath = process.env.PATH ?? "";
    const pathEntries = currentPath.split(";").filter(Boolean);
    if (!pathEntries.includes(directory)) {
      process.env.PATH = currentPath ? `${directory};${currentPath}` : directory;
    }
    return;
  }

  if (process.platform === "linux") {
    const currentLdLibraryPath = process.env.LD_LIBRARY_PATH ?? "";
    const pathEntries = currentLdLibraryPath.split(":").filter(Boolean);
    if (!pathEntries.includes(directory)) {
      process.env.LD_LIBRARY_PATH = currentLdLibraryPath ? `${directory}:${currentLdLibraryPath}` : directory;
    }
    return;
  }
}

function ensureOpenVRLibraryPath(currentDir: string): void {
  const localAddonDir = resolve(currentDir, "..", "..", "native-addon", "build", "Release");
  prependLibrarySearchPath(localAddonDir);

  const prebuiltPackageName = resolvePrebuiltPackageName();
  if (prebuiltPackageName) {
    const require = createRequire(import.meta.url);
    try {
      const prebuiltEntry = require.resolve(prebuiltPackageName);
      prependLibrarySearchPath(dirname(prebuiltEntry));
    } catch {
      // Ignore and fall back to application resolution or SDK lookup.
    }

    try {
      const applicationRequire = createRequire(resolve(process.cwd(), "package.json"));
      const prebuiltEntry = applicationRequire.resolve(prebuiltPackageName);
      prependLibrarySearchPath(dirname(prebuiltEntry));
    } catch {
      // Ignore and fall back to SDK lookup.
    }
  }

  const sdkDir = process.env.OPENVR_SDK_DIR;
  if (!sdkDir) {
    return;
  }

  if (process.platform === "win32") {
    prependLibrarySearchPath(resolve(sdkDir, "bin", "win64"));
  } else if (process.platform === "linux") {
    prependLibrarySearchPath(resolve(sdkDir, "lib", "linux64"));
  }
}

function loadVrBridgeAddon(): VrBridgeAddon {
  if (cachedAddon) {
    return cachedAddon;
  }

  const require = createRequire(import.meta.url);
  const applicationRequire = createRequire(resolve(process.cwd(), "package.json"));
  const currentDir = dirname(fileURLToPath(import.meta.url));
  const localAddonPath = resolve(currentDir, "..", "..", "native-addon", "build", "Release", "vr_bridge.node");
  ensureOpenVRLibraryPath(currentDir);

  try {
    cachedAddon = require(localAddonPath) as VrBridgeAddon;
    return cachedAddon;
  } catch (localError) {
    const prebuiltPackageName = resolvePrebuiltPackageName();
    if (!prebuiltPackageName) {
      throw new Error(
        `No supported prebuilt Electron addon is available for ${process.platform}-${process.arch}. Local addon load failed: ${String(localError)}`
      );
    }

    try {
      cachedAddon = require(prebuiltPackageName) as VrBridgeAddon;
      return cachedAddon;
    } catch (packageRequireError) {
      try {
        cachedAddon = applicationRequire(prebuiltPackageName) as VrBridgeAddon;
        return cachedAddon;
      } catch (applicationRequireError) {
      throw new Error(
          `Failed to load the Electron VR addon from ${localAddonPath} or ${prebuiltPackageName}. Local error: ${String(localError)}. Package-local prebuilt error: ${String(packageRequireError)}. Application-level prebuilt error: ${String(applicationRequireError)}`
      );
      }
    }
  }
}

function sanitizeRuntimeInfo(runtimeInfo: RuntimeInfo): RuntimeInfo {
  return {
    ...runtimeInfo,
    openxrSessionState: runtimeInfo.openxrSessionState ?? "unknown",
    openxrSessionRunning: runtimeInfo.openxrSessionRunning ?? false,
    openxrWindowsD3D12BindingAvailable: runtimeInfo.openxrWindowsD3D12BindingAvailable ?? false,
    openxrMode: runtimeInfo.openxrMode ?? "none",
    openxrApiLayerInstalled: runtimeInfo.openxrApiLayerInstalled ?? false,
    openxrApiLayerEnabled: runtimeInfo.openxrApiLayerEnabled ?? false,
    openxrApiLayerManifestPath: runtimeInfo.openxrApiLayerManifestPath ?? "",
    openxrCompanionConnected: runtimeInfo.openxrCompanionConnected ?? false,
    openxrHostDetected: runtimeInfo.openxrHostDetected ?? false,
    openxrHostProcessId: runtimeInfo.openxrHostProcessId ?? 0,
    openxrHostApplicationName: runtimeInfo.openxrHostApplicationName ?? "",
    openxrHostGraphicsApi: runtimeInfo.openxrHostGraphicsApi ?? "",
    openxrHostAdapterLuid: runtimeInfo.openxrHostAdapterLuid ?? "",
    openxrProtocolVersion: runtimeInfo.openxrProtocolVersion ?? 0,
    openvrSceneApplicationState: runtimeInfo.openvrSceneApplicationState ?? "",
    openvrSceneProcessId: runtimeInfo.openvrSceneProcessId ?? 0,
    openvrSceneApplicationKey: runtimeInfo.openvrSceneApplicationKey ?? "",
    openvrSceneApplicationName: runtimeInfo.openvrSceneApplicationName ?? "",
    openvrSceneApplicationBinaryPath: runtimeInfo.openvrSceneApplicationBinaryPath ?? ""
  };
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

function bgraToVerticallyFlippedRgba(source: Buffer, width: number, height: number): Buffer {
  const result = Buffer.allocUnsafe(source.length);
  const rowBytes = width * 4;
  for (let sourceRow = 0; sourceRow < height; sourceRow += 1) {
    const sourceOffset = sourceRow * rowBytes;
    const targetOffset = (height - sourceRow - 1) * rowBytes;
    for (let columnOffset = 0; columnOffset < rowBytes; columnOffset += 4) {
      result[targetOffset + columnOffset] = source[sourceOffset + columnOffset + 2];
      result[targetOffset + columnOffset + 1] = source[sourceOffset + columnOffset + 1];
      result[targetOffset + columnOffset + 2] = source[sourceOffset + columnOffset];
      result[targetOffset + columnOffset + 3] = source[sourceOffset + columnOffset + 3];
    }
  }
  return result;
}

function isTruthyEnvironmentVariable(name: string): boolean {
  const value = process.env[name]?.toLowerCase();
  return value !== undefined && value !== "" && value !== "0" && value !== "false" && value !== "off";
}

function releaseTexture(texture: SharedTexturePayload | null): void {
  if (texture && typeof texture.release === "function") {
    texture.release();
  }
}

function cloneLinuxTextureInfo(textureInfo: SharedTextureInfo): SharedTextureInfo {
  return {
    codedSize: textureInfo.codedSize
      ? {
          width: textureInfo.codedSize.width,
          height: textureInfo.codedSize.height
        }
      : undefined,
    pixelFormat: textureInfo.pixelFormat,
    modifier: textureInfo.modifier,
    planes: textureInfo.planes?.map((plane) => ({
      fd: plane.fd,
      stride: plane.stride,
      offset: plane.offset,
      size: plane.size
    }))
  };
}

export class VrBridge {
  private readonly addon = loadVrBridgeAddon();
  private attachedWindow: BrowserWindow | null = null;
  private warnedAboutMissingSharedTexture = false;
  private warnedAboutSoftwareFallback = false;
  private warnedAboutWindowsSoftwareFallback = false;
  private warnedAboutEmptyLinuxBitmap = false;
  private loggedFirstPaint = false;
  private loggedFirstWindowsHandle = false;
  private loggedFirstWindowsSubmit = false;
  private loggedFirstWindowsSoftwareSubmit = false;
  private loggedFirstLinuxOpenGLSubmit = false;
  private loggedFirstLinuxOpenXRSoftwareSubmit = false;
  private loggedFirstLinuxOpenVRVulkanSoftwareSubmit = false;
  private linuxOpenVRVisibleTexture: SharedTexturePayload | null = null;
  private windowsReadbackInFlight = false;
  private windowsReadbackPending = false;
  private linuxReadbackInFlight = false;
  private linuxReadbackPending = false;
  private invalidationTimer: ReturnType<typeof setInterval> | null = null;

  attachWindow(window: BrowserWindow, options: AttachWindowOptions = {}): void {
    this.detachWindow();

    const offscreenContents = window.webContents as unknown as OffscreenWebContents;
    const frameRate = options.frameRate ?? 60;
    offscreenContents.setFrameRate(frameRate);
    offscreenContents.on("paint", this.onPaint);
    this.attachedWindow = window;
    if (this.getSelectedBackend() === "openxr" ||
        (process.platform === "linux" &&
          (isTruthyEnvironmentVariable("ELECTRON_VR_OPENVR_GL_UPLOAD") ||
            (this.getSelectedBackend() === "openvr" &&
              !isTruthyEnvironmentVariable("ELECTRON_VR_DISABLE_OPENVR_VULKAN"))))) {
      this.invalidationTimer = setInterval(() => {
        if (this.attachedWindow && !this.attachedWindow.isDestroyed()) {
          (this.attachedWindow.webContents as unknown as OffscreenWebContents).invalidate();
        }
      }, 1000 / frameRate);
    }
  }

  detachWindow(): void {
    if (this.invalidationTimer) {
      clearInterval(this.invalidationTimer);
      this.invalidationTimer = null;
    }
    if (!this.attachedWindow) {
      return;
    }

    this.linuxReadbackPending = false;

    const offscreenContents = this.attachedWindow.webContents as unknown as OffscreenWebContents;
    offscreenContents.removeListener("paint", this.onPaint);
    this.attachedWindow = null;
  }

  getRuntimeInfo(): RuntimeInfo {
    return sanitizeRuntimeInfo(this.addon.getRuntimeInfo());
  }

  getSelectedBackend(): BackendKind {
    return this.getRuntimeInfo().selectedBackend;
  }

  initialize(options: InitializeVROptions): boolean {
    try {
      return this.addon.initializeVR(options);
    } finally {
      releaseTexture(this.linuxOpenVRVisibleTexture);
      this.linuxOpenVRVisibleTexture = null;
    }
  }

  setOverlayPlacement(placement: OverlayPlacement): boolean {
    return this.addon.setOverlayPlacement(placement);
  }

  setOverlayVisible(visible: boolean): boolean {
    return this.addon.setOverlayVisible(visible);
  }

  setOverlaySizeMeters(sizeMeters: number): boolean {
    return this.addon.setOverlaySizeMeters(sizeMeters);
  }

  setOverlayCurvature(curvature: number): boolean {
    return this.addon.setOverlayCurvature(curvature);
  }

  shutdown(): void {
    this.detachWindow();
    try {
      this.addon.shutdownVR();
    } finally {
      releaseTexture(this.linuxOpenVRVisibleTexture);
      this.linuxOpenVRVisibleTexture = null;
    }
  }

  isInitialized(): boolean {
    return this.addon.isInitialized();
  }

  getLastError(): string | null {
    return this.addon.getLastError();
  }

  private submitWindowsSoftwareFrame(nativeImage: NativeImageLike): boolean {
    const { width, height } = nativeImage.getSize();
    const bitmap = nativeImage.toBitmap();
    if (width === 0 || height === 0 || bitmap.length === 0) {
      console.warn(`Windows software frame readback was empty (width=${width}, height=${height}, bytes=${bitmap.length}).`);
      return false;
    }

    const submitted = this.addon.submitSoftwareFrame({
      width,
      height,
      rgbaPixels: bgraToRgba(bitmap)
    });

    if (!submitted) {
      console.error("Failed to submit Windows software frame to VR bridge:", this.addon.getLastError());
      return false;
    }

    if (!this.loggedFirstWindowsSoftwareSubmit) {
      this.loggedFirstWindowsSoftwareSubmit = true;
      console.log("VR overlay submitted first Windows software frame to the native bridge.");
    }

    return true;
  }

  private submitLinuxBitmapFrame(
    nativeImage: NativeImageLike | null,
    verticallyFlip: boolean,
    allowCaptureFallback = true
  ): boolean {
    const openvrVulkanSoftware = !verticallyFlip && this.getSelectedBackend() === "openvr";
    const transportName = verticallyFlip
      ? "OpenVR OpenGL upload"
      : openvrVulkanSoftware
        ? "OpenVR Vulkan software upload"
        : "OpenXR API-layer software transport";
    if (!nativeImage) {
      if (!this.warnedAboutMissingSharedTexture) {
        this.warnedAboutMissingSharedTexture = true;
        console.warn(`Linux ${transportName} requires Electron bitmap paint data.`);
      }
      if (allowCaptureFallback) {
        this.scheduleLinuxCaptureFallback();
      }
      return false;
    }

    const { width, height } = nativeImage.getSize();
    if (width === 0 || height === 0) {
      if (!this.warnedAboutEmptyLinuxBitmap) {
        this.warnedAboutEmptyLinuxBitmap = true;
        console.warn(`Linux ${transportName} paint bitmap was empty (width=${width}, height=${height}); using capturePage readback.`);
      }
      if (allowCaptureFallback) {
        this.scheduleLinuxCaptureFallback();
      }
      return false;
    }

    const bitmap = nativeImage.toBitmap();
    if (bitmap.length === 0) {
      if (!this.warnedAboutEmptyLinuxBitmap) {
        this.warnedAboutEmptyLinuxBitmap = true;
        console.warn(`Linux ${transportName} paint bitmap contained no pixels; using capturePage readback.`);
      }
      if (allowCaptureFallback) {
        this.scheduleLinuxCaptureFallback();
      }
      return false;
    }

    const submitted = this.addon.submitSoftwareFrame({
      width,
      height,
      rgbaPixels: verticallyFlip ? bgraToVerticallyFlippedRgba(bitmap, width, height) : bgraToRgba(bitmap)
    });
    if (!submitted) {
      console.error("Failed to submit Linux frame to VR bridge:", this.addon.getLastError());
      return false;
    }

    if (verticallyFlip && !this.loggedFirstLinuxOpenGLSubmit) {
      this.loggedFirstLinuxOpenGLSubmit = true;
      console.log("VR overlay submitted first Linux frame through OpenGL upload.");
    } else if (openvrVulkanSoftware && !this.loggedFirstLinuxOpenVRVulkanSoftwareSubmit) {
      this.loggedFirstLinuxOpenVRVulkanSoftwareSubmit = true;
      console.log("VR overlay submitted first Linux frame through OpenVR Vulkan software upload.");
    } else if (!openvrVulkanSoftware && !verticallyFlip && !this.loggedFirstLinuxOpenXRSoftwareSubmit) {
      this.loggedFirstLinuxOpenXRSoftwareSubmit = true;
      console.log("VR overlay submitted first Linux frame through OpenXR API-layer software transport.");
    }
    return true;
  }

  private submitLinuxOpenGLFrame(nativeImage: NativeImageLike | null, allowCaptureFallback = true): boolean {
    return this.submitLinuxBitmapFrame(nativeImage, true, allowCaptureFallback);
  }

  private submitLinuxOpenXRSoftwareFrame(nativeImage: NativeImageLike | null, allowCaptureFallback = true): boolean {
    return this.submitLinuxBitmapFrame(nativeImage, false, allowCaptureFallback);
  }

  private scheduleLinuxCaptureFallback(): void {
    if (!this.attachedWindow) {
      return;
    }
    if (this.linuxReadbackInFlight) {
      this.linuxReadbackPending = true;
      return;
    }

    this.linuxReadbackInFlight = true;
    const window = this.attachedWindow;
    const offscreenContents = window.webContents as unknown as OffscreenWebContents;
    const [width, height] = window.getContentSize();
    void offscreenContents.capturePage({ x: 0, y: 0, width, height })
      .then((image) => {
        if (this.attachedWindow !== window || !this.isInitialized()) {
          return;
        }
        const runtimeInfo = this.getRuntimeInfo();
        if (runtimeInfo.selectedBackend === "openxr" && runtimeInfo.openxrMode === "api-layer") {
          if (runtimeInfo.openxrCompanionConnected && runtimeInfo.openxrHostGraphicsApi === "opengl-xlib") {
            this.submitLinuxOpenXRSoftwareFrame(image, false);
          }
          return;
        }
        if (runtimeInfo.selectedBackend === "openvr" &&
            !isTruthyEnvironmentVariable("ELECTRON_VR_DISABLE_OPENVR_VULKAN") &&
            !isTruthyEnvironmentVariable("ELECTRON_VR_OPENVR_GL_UPLOAD")) {
          this.submitLinuxBitmapFrame(image, false, false);
        } else {
          this.submitLinuxOpenGLFrame(image, false);
        }
      })
      .catch((error) => {
        console.error("Failed to capture Linux OpenGL upload frame:", error);
      })
      .finally(() => {
        this.linuxReadbackInFlight = false;
        if (this.linuxReadbackPending) {
          this.linuxReadbackPending = false;
          this.scheduleLinuxCaptureFallback();
        }
      });
  }

  private useWindowsSoftwareFallback(nativeImage: NativeImageLike | null, warning: string): void {
    if (!this.warnedAboutWindowsSoftwareFallback) {
      this.warnedAboutWindowsSoftwareFallback = true;
      console.warn(warning);
    }

    if (nativeImage && this.submitWindowsSoftwareFrame(nativeImage)) {
      return;
    }

    this.scheduleWindowsCaptureFallback();
  }

  private scheduleWindowsCaptureFallback(): void {
    if (!this.attachedWindow) {
      return;
    }

    if (this.windowsReadbackInFlight) {
      this.windowsReadbackPending = true;
      return;
    }

    this.windowsReadbackInFlight = true;
    const window = this.attachedWindow;
    const offscreenContents = window.webContents as unknown as OffscreenWebContents;
    const [width, height] = window.getContentSize();

    void offscreenContents.capturePage({ x: 0, y: 0, width, height })
      .then((image) => {
        this.submitWindowsSoftwareFrame(image);
      })
      .catch((error) => {
        console.error("Failed to capture Windows software fallback frame:", error);
      })
      .finally(() => {
        this.windowsReadbackInFlight = false;
        if (this.windowsReadbackPending) {
          this.windowsReadbackPending = false;
          this.scheduleWindowsCaptureFallback();
        }
      });
  }

  private readonly onPaint = (event: SharedTexturePaintEvent, _dirty: Rectangle, paintResult?: unknown): void => {
    const nativeImage = isNativeImageLike(paintResult) ? paintResult : null;
    const texture = isSharedTexturePayload(event.texture)
      ? event.texture
      : isSharedTexturePayload(paintResult)
        ? paintResult
        : null;

    if (!this.loggedFirstPaint) {
      this.loggedFirstPaint = true;
      console.log(
        `VR overlay received first paint event (platform=${process.platform}, backend=${this.getSelectedBackend()}, hasTexture=${texture ? "yes" : "no"}, hasBitmap=${nativeImage ? "yes" : "no"}).`
      );
    }

    const runtimeInfo = this.getRuntimeInfo();
    if (process.platform === "linux" &&
        runtimeInfo.selectedBackend === "openvr" &&
        isTruthyEnvironmentVariable("ELECTRON_VR_OPENVR_VULKAN_SOFTWARE")) {
      try {
        this.submitLinuxBitmapFrame(nativeImage, false);
      } finally {
        releaseTexture(texture);
      }
      return;
    }
    if (process.platform === "linux" &&
        runtimeInfo.selectedBackend === "openxr" &&
        runtimeInfo.openxrMode === "api-layer" &&
        (!runtimeInfo.openxrCompanionConnected || runtimeInfo.openxrHostGraphicsApi === "opengl-xlib")) {
      try {
        if (runtimeInfo.openxrCompanionConnected) {
          this.submitLinuxOpenXRSoftwareFrame(nativeImage);
        }
      } finally {
        releaseTexture(texture);
      }
      return;
    }

    if (!texture) {
      if (process.platform === "linux" &&
          this.getSelectedBackend() === "openvr") {
        if (isTruthyEnvironmentVariable("ELECTRON_VR_OPENVR_GL_UPLOAD")) {
          this.submitLinuxOpenGLFrame(nativeImage);
        } else if (!isTruthyEnvironmentVariable("ELECTRON_VR_DISABLE_OPENVR_VULKAN")) {
          this.submitLinuxBitmapFrame(nativeImage, false);
        }
        return;
      }

      if (process.platform === "win32" && this.getSelectedBackend() === "openvr") {
        this.useWindowsSoftwareFallback(
          nativeImage,
          "Windows shared texture payload was unavailable; falling back to software RGBA upload."
        );
        return;
      }

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

    let releaseCurrentTexture = true;
    try {
      const textureInfo = texture.textureInfo;
      const handle = textureInfo?.sharedTextureHandle;

      if (process.platform === "win32") {
        if (Buffer.isBuffer(handle)) {
          if (!this.loggedFirstWindowsHandle) {
            this.loggedFirstWindowsHandle = true;
            console.log(`VR overlay received Windows shared texture handle (${handle.byteLength} bytes).`);
          }

          const submitted = this.addon.submitSharedTexture(handle);
          if (!submitted) {
            console.error("Failed to submit Windows frame to VR bridge:", this.addon.getLastError());
            if (this.getSelectedBackend() === "openvr") {
              this.useWindowsSoftwareFallback(
                nativeImage,
                "Windows shared texture submission failed; falling back to software RGBA upload."
              );
            }
          } else if (!this.loggedFirstWindowsSubmit) {
            this.loggedFirstWindowsSubmit = true;
            console.log("VR overlay submitted first Windows frame to the native bridge.");
          }
        } else if (this.getSelectedBackend() === "openvr") {
          this.useWindowsSoftwareFallback(
            nativeImage,
            "Windows shared texture handle was unavailable; falling back to software RGBA upload."
          );
        } else if (!this.warnedAboutMissingSharedTexture) {
          this.warnedAboutMissingSharedTexture = true;
          console.warn("Shared texture handle was unavailable on Windows; VR submission was skipped.");
        }
        return;
      }

      if (process.platform === "linux") {
        if (this.getSelectedBackend() === "openvr" &&
            isTruthyEnvironmentVariable("ELECTRON_VR_OPENVR_GL_UPLOAD")) {
          this.submitLinuxOpenGLFrame(nativeImage);
        } else if (textureInfo?.planes?.length) {
          const textureInfoSnapshot = cloneLinuxTextureInfo(textureInfo);

          const submitted = this.addon.submitSharedTexture(textureInfoSnapshot);
          if (!submitted) {
            console.error("Failed to submit Linux frame to VR bridge:", this.addon.getLastError());
            if (this.getSelectedBackend() === "openvr" &&
                !isTruthyEnvironmentVariable("ELECTRON_VR_DISABLE_OPENVR_VULKAN")) {
              this.submitLinuxBitmapFrame(nativeImage, false);
            }
          } else if (this.getSelectedBackend() === "openvr" &&
              isTruthyEnvironmentVariable("ELECTRON_VR_DISABLE_OPENVR_VULKAN")) {
            const previousTexture = this.linuxOpenVRVisibleTexture;
            this.linuxOpenVRVisibleTexture = texture;
            releaseCurrentTexture = false;
            releaseTexture(previousTexture);
          }
        } else if (!this.warnedAboutMissingSharedTexture) {
          this.warnedAboutMissingSharedTexture = true;
          console.warn("Shared texture metadata was unavailable on paint; preview submission was skipped.");
        }
        return;
      }

      if (process.platform === "darwin") {
        if (Buffer.isBuffer(handle) && textureInfo?.codedSize) {
          const submitted = this.addon.submitSharedTexture(textureInfo);
          if (!submitted) {
            console.error("Failed to submit macOS frame to VR bridge:", this.addon.getLastError());
          }
        } else if (!this.warnedAboutMissingSharedTexture) {
          this.warnedAboutMissingSharedTexture = true;
          console.warn("IOSurface metadata was unavailable on paint; OpenXR submission was skipped.");
        }
      }
    } catch (error) {
      console.error("Error while forwarding frame to VR bridge:", error);
    } finally {
      if (releaseCurrentTexture) {
        releaseTexture(texture);
      }
    }
  };
}

export function createVrBridge(): VrBridge {
  return new VrBridge();
}
