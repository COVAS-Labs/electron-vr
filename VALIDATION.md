# Windows and Linux Host Validation

This document defines real-host validation for the
Electron VR overlay implementation. Repository CI validates compilation,
API-layer negotiation, pass-through behavior, installation, and Electron smoke
tests. It does not validate presentation through a real OpenXR/OpenVR runtime,
headset, GPU driver, or game.

Do not mark a row as supported from CI alone. A row is validated only after the
procedure below has been run on the named operating system and its evidence has
been retained.

## Scope

Validate these paths:

| ID | Platform | Backend path | Host graphics binding | Required result |
| --- | --- | --- | --- | --- |
| W-XR-DIRECT | Windows x64 | Direct `XR_EXTX_overlay` | D3D11 | Electron overlay is visible while another primary OpenXR app runs |
| W-LAYER-11 | Windows x64 | Implicit API layer | D3D11 | One Electron quad is appended to the host session |
| W-LAYER-12 | Windows x64 | Implicit API layer | D3D12 | One Electron quad is appended using the host direct queue |
| W-OVR | Windows x64 | OpenVR fallback | D3D11 shared texture | Electron overlay is visible over a SteamVR scene app |
| L-XR-DIRECT | Linux x64 | Direct `XR_EXTX_overlay` | EGL/OpenGL ES | Electron overlay is visible while another primary OpenXR app runs |
| L-LAYER-GLX | Linux x64 | Implicit API layer | desktop OpenGL Xlib/GLX | One DMA-BUF-backed Electron quad is appended to the host session |
| L-LAYER-VK | Linux x64 | Implicit API layer | Vulkan | One DMA-BUF-backed Electron quad is copied into the host session |
| L-OVR | Linux x64 | OpenVR fallback | Vulkan overlay texture | Electron overlay is visible over a SteamVR scene app |

The following are negative pass-through tests, not supported overlay paths:

| ID | Platform | Host case | Required result |
| --- | --- | --- | --- |
| W-PASS | Windows x64 | Unsupported OpenXR graphics binding or no companion | Host continues normally without the overlay |
| L-PASS-NOCOMP | Linux x64 | OpenGL Xlib host with no Electron companion | Host continues normally without the overlay |

## Required Evidence

Create one directory per machine and test date:

```text
artifacts/host-validation/<YYYY-MM-DD>-<platform>-<machine>/
```

Set that directory once in each shell and use it for every command below.

Windows PowerShell:

```powershell
$ValidationDir = Join-Path $PWD "artifacts\host-validation\<YYYY-MM-DD>-windows-<machine>"
New-Item -ItemType Directory -Force $ValidationDir | Out-Null
```

Linux:

```bash
export VALIDATION_DIR="$PWD/artifacts/host-validation/<YYYY-MM-DD>-linux-<machine>"
mkdir -p "$VALIDATION_DIR"
```

Capture the validated repository state before building.

Windows PowerShell:

```powershell
git rev-parse HEAD | Tee-Object "$ValidationDir\git.txt"
git status --short | Tee-Object -Append "$ValidationDir\git.txt"
git submodule status | Tee-Object -Append "$ValidationDir\git.txt"
```

Linux:

```bash
{
  git rev-parse HEAD
  git status --short
  git submodule status
} | tee "$VALIDATION_DIR/git.txt"
```

Retain all of the following:

- `system.txt`: OS version, CPU, RAM, GPU models, driver versions, display
  server, headset, connection method, and whether a hybrid-GPU setup is used.
- `git.txt`: repository commit and working-tree status.
- `runtime-info-*.log`: output from the runtime probe before each backend path.
- `layer-status-*.log`: API-layer status before and after installation.
- `demo-*.log`: complete stdout and stderr from the Electron demo.
- `host-*.log`: complete output from `hello_xr` or the tested application.
- `runtime-*.log`: OpenXR loader/runtime or SteamVR logs where available.
- A headset capture or through-the-lens video showing the host and animated
  Electron panel together.
- A completed result table from the end of this document.

Record failures as failures. Do not omit a failing runtime or GPU from the
evidence bundle.

## Common Repository Setup

Use the exact commit intended for release. Do not validate from an unrecorded
dirty tree.

```bash
git status --short
git rev-parse HEAD
node --version
npm --version
npm ci
npm run build
npm run rebuild:electron
npm test
```

Expected baseline:

- Node.js is version 20 or newer; Node.js 22 is the CI reference.
- Electron is version 37 from the repository lockfile.
- `npm run rebuild:electron` succeeds.
- `npm test` succeeds, allowing tests documented as runtime-gated to skip.
- `git status --short` contains no unexpected generated or source changes.

The build scripts fetch the OpenVR and OpenXR SDK sources when needed. Internet
access to GitHub and Electron header downloads is therefore required for a
fresh checkout.

## Controlled OpenXR Host

Use Khronos `hello_xr` from the OpenXR-SDK-Source repository as the controlled
host. Record the exact OpenXR-SDK-Source commit in `system.txt`. Prefer the SDK
version matching the OpenXR headers fetched into this repository's
`.openxr-sdk` directory.

The expected host commands are:

```text
hello_xr -g D3D11
hello_xr -g D3D12
hello_xr -g OpenGL
hello_xr -g Vulkan
```

Confirm supported options with `hello_xr --help` on the checked-out SDK. If its
CLI differs, record the exact equivalent command. Do not silently substitute a
different graphics API.

`hello_xr` is the binding-isolation test. It is not sufficient game coverage.
After it passes, repeat each applicable path with at least one Unity OpenXR
title and one Unreal OpenXR title, recording engine version and graphics API.

## Windows Host

### Requirements

- Windows 10 22H2 or Windows 11, fully updated, x64.
- A physical D3D11/D3D12-capable GPU and current vendor driver.
- A connected OpenXR-capable headset.
- Visual Studio 2022 Build Tools or Visual Studio 2022 with:
  - Desktop development with C++.
  - MSVC v143 x64 tools.
  - A current Windows 10 or Windows 11 SDK.
  - CMake tools for Windows.
- Git for Windows.
- Node.js 22 x64.
- CMake and Ninja available in `PATH` if building `hello_xr` outside Visual
  Studio.
- At least one active OpenXR runtime. Target validation should include, where
  hardware is available:
  - Meta Quest Link OpenXR.
  - SteamVR OpenXR.
  - Virtual Desktop VDXR.
- SteamVR for the OpenVR test.

Run Electron, the test host, and the runtime at the same integrity level. The
implicit layer is registered per user and is not expected to load into an
elevated host. Do not run the host as Administrator for the normal test.

For hybrid-GPU systems, force Electron, the OpenXR host, and the runtime onto
the same high-performance GPU for the baseline. Run the cross-GPU combination
separately and record it as an expected unsupported case; cross-adapter copying
is not implemented.

### Record The Machine

Run in PowerShell and save the output as `system.txt`:

```powershell
Get-ComputerInfo | Select-Object WindowsProductName, WindowsVersion, OsBuildNumber, OsArchitecture
Get-CimInstance Win32_Processor | Select-Object Name
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory
$PSVersionTable
node --version
npm --version
```

Also record headset model, wired/wireless connection, runtime version, refresh
rate, active OpenXR runtime, and GPU assignment settings manually.

### Build `hello_xr`

Use an x64 Native Tools PowerShell for Visual Studio 2022:

```powershell
git clone https://github.com/KhronosGroup/OpenXR-SDK-Source.git C:\src\OpenXR-SDK-Source
Set-Location C:\src\OpenXR-SDK-Source
git rev-parse HEAD
cmake -S . -B build -A x64 -DBUILD_TESTS=ON -DBUILD_CONFORMANCE_TESTS=OFF
cmake --build build --config Release --target hello_xr
Get-ChildItem -Recurse -Filter hello_xr.exe build
```

Use the path reported by the last command in subsequent steps.

### Windows Baseline

From the electron-vr repository in PowerShell:

```powershell
npm ci
npm run build
npm run rebuild:electron
npm test
npm run openxr-layer -- uninstall
npm run openxr-layer -- status
npm run test:e2e:runtime-info 2>&1 | Tee-Object "$ValidationDir\runtime-info-windows-baseline.log"
```

Expected layer status after uninstall:

```text
installed=false
enabled=false
```

### W-LAYER-11: D3D11 API Layer

This test requires a runtime that does not expose `XR_EXTX_overlay`, because
the implementation intentionally prefers the direct path when that extension
is available.

1. Close all OpenXR applications.
2. Select the target runtime as the active system OpenXR runtime.
3. Install and verify the layer:

```powershell
npm run openxr-layer -- install
npm run openxr-layer -- status 2>&1 | Tee-Object "$ValidationDir\layer-status-windows-d3d11.log"
npm run test:e2e:runtime-info 2>&1 | Tee-Object "$ValidationDir\runtime-info-windows-d3d11.log"
```

Required probe values:

```text
openxrAvailable: true
openxrOverlayExtensionAvailable: false
openxrApiLayerInstalled: true
openxrApiLayerEnabled: true
selectedBackend: 'openxr'
openxrMode: 'api-layer'
```

4. Start the Electron producer in terminal A and retain its full output:

```powershell
npm start 2>&1 | Tee-Object "$ValidationDir\demo-windows-d3d11.log"
```

5. After Electron reports `Overlay initialized with backend: openxr`, start the
   controlled host in terminal B:

```powershell
& "C:\path\to\hello_xr.exe" -g D3D11 2>&1 | Tee-Object "$ValidationDir\host-windows-d3d11.log"
```

6. In the headset, verify all acceptance criteria below for at least five
   continuous minutes.
7. Close `hello_xr`, restart it while Electron remains running, and verify the
   overlay reconnects.
8. Close Electron while `hello_xr` remains running. The host must continue
   rendering without an OpenXR error or crash.
9. Restart Electron while the host remains running. Verify reconnection where
   supported; if it does not reconnect, record the result and logs rather than
   restarting the host.

Required connected state:

```text
openxrCompanionConnected: true
openxrHostProcessId: nonzero
openxrHostApplicationName: hello_xr or the reported host name
openxrHostGraphicsApi: d3d11
openxrProtocolVersion: 2
```

The current demo logs runtime information before the host connects. To capture
the required connected state, query `overlay.getRuntimeInfo()` from the demo
main process after connection or temporarily add periodic logging around
`apps/demo-electron/src/main.ts:39`. Keep any temporary instrumentation out of
the validation commit, and include its diff in the evidence bundle.

### W-LAYER-12: D3D12 API Layer

Repeat W-LAYER-11 without reinstalling the layer, using:

```powershell
& "C:\path\to\hello_xr.exe" -g D3D12 2>&1 | Tee-Object "$ValidationDir\host-windows-d3d12.log"
```

Required differences:

```text
openxrHostGraphicsApi: d3d12
```

The host must use a direct D3D12 command queue. Verify resize, host restart,
Electron restart, and five minutes of continuous animated rendering. Watch for
GPU timeout, fence, command allocator, and swapchain errors.

### W-XR-DIRECT: Direct OpenXR Overlay

This test is applicable only when the runtime reports
`openxrOverlayExtensionAvailable: true` and
`openxrWindowsD3D11BindingAvailable: true`.

```powershell
npm run openxr-layer -- disable
npm run test:e2e:runtime-info 2>&1 | Tee-Object "$ValidationDir\runtime-info-windows-direct.log"
```

Required probe values:

```text
selectedBackend: 'openxr'
openxrMode: 'overlay-session'
openxrOverlayExtensionAvailable: true
openxrWindowsD3D11BindingAvailable: true
```

Start a primary OpenXR application first, then run:

```powershell
npm start 2>&1 | Tee-Object "$ValidationDir\demo-windows-direct.log"
```

Verify that the runtime accepts the additional overlay session and both the
primary app and Electron panel remain visible and responsive. If no available
Windows runtime exposes `XR_EXTX_overlay`, mark this row `NOT AVAILABLE`, not
`PASS`.

### W-OVR: Windows OpenVR

1. Make SteamVR the active environment and start a SteamVR scene application.
2. Disable OpenXR selection for the Electron process only:

```powershell
$env:ELECTRON_VR_DISABLE_OPENXR = "1"
npm run test:e2e:runtime-info 2>&1 | Tee-Object "$ValidationDir\runtime-info-windows-openvr.log"
npm start 2>&1 | Tee-Object "$ValidationDir\demo-windows-openvr.log"
Remove-Item Env:ELECTRON_VR_DISABLE_OPENXR
```

Required probe values:

```text
openvrAvailable: true
openvrRuntimeInstalled: true
selectedBackend: 'openvr'
```

Verify the animated overlay over the active SteamVR scene, head/world
placement, visibility, size, alpha, and curvature.

### Windows Negative Tests

No companion:

```powershell
npm run openxr-layer -- enable
& "C:\path\to\hello_xr.exe" -g D3D11
```

The host must run normally while Electron is stopped.

Disabled layer:

```powershell
npm run openxr-layer -- disable
npm run openxr-layer -- status
& "C:\path\to\hello_xr.exe" -g D3D11
```

The host must run normally and no Electron quad should appear.

Adapter mismatch, if the machine has two GPUs:

- Force Electron and the host onto different adapters.
- The host must continue rendering.
- The overlay may be omitted, and diagnostics must identify failure rather than
  crashing either process.

### Windows Cleanup

```powershell
npm run openxr-layer -- uninstall
npm run openxr-layer -- status
Remove-Item Env:ELECTRON_VR_DISABLE_OPENXR -ErrorAction SilentlyContinue
Remove-Item Env:ELECTRON_VR_DISABLE_OPENXR_API_LAYER -ErrorAction SilentlyContinue
```

Expected final state is `installed=false` and `enabled=false`.

## Linux Host

### Requirements

- Ubuntu 24.04 x64 is the reference distribution. Record deviations.
- An X11 desktop session, or a Wayland session where the controlled host runs
  through XWayland and confirms `XrGraphicsBindingOpenGLXlibKHR`.
- A valid per-user `XDG_RUNTIME_DIR` owned by the current user, normally
  `/run/user/$(id -u)`.
- A physical OpenGL/Vulkan-capable GPU and current Mesa or NVIDIA driver.
- A connected OpenXR-capable headset and active Linux OpenXR runtime.
- Node.js 22 x64, Git, CMake, Ninja, and a C++17 compiler.
- Development packages used by the repository and controlled host.
- Steam and SteamVR for Linux OpenVR validation.

The Linux API layer supports Vulkan and desktop OpenGL with the OpenXR Xlib
binding. Vulkan hosts use direct DMA-BUF import and a fenced swapchain copy;
GLX hosts use the acknowledged software snapshot fallback. Xcb, native
Wayland host bindings, multiplane DMA-BUF, and curvature remain unsupported.

For the baseline, Electron and the host must run as the same non-root desktop
user in the same login session. Never run either process with `sudo`.

### Install Dependencies

On Ubuntu 24.04:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  libx11-dev libxrandr-dev libxxf86vm-dev libgl1-mesa-dev \
  libegl1-mesa-dev libgles2-mesa-dev libdrm-dev libopenxr-dev \
  libvulkan-dev mesa-vulkan-drivers mesa-utils vulkan-tools pciutils \
  gnome-screenshot
```

Install Node.js 22 x64 through the machine's normal Node version manager or
package policy. Confirm `node --version` and `npm --version` before proceeding.

### Remote Validation Session

Use a dedicated non-root Linux workstation with an active graphical login.
SSH is suitable for orchestration, but every VR, Electron, screenshot, and host
process must join the logged-in user's desktop and D-Bus session. Do not encode
user names, home-directory paths, display numbers, or display-manager paths in
scripts or committed evidence.

Discover the session values on the target instead:

```bash
export DISPLAY="$(systemctl --user show-environment | sed -n 's/^DISPLAY=//p')"
export XAUTHORITY="$(systemctl --user show-environment | sed -n 's/^XAUTHORITY=//p')"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
test -n "$DISPLAY"
test -r "$XAUTHORITY"
test -S "$XDG_RUNTIME_DIR/bus"
```

Keep the repository in an arbitrary target directory exported as `REPO_DIR`.
Store evidence outside source-controlled paths unless the artifact is intended
for review:

```bash
export REPO_DIR=/absolute/path/to/electron-vr
export VALIDATION_DIR=/absolute/path/to/validation-output
mkdir -p "$VALIDATION_DIR"
```

For long-running remote commands, transient user services are preferred over
SSH-owned processes. Pass the discovered session variables with `--setenv`,
give each run a descriptive unit name, and inspect it with `systemctl --user`
and `journalctl --user-unit`.

### Record The Machine

Save this output as `system.txt`:

```bash
uname -a
cat /etc/os-release
lscpu
free -h
lspci -nnk
glxinfo -B
vulkaninfo --summary
printf 'XDG_SESSION_TYPE=%s\nDISPLAY=%s\nXDG_RUNTIME_DIR=%s\n' \
  "$XDG_SESSION_TYPE" "$DISPLAY" "$XDG_RUNTIME_DIR"
stat -c '%U %G %a %n' "$XDG_RUNTIME_DIR"
node --version
npm --version
```

Install `mesa-utils`, `vulkan-tools`, and `pciutils` if the corresponding
inventory commands are unavailable. Also record headset, connection method,
runtime version, refresh rate, compositor, and whether the runtime itself uses
X11, XWayland, or native Wayland.

### Build `hello_xr`

```bash
git clone https://github.com/KhronosGroup/OpenXR-SDK-Source.git "$HOME/src/OpenXR-SDK-Source"
cmake -S "$HOME/src/OpenXR-SDK-Source" \
  -B "$HOME/src/OpenXR-SDK-Source/build" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DBUILD_CONFORMANCE_TESTS=OFF
cmake --build "$HOME/src/OpenXR-SDK-Source/build" --target hello_xr
```

Record the SDK commit and locate the executable:

```bash
git -C "$HOME/src/OpenXR-SDK-Source" rev-parse HEAD
find "$HOME/src/OpenXR-SDK-Source/build" -type f -name hello_xr -perm -111
```

Use the reported executable path below as `$HELLO_XR`:

```bash
export HELLO_XR="$HOME/src/OpenXR-SDK-Source/build/src/tests/hello_xr/hello_xr"
"$HELLO_XR" --help
```

Adjust `HELLO_XR` if the build layout differs and record the resulting path.

### Linux Baseline

Verify the desktop session and IPC directory first. Native X11 is the simplest
baseline:

```bash
test "$XDG_SESSION_TYPE" = x11
test -n "$DISPLAY"
test -d "$XDG_RUNTIME_DIR"
test -O "$XDG_RUNTIME_DIR"
```

On Wayland, XWayland is acceptable when the host still uses the OpenXR Xlib
binding. Do not infer the OpenXR binding from `XDG_SESSION_TYPE`; confirm that
the host supplied `XrGraphicsBindingOpenGLXlibKHR`. Electron may remain native
Wayland when that is required for shared-texture DMA-BUF export. Native Wayland
and Xcb OpenXR host bindings remain unsupported by the implicit layer.

Then build and probe:

```bash
npm ci
npm run build
npm run rebuild:electron
npm test
npm run openxr-layer -- uninstall
npm run openxr-layer -- status
npm run test:e2e:runtime-info 2>&1 | tee "$VALIDATION_DIR/runtime-info-linux-baseline.log"
```

Expected layer status after uninstall:

```text
installed=false
enabled=false
```

### L-LAYER-GLX: OpenGL Xlib API Layer

This test requires a runtime that does not expose `XR_EXTX_overlay`, because
the direct overlay path takes priority when the extension is present.

For controlled validation against a runtime such as Monado that also exposes
`XR_EXTX_overlay`, set `ELECTRON_VR_FORCE_OPENXR_API_LAYER=1` for the Electron
producer only. The override takes effect only when the layer is installed and
enabled. Do not set it for the host process or normal product launches.

1. Close all OpenXR applications.
2. Select the target OpenXR runtime with its normal activation method or set
   `XR_RUNTIME_JSON` in both terminals to the same absolute manifest path.
3. Install and inspect the layer:

```bash
npm run openxr-layer -- install
npm run openxr-layer -- status 2>&1 | tee "$VALIDATION_DIR/layer-status-linux-glx.log"
npm run test:e2e:runtime-info 2>&1 | tee "$VALIDATION_DIR/runtime-info-linux-glx.log"
```

Required probe values:

```text
openxrAvailable: true
openxrOverlayExtensionAvailable: false
openxrApiLayerInstalled: true
openxrApiLayerEnabled: true
selectedBackend: 'openxr'
openxrMode: 'api-layer'
```

4. Start Electron in terminal A:

```bash
npm start 2>&1 | tee "$VALIDATION_DIR/demo-linux-glx.log"
```

5. After Electron reports `Overlay initialized with backend: openxr`, start
   the host in terminal B:

```bash
"$HELLO_XR" -g OpenGL 2>&1 | tee "$VALIDATION_DIR/host-linux-glx.log"
```

6. Confirm the host actually supplied `XrGraphicsBindingOpenGLXlibKHR`. Merely
   running under XWayland is not proof of the binding.
7. Validate for at least five continuous minutes.
8. Close and restart the host while Electron remains running.
9. Close Electron while the host remains running; the host must continue.
10. Restart Electron while the host remains running and record reconnect
    behavior.

Required connected state:

```text
openxrCompanionConnected: true
openxrHostProcessId: nonzero
openxrHostApplicationName: hello_xr or the reported host name
openxrHostGraphicsApi: opengl-xlib
openxrProtocolVersion: 2
```

As on Windows, the current demo logs runtime information before connection.
Query `overlay.getRuntimeInfo()` after connection or temporarily add periodic
logging around `apps/demo-electron/src/main.ts:39`, retain that temporary diff
with the evidence, and do not commit it as product behavior.

Run the same test on each available GPU/driver family:

- AMD with Mesa.
- Intel with Mesa.
- NVIDIA proprietary driver.

At minimum, one hardware/driver combination is required before initial support
can be claimed. The others remain explicitly unvalidated until executed.

### L-LAYER-VULKAN: Vulkan API Layer

Install and enable the layer as above, force API-layer selection only for the
controlled Electron producer when the runtime also exposes `XR_EXTX_overlay`,
and start a Vulkan host:

In terminal A:

```bash
ELECTRON_VR_FORCE_OPENXR_API_LAYER=1 npm start \
  2>&1 | tee "$VALIDATION_DIR/demo-linux-vulkan-layer.log"
```

In terminal B:

```bash
"$HELLO_XR" -g Vulkan \
  2>&1 | tee "$VALIDATION_DIR/host-linux-vulkan-layer.log"
```

Required connected state includes `openxrHostGraphicsApi: vulkan` and protocol
version 2. Capture the compositor view at least 70 seconds apart and verify the
overlay clock and host scene both change. Confirm correct orientation, alpha,
color, and placement. Retain service journals and check the kernel log for GPU
faults or resets.

### L-XR-DIRECT: Direct OpenXR Overlay

This test applies only if the runtime reports all of:

```text
openxrOverlayExtensionAvailable: true
openxrLinuxEglBindingAvailable: true
openxrLinuxOpenGlesBindingAvailable: true
```

Disable the implicit layer and probe:

```bash
npm run openxr-layer -- disable
npm run test:e2e:runtime-info 2>&1 | tee "$VALIDATION_DIR/runtime-info-linux-direct.log"
```

Required selection:

```text
selectedBackend: 'openxr'
openxrMode: 'overlay-session'
```

Start a primary OpenXR application, then run:

```bash
npm start 2>&1 | tee "$VALIDATION_DIR/demo-linux-direct.log"
```

Verify coexistence, alpha, animated frame delivery, placement, visibility, and
size. If no available Linux runtime exposes the three required extensions,
mark this row `NOT AVAILABLE`, not `PASS`.

### L-OVR: Linux OpenVR

Start SteamVR and an active SteamVR scene application. Linux OpenVR uses public
`TextureType_Vulkan` automatically, independent of the scene application's
graphics API. Direct DMA-BUF import is preferred; bitmap-to-Vulkan upload is
the automatic correctness fallback. OpenGL upload is diagnostic-only.

Then run without a Vulkan enable flag:

```bash
ELECTRON_VR_DISABLE_OPENXR=1 npm run test:e2e:runtime-info \
  2>&1 | tee "$VALIDATION_DIR/runtime-info-linux-openvr.log"
ELECTRON_VR_DISABLE_OPENXR=1 npm run test:e2e:smoke:openvr:linux \
  2>&1 | tee "$VALIDATION_DIR/demo-linux-openvr.log"
```

Required probe values:

```text
openvrAvailable: true
openvrRuntimeInstalled: true
selectedBackend: 'openvr'
```

A skip caused by missing overlay application support is not a pass. Verify the
animated panel over the active scene, head/world placement, visibility, size,
alpha, and curvature. Retain SteamVR compositor and VR server logs.

For synthetic testing, SteamVR's null driver may be enabled with a generic HMD
profile and `displayDebug=true`. This validates initialization and transport,
but it does not replace physical-headset acceptance. A scene application must
continuously submit frames: while the null HMD reports `Waiting...`, SteamVR
can accept overlay calls without consuming updated texture contents.

Use GNOME Screenshot to capture the compositor view twice across the previous
freeze window:

```bash
gnome-screenshot -f "$VALIDATION_DIR/openvr-vulkan-a.png"
sleep 8
gnome-screenshot -f "$VALIDATION_DIR/openvr-vulkan-b.png"
```

The scene and overlay clock must both change. Also confirm the Electron log
contains `submitted first DMA-BUF through TextureType_Vulkan`. To exercise the
fallback explicitly, set `ELECTRON_VR_OPENVR_VULKAN_SOFTWARE=1`; use
`ELECTRON_VR_DISABLE_OPENVR_VULKAN=1` only for negative testing.

### Linux Negative Tests

No companion with a supported binding:

```bash
npm run openxr-layer -- enable
"$HELLO_XR" -g OpenGL 2>&1 | tee "$VALIDATION_DIR/host-linux-no-companion.log"
```

The host must run normally with no Electron process.

Unsupported native Wayland or Xcb binding:

```bash
npm run openxr-layer -- enable
npm start 2>&1 | tee "$VALIDATION_DIR/demo-linux-unsupported-binding.log"
```

In a second terminal, launch a controlled host known to use a native Wayland
or Xcb OpenXR binding and record its exact command in the evidence.

The unsupported host must continue normally and no injected overlay is expected.
The Electron process may remain waiting for a compatible host.

Disabled layer:

```bash
npm run openxr-layer -- disable
npm run openxr-layer -- status
"$HELLO_XR" -g OpenGL
```

The host must run normally and no Electron quad should appear.

Wayland exploratory test:

- Run the same OpenGL host from a native Wayland session.
- Record the exact OpenXR graphics binding.
- An Xlib binding may still work through XWayland, but native Wayland/Xcb
  bindings are unsupported and must pass through without host failure.

### Linux Cleanup

```bash
npm run openxr-layer -- uninstall
npm run openxr-layer -- status
unset ELECTRON_VR_DISABLE_OPENXR
unset ELECTRON_VR_DISABLE_OPENXR_API_LAYER
unset ELECTRON_VR_FORCE_OPENXR_API_LAYER
unset ELECTRON_VR_DISABLE_OPENVR_VULKAN
unset ELECTRON_VR_OPENVR_VULKAN_SOFTWARE
unset ELECTRON_VR_OPENVR_GL_UPLOAD
unset XR_RUNTIME_JSON
```

Expected final state is `installed=false` and `enabled=false`.

## Visual Acceptance Criteria

Every positive path must satisfy all applicable criteria:

| Check | Pass condition |
| --- | --- |
| Coexistence | Primary app remains active and renders while Electron panel is visible |
| Frame delivery | Clock and signal bars animate continuously without freezing |
| Alpha | Transparent regions reveal host content without an opaque black rectangle |
| Orientation | Panel is upright and text is readable, not mirrored or vertically inverted |
| Head placement | After the demo update, panel remains fixed relative to the headset |
| World placement | With world placement selected, panel remains fixed in runtime space while the head moves |
| Size | Reported size update succeeds and apparent width is approximately the requested metres |
| Visibility | Hiding removes the panel and showing restores it without recreating the host session |
| Resize | Source resize recreates resources without stale content, corruption, or host failure |
| Curvature | Required only for direct OpenXR with cylinder support and OpenVR; API-layer paths remain flat |
| Stability | Five minutes without crash, device loss, compositor failure, persistent flicker, or unbounded memory growth |
| Shutdown | Either process can exit without crashing or hanging the other |
| Restart | Host restart and Electron restart produce the documented reconnect behavior |
| Layer ordering | Electron quad appears above the host projection content where runtime ordering permits |

The stock demo automatically starts with world placement and changes to head
placement immediately after initialization. It is enough for frame, alpha,
orientation, and head-placement checks, but not a deliberate world-lock,
visibility-toggle, resize, or curvature test. Before final release validation,
add a temporary validation control surface or timed sequence that exercises:

```text
world placement for 30 seconds
head placement for 30 seconds
visible false for 5 seconds, then true
size 0.6 m, then 1.2 m
source resize 1280x720 -> 800x800 -> 1280x720
curvature 0 -> 0.5 -> 0 where supported
```

Retain the temporary harness diff with the evidence. Do not infer these visual
results solely from setters returning `true`.

## Performance Capture

For each positive path, record at idle and during active host rendering:

- Host frame rate and compositor dropped/reprojected frame counts.
- Electron process CPU and GPU use.
- Host process CPU and GPU use.
- GPU memory use.
- Overlay update rate and visible stutter.
- Any runtime validation or graphics-driver warnings.

Compare a 60-second host-only baseline with a 60-second host-plus-overlay run.
There is no fixed release threshold yet, so retain raw measurements. Any
repeatable frame-time regression above 1 ms, persistent reprojection increase,
or blocking hitch should be filed before declaring the path validated. Linux
deserves particular attention because the GLX fallback uses CPU snapshots and
`glFinish()`, while Vulkan uses DMA-BUF synchronization and a fenced GPU copy.

## Real Application Matrix

After `hello_xr`, run the applicable layer path against this minimum matrix:

| Platform | Application | Graphics API | Required |
| --- | --- | --- | --- |
| Windows | Unity OpenXR title | D3D11 | Yes |
| Windows | Unreal OpenXR title | D3D12 | Yes |
| Windows | SteamVR scene app | OpenVR | Yes |
| Linux | OpenXR sample or title confirmed to use OpenGL Xlib | OpenGL | Yes |
| Linux | OpenXR title using Vulkan | Vulkan overlay | Yes |
| Linux | SteamVR scene app | OpenVR | Yes when runtime supports overlay apps |

For every title, record title version, engine version if known, launch options,
graphics API, runtime, headset, GPU, and whether anti-cheat is present. Do not
test API-layer injection in anti-cheat-protected software without explicit
permission from the software vendor. Anti-cheat compatibility is not claimed.

## Result Template

Copy this table into the evidence directory as `RESULTS.md`:

| ID | Runtime | Runtime version | Headset | GPU/driver | Host | Result | Evidence path | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| W-XR-DIRECT | | | | | | NOT RUN | | |
| W-LAYER-11 | | | | | | NOT RUN | | |
| W-LAYER-12 | | | | | | NOT RUN | | |
| W-OVR | | | | | | NOT RUN | | |
| W-PASS | | | | | | NOT RUN | | |
| L-XR-DIRECT | | | | | | NOT RUN | | |
| L-LAYER-GLX | | | | | | NOT RUN | | |
| L-LAYER-VK | | | | | | NOT RUN | | |
| L-OVR | | | | | | NOT RUN | | |
| L-PASS-NOCOMP | | | | | | NOT RUN | | |

Allowed result values:

- `PASS`: all acceptance criteria passed and evidence is attached.
- `FAIL`: any acceptance criterion failed.
- `BLOCKED`: setup should support the test, but an environmental issue stopped it.
- `NOT AVAILABLE`: the required runtime extension, graphics binding, or hardware
  is unavailable on the tested machine.
- `NOT RUN`: no attempt has been made.

## Release Gate

Do not describe Windows or Linux real-host support as validated until:

- W-LAYER-11 and W-LAYER-12 pass on at least one physical Windows VR machine.
- L-LAYER-GLX and L-LAYER-VK pass on at least one physical Linux VR machine.
- W-PASS and L-PASS-NOCOMP pass.
- The required Unity/Unreal rows pass.
- OpenVR rows either pass or remain explicitly documented as unvalidated.
- Direct-overlay rows pass where a runtime exposing `XR_EXTX_overlay` is
  actually available; otherwise they remain `NOT AVAILABLE`.
- All evidence is linked from the release or pull request.
