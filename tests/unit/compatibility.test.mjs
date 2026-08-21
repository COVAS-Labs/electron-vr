import assert from "node:assert/strict";
import test from "node:test";

import { analyzeVRCompatibility } from "../../packages/electron-vr/dist/compatibility.js";

function runtime(overrides = {}) {
  return {
    platform: "win32",
    probeMode: "test",
    openxrAvailable: true,
    openxrOverlayExtensionAvailable: false,
    openxrLinuxEglBindingAvailable: false,
    openxrLinuxOpenGlesBindingAvailable: false,
    openxrWindowsD3D11BindingAvailable: true,
    openxrWindowsD3D12BindingAvailable: true,
    openxrMacosMetalBindingAvailable: false,
    openxrRuntimeName: "Test Runtime",
    openxrRuntimeManifestPath: "",
    openxrRuntimeLibraryPath: "",
    openxrLoaderPath: "",
    openxrSessionState: "unknown",
    openxrSessionRunning: false,
    openxrMode: "none",
    openxrApiLayerInstalled: false,
    openxrApiLayerEnabled: false,
    openxrApiLayerManifestPath: "",
    openxrCompanionConnected: false,
    openxrHostDetected: false,
    openxrHostProcessId: 0,
    openxrHostApplicationName: "",
    openxrHostGraphicsApi: "",
    openxrHostAdapterLuid: "",
    openxrProtocolVersion: 0,
    openvrAvailable: false,
    openvrRuntimeInstalled: false,
    openvrRuntimePath: "",
    openvrSceneApplicationState: "",
    openvrSceneProcessId: 0,
    openvrSceneApplicationKey: "",
    openvrSceneApplicationName: "",
    openvrSceneApplicationBinaryPath: "",
    selectedBackend: "mock",
    ...overrides
  };
}

test("compatibility report marks direct OpenXR as ready", () => {
  const report = analyzeVRCompatibility(runtime({
    selectedBackend: "openxr",
    openxrMode: "overlay-session",
    openxrOverlayExtensionAvailable: true
  }), { architecture: "x64" });
  assert.equal(report.readiness, "ready");
  assert.equal(report.launch.verdict, "works-now");
  assert.equal(report.launch.wouldWorkNow, true);
  assert.equal(report.launch.canStartNow, true);
  assert.equal(report.requiresApiLayer, false);
  assert.equal(report.features.curvature, "runtime-dependent");
  assert.deepEqual(report.compatibleHostGraphicsApis, ["D3D11"]);
});

test("compatibility report recommends API-layer installation", () => {
  const report = analyzeVRCompatibility(runtime(), { architecture: "x64" });
  assert.equal(report.readiness, "setup-required");
  assert.equal(report.launch.verdict, "action-required");
  assert.deepEqual(report.launch.requiredActions, ["install-openxr-api-layer", "restart-openxr-apps"]);
  assert.equal(report.recommendedAction, "install-openxr-api-layer");
  assert.equal(report.requiresApiLayer, true);
  assert.equal(report.issues[0].code, "openxr-api-layer-not-installed");
});

test("compatibility report recommends enabling an installed layer", () => {
  const report = analyzeVRCompatibility(runtime({ openxrApiLayerInstalled: true }), {
    architecture: "x64",
    apiLayer: {
      installed: true,
      enabled: false,
      registered: true,
      requiresUpdate: false,
      manifestPath: "layer.json",
      scope: "current-user"
    }
  });
  assert.equal(report.readiness, "setup-required");
  assert.equal(report.launch.verdict, "action-required");
  assert.equal(report.recommendedAction, "enable-openxr-api-layer");
});

test("compatibility report recommends reinstalling an outdated API layer", () => {
  const report = analyzeVRCompatibility(runtime({
    selectedBackend: "openxr",
    openxrMode: "api-layer",
    openxrApiLayerInstalled: true,
    openxrApiLayerEnabled: true
  }), {
    architecture: "x64",
    apiLayer: {
      installed: true,
      enabled: true,
      registered: true,
      requiresUpdate: true,
      manifestPath: "layer.json",
      scope: "current-user"
    }
  });
  assert.equal(report.readiness, "setup-required");
  assert.equal(report.launch.wouldWorkNow, false);
  assert.equal(report.recommendedAction, "reinstall-openxr-api-layer");
  assert.equal(report.issues[0].code, "openxr-api-layer-update-required");
});

test("compatibility report distinguishes waiting and connected API-layer hosts", () => {
  const waiting = analyzeVRCompatibility(runtime({
    selectedBackend: "openxr",
    openxrMode: "api-layer",
    openxrApiLayerInstalled: true,
    openxrApiLayerEnabled: true
  }), { architecture: "x64" });
  assert.equal(waiting.readiness, "waiting-for-host");
  assert.equal(waiting.launch.wouldWorkNow, false);
  assert.equal(waiting.launch.canStartNow, true);
  assert.deepEqual(waiting.launch.requiredActions, ["start-openxr-app"]);
  assert.equal(waiting.recommendedAction, "start-openxr-app");

  const detected = analyzeVRCompatibility(runtime({
    selectedBackend: "openxr",
    openxrMode: "api-layer",
    openxrApiLayerInstalled: true,
    openxrApiLayerEnabled: true,
    openxrHostDetected: true,
    openxrHostProcessId: 1234,
    openxrHostApplicationName: "Elite Dangerous",
    openxrHostGraphicsApi: "d3d11"
  }), { architecture: "x64" });
  assert.equal(detected.readiness, "ready");
  assert.equal(detected.launch.wouldWorkNow, true);
  assert.equal(detected.launch.canStartNow, true);
  assert.equal(detected.requiresOpenXRAppRestart, false);
  assert.match(detected.summary, /Detected Elite Dangerous/);
  assert.equal(detected.diagnostics.openxrCompanionConnected, false);

  const connected = analyzeVRCompatibility(runtime({
    selectedBackend: "openxr",
    openxrMode: "api-layer",
    openxrApiLayerInstalled: true,
    openxrApiLayerEnabled: true,
    openxrCompanionConnected: true,
    openxrHostDetected: true,
    openxrHostApplicationName: "hello_xr",
    openxrHostGraphicsApi: "d3d12"
  }), { architecture: "x64" });
  assert.equal(connected.readiness, "ready");
  assert.equal(connected.launch.verdict, "works-now");
  assert.match(connected.summary, /hello_xr/);
  assert.equal(connected.features.curvature, "unsupported");
});

test("compatibility report treats OpenVR as ready with optional OpenXR setup", () => {
  const report = analyzeVRCompatibility(runtime({
    selectedBackend: "openvr",
    openvrAvailable: true,
    openvrRuntimeInstalled: true
  }), { architecture: "x64" });
  assert.equal(report.readiness, "ready");
  assert.equal(report.launch.verdict, "works-now");
  assert.equal(report.features.curvature, "supported");
  assert.equal(report.issues[0].code, "openxr-api-layer-optional");
});

test("compatibility report labels Linux mock as desktop fallback", () => {
  const report = analyzeVRCompatibility(runtime({
    platform: "linux",
    openxrAvailable: false,
    openxrWindowsD3D11BindingAvailable: false,
    openxrWindowsD3D12BindingAvailable: false
  }), { architecture: "x64" });
  assert.equal(report.readiness, "fallback-only");
  assert.equal(report.launch.verdict, "action-required");
  assert.equal(report.launch.wouldWorkNow, false);
  assert.equal(report.canRenderOverlay, true);
  assert.equal(report.isRealVrBackend, false);
  assert.equal(report.features.headLockedPlacement, "unsupported");
});

test("compatibility report marks unsupported production platforms incompatible", () => {
  const report = analyzeVRCompatibility(runtime({
    platform: "darwin",
    openxrAvailable: false,
    openxrWindowsD3D11BindingAvailable: false,
    openxrWindowsD3D12BindingAvailable: false
  }), { architecture: "arm64" });
  assert.equal(report.launch.verdict, "incompatible");
  assert.equal(report.launch.fundamentalIncompatibility, true);
  assert.deepEqual(report.launch.requiredActions, []);
});

test("compatibility report keeps utility failures out of user-facing copy", () => {
  const report = analyzeVRCompatibility(runtime(), {
    architecture: "x64",
    apiLayerStatusError: "spawn failed: /private/user/path"
  });
  const statusIssue = report.issues.find((candidate) => candidate.code === "openxr-api-layer-status-unavailable");
  assert.ok(statusIssue);
  assert.doesNotMatch(statusIssue.message, /private\/user/);
  assert.match(statusIssue.diagnosticDetails, /private\/user/);
});
