# electron-vr

Electron VR overlay workspace built around one public package: `@covas-labs/electron-vr`.

## Layout

- `packages/native-addon`: native `node-gyp` addon source and build config
- `packages/electron-vr`: public Electron package and runtime loader
- `apps/demo-electron`: demo application that consumes the public package
- `tests/e2e`: repository-owned end-to-end tests
- `tools`: workspace build and publish helpers

## Local workflow

- `npm install`
- `npm run build`
- `npm run rebuild:electron`
- `npm run start`
- `npm run test:e2e`

## Package model

Consumers install only `@covas-labs/electron-vr`.

Platform-specific prebuilt binaries are published as internal implementation packages and loaded automatically by the public package at runtime.

The public package is the only consumer-facing install target. Apps should not import platform-specific package names directly.

## Publishing

`.github/workflows/publish-prebuilt-packages.yml` publishes:

- internal Electron prebuilt packages for Linux and Windows
- the public `@covas-labs/electron-vr` package that depends on those prebuilds

The same workflow also creates a temporary consumer app and verifies that the published package installs and boots under Electron.
