import { createVrBridge, type BackendKind, type OpenXRMode, type RuntimeInfo } from "./bridge.js";
import { getOpenXRApiLayerStatus, type OpenXRApiLayerStatus } from "./openxrApiLayer.js";

export type VRReadiness = "ready" | "setup-required" | "waiting-for-host" | "fallback-only" | "unavailable" | "development-only";
export type VRLaunchVerdict = "works-now" | "action-required" | "incompatible";
export type VRFeatureSupport = "supported" | "unsupported" | "runtime-dependent";
export type VRIssueSeverity = "info" | "warning" | "error";
export type VRSetupAction =
  | "install-openxr-api-layer"
  | "reinstall-openxr-api-layer"
  | "enable-openxr-api-layer"
  | "restart-openxr-apps"
  | "start-openxr-app"
  | "install-xr-runtime"
  | "use-supported-openxr-host"
  | "none";

export interface VRFeatureCompatibility {
  flatOverlay: VRFeatureSupport;
  curvature: VRFeatureSupport;
  headLockedPlacement: VRFeatureSupport;
  worldLockedPlacement: VRFeatureSupport;
  alphaTransparency: VRFeatureSupport;
  multipleOverlays: VRFeatureSupport;
  softwareFrameFallback: VRFeatureSupport;
}

export interface VRCompatibilityIssue {
  code: string;
  severity: VRIssueSeverity;
  title: string;
  message: string;
  action: VRSetupAction;
  diagnosticDetails?: string;
}

export interface VRLaunchAssessment {
  verdict: VRLaunchVerdict;
  wouldWorkNow: boolean;
  canStartNow: boolean;
  fundamentalIncompatibility: boolean;
  message: string;
  requiredActions: VRSetupAction[];
}

export interface VRCompatibilityReport {
  readiness: VRReadiness;
  backend: BackendKind;
  openxrMode: OpenXRMode;
  backendLabel: string;
  summary: string;
  launch: VRLaunchAssessment;
  canRenderOverlay: boolean;
  isRealVrBackend: boolean;
  requiresApiLayer: boolean;
  requiresOpenXRAppRestart: boolean;
  apiLayer: OpenXRApiLayerStatus | null;
  compatibleHostGraphicsApis: string[];
  features: VRFeatureCompatibility;
  issues: VRCompatibilityIssue[];
  recommendedAction: VRSetupAction;
  diagnostics: RuntimeInfo;
}

export interface VRCompatibilityContext {
  architecture?: string;
  apiLayer?: OpenXRApiLayerStatus | null;
  apiLayerStatusError?: string;
}

const realFeatures: VRFeatureCompatibility = {
  flatOverlay: "supported",
  curvature: "unsupported",
  headLockedPlacement: "supported",
  worldLockedPlacement: "supported",
  alphaTransparency: "supported",
  multipleOverlays: "unsupported",
  softwareFrameFallback: "unsupported"
};

function issue(
  code: string,
  severity: VRIssueSeverity,
  title: string,
  message: string,
  action: VRSetupAction = "none",
  diagnosticDetails?: string
): VRCompatibilityIssue {
  return { code, severity, title, message, action, ...(diagnosticDetails ? { diagnosticDetails } : {}) };
}

function directOpenXRFeatures(): VRFeatureCompatibility {
  return { ...realFeatures, curvature: "runtime-dependent" };
}

function openVRFeatures(): VRFeatureCompatibility {
  return { ...realFeatures, curvature: "supported", softwareFrameFallback: "supported" };
}

function unavailableFeatures(platform: string, backend: BackendKind): VRFeatureCompatibility {
  const linuxMock = platform === "linux" && backend === "mock";
  return {
    flatOverlay: linuxMock ? "supported" : "unsupported",
    curvature: "unsupported",
    headLockedPlacement: "unsupported",
    worldLockedPlacement: "unsupported",
    alphaTransparency: linuxMock ? "supported" : "unsupported",
    multipleOverlays: "unsupported",
    softwareFrameFallback: linuxMock ? "supported" : "unsupported"
  };
}

export function analyzeVRCompatibility(
  runtime: RuntimeInfo,
  context: VRCompatibilityContext = {}
): VRCompatibilityReport {
  const { apiLayer = null, apiLayerStatusError } = context;
  const issues: VRCompatibilityIssue[] = [];
  const platformSupportsLayer = (runtime.platform === "win32" || runtime.platform === "linux") &&
    (context.architecture ?? process.arch) === "x64";
  const layerInstalled = apiLayer?.installed ?? runtime.openxrApiLayerInstalled;
  const layerEnabled = apiLayer?.enabled ?? runtime.openxrApiLayerEnabled;
  const layerRequiresUpdate = apiLayer?.requiresUpdate ?? false;
  let readiness: VRReadiness = "unavailable";
  let backendLabel = "No compatible VR backend";
  let summary = "No compatible XR runtime is ready.";
  let features = unavailableFeatures(runtime.platform, runtime.selectedBackend);
  let compatibleHostGraphicsApis: string[] = [];
  let requiresApiLayer = false;
  let recommendedAction: VRSetupAction = "none";
  const supportedHostDetected = runtime.openxrCompanionConnected || runtime.openxrHostDetected ||
    runtime.openvrSceneProcessId !== 0;
  const unsupportedLibOVRHost = runtime.libovrHostDetected && !supportedHostDetected &&
    runtime.openxrMode !== "overlay-session";

  if (unsupportedLibOVRHost) {
    const applicationName = runtime.libovrHostApplicationName || "A VR application";
    readiness = "unavailable";
    backendLabel = "Unsupported native Oculus application";
    summary = `${applicationName} is using native Oculus/LibOVR, which electron-vr cannot overlay.`;
    recommendedAction = "use-supported-openxr-host";
    issues.push(issue(
      "libovr-host-unsupported",
      "error",
      "Native Oculus application detected",
      "Relaunch the application through OpenXR or a supported OpenVR runtime. Native LibOVR applications do not expose a cross-application overlay API.",
      "use-supported-openxr-host"
    ));
  } else if (runtime.selectedBackend === "openxr" && runtime.openxrMode === "overlay-session") {
    readiness = "ready";
    backendLabel = "OpenXR direct overlay";
    summary = "The active OpenXR runtime supports a direct overlay session.";
    features = directOpenXRFeatures();
    compatibleHostGraphicsApis = runtime.platform === "win32" ? ["D3D11"] : ["EGL/OpenGL ES"];
  } else if (runtime.selectedBackend === "openxr" && runtime.openxrMode === "api-layer") {
    requiresApiLayer = true;
    features = { ...realFeatures };
    compatibleHostGraphicsApis = runtime.platform === "win32" ? ["D3D11", "D3D12"] : ["Vulkan", "OpenGL Xlib/GLX"];
    backendLabel = "OpenXR application integration";
    if (layerRequiresUpdate) {
      readiness = "setup-required";
      summary = "OpenXR application integration must be updated before overlays can appear.";
      recommendedAction = "reinstall-openxr-api-layer";
      issues.push(issue(
        "openxr-api-layer-update-required",
        "warning",
        "Update OpenXR integration",
        "Reinstall the per-user integration and restart OpenXR applications.",
        "reinstall-openxr-api-layer"
      ));
    } else if (runtime.openxrCompanionConnected || runtime.openxrHostDetected) {
      readiness = "ready";
      summary = `${runtime.openxrCompanionConnected ? "Connected to" : "Detected"} ${runtime.openxrHostApplicationName || "an OpenXR application"} using ${runtime.openxrHostGraphicsApi || "a supported graphics API"}.`;
    } else if (!layerEnabled) {
      readiness = "setup-required";
      summary = "OpenXR application integration is installed but disabled.";
      recommendedAction = "enable-openxr-api-layer";
      issues.push(issue(
        "openxr-api-layer-disabled",
        "warning",
        "OpenXR integration is disabled",
        "Enable the per-user integration and restart OpenXR applications.",
        "enable-openxr-api-layer"
      ));
    } else {
      readiness = "waiting-for-host";
      summary = "OpenXR integration is ready. Start or restart a compatible OpenXR application.";
      recommendedAction = "start-openxr-app";
      issues.push(issue(
        "openxr-host-not-connected",
        "info",
        "Waiting for a VR application",
        `Start a compatible ${compatibleHostGraphicsApis.join(" or ")} OpenXR application. If it was already running when integration was enabled, restart it.`,
        "start-openxr-app"
      ));
    }
  } else if (runtime.selectedBackend === "openxr" && runtime.openxrMode === "standard-test-session") {
    readiness = "development-only";
    backendLabel = "OpenXR test session";
    summary = "OpenXR is available as a development test session, not as a production coexistence overlay.";
    features = directOpenXRFeatures();
    issues.push(issue(
      "openxr-test-session-only",
      "warning",
      "Development mode only",
      "This session cannot place the overlay above an unrelated OpenXR application."
    ));
  } else if (runtime.selectedBackend === "openvr") {
    readiness = "ready";
    backendLabel = "SteamVR/OpenVR overlay";
    summary = "A registered OpenVR runtime is ready for native overlays.";
    features = openVRFeatures();
    compatibleHostGraphicsApis = runtime.platform === "win32" ? ["SteamVR/OpenVR"] : ["SteamVR/OpenVR with single-plane RGB DMA-BUF"];
    if (runtime.openxrAvailable && platformSupportsLayer && !layerInstalled) {
      issues.push(issue(
        "openxr-api-layer-optional",
        "info",
        "OpenXR application support is optional",
        "SteamVR overlays are ready. Install OpenXR application integration only if overlays are also needed in OpenXR applications without native overlay support.",
        "install-openxr-api-layer"
      ));
    }
  } else {
    const canInstallLayer = platformSupportsLayer && runtime.openxrAvailable &&
      (runtime.platform === "linux" || runtime.openxrWindowsD3D11BindingAvailable || runtime.openxrWindowsD3D12BindingAvailable);
    if (canInstallLayer && !layerInstalled) {
      readiness = "setup-required";
      backendLabel = "Desktop preview";
      summary = "OpenXR is installed, but application integration must be enabled before overlays can appear in compatible OpenXR applications.";
      requiresApiLayer = true;
      recommendedAction = "install-openxr-api-layer";
      compatibleHostGraphicsApis = runtime.platform === "win32" ? ["D3D11", "D3D12"] : ["Vulkan", "OpenGL Xlib/GLX"];
      issues.push(issue(
        "openxr-api-layer-not-installed",
        "warning",
        "Enable OpenXR application integration",
        "Install the per-user integration after asking for permission. Restart OpenXR applications afterward.",
        "install-openxr-api-layer"
      ));
    } else if (canInstallLayer && !layerEnabled) {
      readiness = "setup-required";
      backendLabel = "Desktop preview";
      summary = "OpenXR application integration is installed but disabled.";
      requiresApiLayer = true;
      recommendedAction = "enable-openxr-api-layer";
      compatibleHostGraphicsApis = runtime.platform === "win32" ? ["D3D11", "D3D12"] : ["Vulkan", "OpenGL Xlib/GLX"];
      issues.push(issue(
        "openxr-api-layer-disabled",
        "warning",
        "OpenXR integration is disabled",
        "Enable the per-user integration and restart OpenXR applications.",
        "enable-openxr-api-layer"
      ));
    } else if (runtime.selectedBackend === "mock") {
      readiness = "fallback-only";
      backendLabel = runtime.platform === "linux" ? "Desktop preview" : "No visible VR output";
      summary = runtime.platform === "linux"
        ? "No real XR backend is ready. The overlay can only be previewed on the desktop."
        : "No real XR backend or visible desktop preview is available.";
      issues.push(issue(
        "no-real-xr-backend",
        "error",
        "No compatible VR runtime",
        "Install and configure a supported OpenXR or SteamVR runtime, then check again.",
        "install-xr-runtime"
      ));
      recommendedAction = "install-xr-runtime";
    }
  }

  if (apiLayerStatusError) {
    issues.push(issue(
      "openxr-api-layer-status-unavailable",
      "warning",
      "Could not inspect OpenXR integration",
      "OpenXR integration status could not be checked. Retry the check or repair the application installation.",
      "none",
      apiLayerStatusError
    ));
  }

  const canRenderOverlay = readiness === "ready" || readiness === "waiting-for-host" ||
    (readiness === "fallback-only" && runtime.platform === "linux" && runtime.selectedBackend === "mock");
  let launch: VRLaunchAssessment;
  if (unsupportedLibOVRHost) {
    launch = {
      verdict: "incompatible",
      wouldWorkNow: false,
      canStartNow: false,
      fundamentalIncompatibility: true,
      message: "The running VR application uses native Oculus/LibOVR and cannot host an electron-vr overlay.",
      requiredActions: ["use-supported-openxr-host"]
    };
  } else if (readiness === "ready" && (runtime.selectedBackend === "openxr" || runtime.selectedBackend === "openvr")) {
    launch = {
      verdict: "works-now",
      wouldWorkNow: true,
      canStartNow: true,
      fundamentalIncompatibility: false,
      message: "Launching the browser overlay now is expected to produce real VR output.",
      requiredActions: []
    };
  } else if (readiness === "setup-required") {
    const actions: VRSetupAction[] = recommendedAction === "none" ? [] : [recommendedAction];
    if (requiresApiLayer) actions.push("restart-openxr-apps");
    launch = {
      verdict: "action-required",
      wouldWorkNow: false,
      canStartNow: false,
      fundamentalIncompatibility: false,
      message: "The overlay is compatible, but setup must be completed before launching it.",
      requiredActions: actions
    };
  } else if (readiness === "waiting-for-host") {
    launch = {
      verdict: "action-required",
      wouldWorkNow: false,
      canStartNow: true,
      fundamentalIncompatibility: false,
      message: `The overlay is ready, but real VR output requires a compatible ${compatibleHostGraphicsApis.join(" or ")} OpenXR application to be started or restarted.`,
      requiredActions: ["start-openxr-app"]
    };
  } else if ((readiness === "fallback-only" || readiness === "unavailable") && platformSupportsLayer) {
    launch = {
      verdict: "action-required",
      wouldWorkNow: false,
      canStartNow: false,
      fundamentalIncompatibility: false,
      message: "Launching now would not produce real VR output. Install and configure a supported OpenXR or SteamVR runtime first.",
      requiredActions: ["install-xr-runtime"]
    };
  } else {
    launch = {
      verdict: "incompatible",
      wouldWorkNow: false,
      canStartNow: false,
      fundamentalIncompatibility: true,
      message: "This platform or execution mode cannot provide a production VR overlay with the current implementation.",
      requiredActions: []
    };
  }
  return {
    readiness,
    backend: runtime.selectedBackend,
    openxrMode: runtime.openxrMode,
    backendLabel,
    summary,
    launch,
    canRenderOverlay,
    isRealVrBackend: runtime.selectedBackend === "openxr" || runtime.selectedBackend === "openvr",
    requiresApiLayer,
    requiresOpenXRAppRestart: requiresApiLayer && (!(runtime.openxrHostDetected || runtime.openxrCompanionConnected) || !layerEnabled),
    apiLayer,
    compatibleHostGraphicsApis,
    features,
    issues,
    recommendedAction,
    diagnostics: runtime
  };
}

export async function getVRCompatibilityReport(): Promise<VRCompatibilityReport> {
  const runtime = createVrBridge().getRuntimeInfo();
  if ((runtime.platform !== "win32" && runtime.platform !== "linux") || process.arch !== "x64") {
    return analyzeVRCompatibility(runtime);
  }

  try {
    return analyzeVRCompatibility(runtime, { apiLayer: await getOpenXRApiLayerStatus() });
  } catch (error) {
    return analyzeVRCompatibility(runtime, {
      apiLayerStatusError: error instanceof Error ? error.message : String(error)
    });
  }
}
