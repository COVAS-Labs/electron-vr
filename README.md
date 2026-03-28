# electron-vr-overlay-api

Scaffold for a TypeScript + ESM Electron demo backed by a `node-gyp` native addon.

## What is included

- `src/electron/*`: Electron main-process wrapper and demo UI
- `native/src/*`: N-API addon scaffold with runtime probing and backend stubs
- `scripts/*`: local build helpers for TypeScript output, asset copying, and Electron header rebuilds

## Commands

- `npm install`
- `npm run build`
- `npm run rebuild:electron`
- `npm run test:e2e`
- `npm run start`

`npm install` does not auto-build the native addon. Build it explicitly with `npm run build:addon` for Node or `npm run rebuild:electron` for Electron.

## Prebuilt modules

The GitHub Actions workflow in `.github/workflows/prebuilt-modules.yml` builds prebuilt `vr_bridge.node` artifacts for:

- Linux x64 for Node
- Linux x64 for Electron
- Windows x64 for Node
- Windows x64 for Electron

Each uploaded artifact includes:

- installable `package/`
- `metadata.json`

The metadata marks the build as an `all-backends` prebuilt, meaning the module is compiled with the OpenXR, OpenVR, and mock backend paths included.

## Published prebuilt packages

The workflow in `.github/workflows/publish-prebuilt-packages.yml` publishes installable prebuilt packages to GitHub Packages for each runtime and platform target.

The sample consumer in `samples/registry-consumer/package.json` is designed to install those published packages from `npm.pkg.github.com` and smoke test them under Node and Electron.

To install the published packages on another device, configure `NODE_AUTH_TOKEN` for GitHub Packages access and point the `@covas-labs` scope at `https://npm.pkg.github.com`.

## Current native status

This version is an initialization scaffold:

- probes for likely OpenXR/OpenVR loader libraries at runtime
- selects `openxr`, then `openvr`, then `mock` as a final fallback
- accepts Electron frame handles through the addon boundary
- logs and tracks state safely

## Mock preview fallback

When neither OpenXR nor OpenVR is available, the addon now selects a `mock` backend. The repo also includes a Linux/Xvfb e2e path so CI can validate that the native mock preview window receives frames without a VR runtime.

It does not yet create a real OpenXR/OpenVR overlay or import GPU textures into a compositor.
