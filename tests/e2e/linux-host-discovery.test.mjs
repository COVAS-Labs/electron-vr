import assert from 'node:assert/strict';
import test from 'node:test';
import { spawn, spawnSync } from 'node:child_process';
import { copyFile, mkdir, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import electron from 'electron';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const release = resolve(root, 'packages/native-addon/build/Release');

test('Linux discovers a game-side runtime before overlay initialization, without a usable local runtime',
  { skip: process.platform !== 'linux', timeout: 20000 }, async () => {
    const directory = await mkdtemp(resolve(tmpdir(), 'electron-vr-discovery-'));
    const manifests = resolve(directory, 'openxr/1/api_layers/implicit.d');
    await mkdir(manifests, { recursive: true });
    await copyFile(resolve(release, 'libelectron_vr_openxr_layer.so'), resolve(manifests, 'libelectron_vr_openxr_layer.so'));
    await copyFile(resolve(release, 'electron_vr_openxr_layer_linux.json'), resolve(manifests, 'electron_vr_openxr_layer.json'));
    const host = spawn(resolve(release, 'electron_vr_openxr_layer_test'), ['--host-seconds', '30'],
      { cwd: release, stdio: ['ignore', 'pipe', 'pipe'] });
    try {
      await new Promise((resolveReady, reject) => {
        let output = '';
        const timer = setTimeout(() => reject(new Error(`Host discovery fixture did not start: ${output}`)), 5000);
        host.stdout.on('data', data => {
          output += data;
          if (output.includes('HOST_READY')) { clearTimeout(timer); resolveReady(); }
        });
        host.once('error', error => { clearTimeout(timer); reject(error); });
        host.once('exit', code => { clearTimeout(timer); reject(new Error(`Host fixture exited: ${code} ${output}`)); });
      });
      const probe = expected => spawnSync(electron, ['-e', `
        const assert = require('node:assert/strict');
        const info = require(${JSON.stringify(resolve(release, 'vr_bridge.node'))}).getRuntimeInfo();
        assert.equal(info.openxrHostDetected, ${expected});
        assert.equal(info.openxrCompanionConnected, false);
        ${expected ? `assert.equal(info.openxrHostRuntimeName, 'host-runtime-test');
        assert.equal(info.openxrHostRuntimeVersion, '7.5.3');
        assert.equal(info.selectedBackend, 'openxr');
        assert.equal(info.openxrMode, 'api-layer');` : ''}
        console.log(JSON.stringify(info));
      `], { encoding: 'utf8', timeout: 5000, env: { ...process.env,
        ELECTRON_RUN_AS_NODE: '1', ELECTRON_VR_DISABLE_OPENVR: '1',
        XR_RUNTIME_JSON: '/nonexistent/electron-vr-test-runtime.json', XDG_DATA_HOME: directory } });
      const detected = probe(true);
      assert.equal(detected.status, 0, detected.stderr + detected.stdout);
      const stopped = once(host, 'exit');
      host.kill('SIGTERM');
      await stopped;
      const absent = probe(false);
      assert.equal(absent.status, 0, absent.stderr + absent.stdout);
    } finally {
      if (host.exitCode === null && host.signalCode === null) host.kill('SIGKILL');
      await rm(directory, { recursive: true, force: true });
    }
  });
