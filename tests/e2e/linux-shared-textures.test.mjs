import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { copyFile, mkdir, mkdtemp, stat, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const require = createRequire(import.meta.url);
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

for (const api of ["glx", "vulkan"]) test(`${api} copies live Electron DMA-BUFs on the GPU without bitmap recovery`, {
  skip: process.platform !== "linux",
  timeout: 240000
}, async (t) => {
  const directory = await mkdtemp(join(tmpdir(), "evr-gpu-"));
  const build = join(root, "packages/native-addon/build/Release");
  const layer = join(directory, "data/openxr/1/api_layers/implicit.d");
  const artifacts = join(root, "artifacts");
  await mkdir(layer, { recursive: true, mode: 0o700 });
  await mkdir(artifacts, { recursive: true });
  await copyFile(join(build, "libelectron_vr_openxr_layer.so"), join(layer, "libelectron_vr_openxr_layer.so"));
  await copyFile(join(build, "electron_vr_openxr_layer_linux.json"), join(layer, "electron_vr_openxr_layer.json"));
  const env = { ...process.env, XDG_RUNTIME_DIR: directory, XDG_DATA_HOME: join(directory, "data"),
    XR_RUNTIME_JSON: process.env.XR_RUNTIME_JSON ?? "/usr/share/openxr/1/openxr_monado.json",
    ELECTRON_VR_PROBE_SAMPLE_INTERVAL: "60",
    ELECTRON_VR_PROBE_PATTERN: "quadrants",
    ELECTRON_VR_FORCE_OPENXR_API_LAYER: "1", ELECTRON_VR_DISABLE_OPENVR: "1" };
  let producerLog = "";
  let producerLogAtCompletion = null;
  let probeLog = "";
  const producer = spawn(require("electron"), [join(root, "tests/fixtures/gpu-texture-producer.mjs"), "--no-sandbox"],
    { cwd: root, env, stdio: ["ignore", "pipe", "pipe"] });
  producer.stdout.on("data", (data) => { producerLog += data; });
  producer.stderr.on("data", (data) => { producerLog += data; });
  let probe;
  const abort = () => { probe?.kill("SIGKILL"); producer.kill("SIGKILL"); };
  t.signal.addEventListener("abort", abort, { once: true });
  try {
    const socket = join(directory, "electron-vr/electron-vr-openxr-overlay-v3.sock");
    const deadline = Date.now() + 20000;
    while (!(await stat(socket).catch(() => null))) {
      if (producer.exitCode !== null || Date.now() > deadline) throw new Error(`No companion socket:\n${producerLog}`);
      await sleep(100);
    }
    probe = spawn(join(build, `electron_vr_${api}_dmabuf_probe`), [process.env.ELECTRON_VR_GPU_TEST_FRAMES ?? "6000"],
      { cwd: root, env, stdio: ["ignore", "pipe", "pipe"] });
    probe.stdout.on("data", (data) => {
      probeLog += data;
      const finished = api === "glx" ? probeLog.includes("GPU GLX copy result:") : /\nframes=\d+ changed=\d+/.test(probeLog);
      if (finished && producerLogAtCompletion === null) {
        producerLogAtCompletion = producerLog;
        producer.kill("SIGTERM");
      }
    });
    probe.stderr.on("data", (data) => { probeLog += data; });
    const exit = await new Promise((done, reject) => {
      probe.once("error", reject);
      probe.once("exit", (code, signal) => done({ code, signal }));
    });
    assert.equal(exit.code, 0, `${JSON.stringify(exit)}\n${probeLog}\nProducer:\n${producerLog}`);
    assert.match(probeLog, api === "glx" ? /GPU GLX copy result: frames=\d+ distinct=(?:[2-9]|\d{2,})/
      : /frames=\d+ changed=(?:[2-9]|\d{2,})/);
    assert.doesNotMatch(producerLogAtCompletion ?? producerLog,
      /Failed to submit Linux|Error while forwarding|Failed to initialize VR bridge/);
  } finally {
    t.signal.removeEventListener("abort", abort);
    probe?.kill("SIGTERM");
    producer.kill("SIGTERM");
    await sleep(500);
    if (producer.exitCode === null) producer.kill("SIGKILL");
    if (probe && probe.exitCode === null) probe.kill("SIGKILL");
    await writeFile(join(artifacts, `strict-${api}-producer.log`), producerLog);
    await writeFile(join(artifacts, `strict-${api}-probe.log`), probeLog);
  }
});
