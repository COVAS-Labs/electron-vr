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

for (const [api, label] of [["OpenGL", "GLX"], ["Vulkan", "Vulkan"]]) test(`real Monado ${label} session consumes GPU frames without software recovery`, {
  skip: process.platform !== "linux" || !process.env.ELECTRON_VR_TEST_HELLO_XR,
  timeout: 120000
}, async (t) => {
  const directory = await mkdtemp(join(tmpdir(), "evr-xr-"));
  const layer = join(directory, "data/openxr/1/api_layers/implicit.d");
  const build = join(root, "packages/native-addon/build/Release");
  const artifacts = join(root, "artifacts");
  await mkdir(layer, { recursive: true, mode: 0o700 });
  await mkdir(artifacts, { recursive: true });
  await copyFile(join(build, "libelectron_vr_openxr_layer.so"), join(layer, "libelectron_vr_openxr_layer.so"));
  await copyFile(join(build, "electron_vr_openxr_layer_linux.json"), join(layer, "electron_vr_openxr_layer.json"));
  const env = { ...process.env, XDG_RUNTIME_DIR: directory, XDG_DATA_HOME: join(directory, "data"),
    XR_RUNTIME_JSON: "/usr/share/openxr/1/openxr_monado.json", SIMULATED_ENABLE: "1", XRT_COMPOSITOR_NULL: "1",
    ELECTRON_VR_FORCE_OPENXR_API_LAYER: "1", ELECTRON_VR_DISABLE_OPENVR: "1" };
  const children = [];
  const logs = { service: "", host: "", producer: "" };
  function start(key, executable, args) {
    // Monado polls stdin with epoll and hello_xr exits on stdin EOF. Keep a
    // pipe open for both rather than giving either process /dev/null.
    const child = spawn(executable, args, { env, cwd: root, stdio: [key === "producer" ? "ignore" : "pipe", "pipe", "pipe"] });
    child.stdout.on("data", (data) => { logs[key] += data; });
    child.stderr.on("data", (data) => { logs[key] += data; });
    children.push(child);
    return child;
  }
  const abort = () => { for (const child of children) child.kill("SIGKILL"); };
  t.signal.addEventListener("abort", abort, { once: true });
  try {
    const service = start("service", "/usr/bin/monado-service", []);
    const deadline = Date.now() + 15000;
    while (!(await stat(join(directory, "monado_comp_ipc")).catch(() => null))) {
      if (service.exitCode !== null || Date.now() > deadline) throw new Error(`No Monado socket:\n${logs.service}`);
      await sleep(100);
    }
    const host = start("host", process.env.ELECTRON_VR_TEST_HELLO_XR, ["-g", api]);
    await sleep(2000);
    assert.equal(host.exitCode, null, logs.host);
    const producer = start("producer", require("electron"), [join(root, "apps/demo-electron"), "--no-sandbox"]);
    await sleep(20000);
    assert.equal(host.exitCode, null, logs.host);
    assert.equal(producer.exitCode, null, logs.producer);
    assert.equal(service.exitCode, null, logs.service);
    assert.match(logs.producer, /connected=true host=(?:opengl-xlib|vulkan) submitted=[1-9]\d* consumed=[1-9]\d*/);
    assert.match(logs.host, new RegExp(`Linux OpenXR ${label} DMA-BUF GPU frame count=`));
    assert.doesNotMatch(logs.producer, /Failed to submit Linux|Error while forwarding|Failed to initialize VR bridge/);
    producer.kill("SIGTERM");
    await sleep(2000);
    assert.equal(host.exitCode, null, "Host must continue after the companion stops");
    assert.equal(service.exitCode, null, "Runtime must continue after the companion stops");
  } finally {
    t.signal.removeEventListener("abort", abort);
    for (const child of children.toReversed()) child.kill("SIGTERM");
    await sleep(500);
    for (const child of children) if (child.exitCode === null) child.kill("SIGKILL");
    for (const [key, log] of Object.entries(logs)) await writeFile(join(artifacts, `strict-monado-${api}-${key}.log`), log);
  }
});
