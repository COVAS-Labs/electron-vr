import assert from "node:assert/strict";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const addon = require("@covas-labs/vr-bridge-prebuilt-node-linux-x64");
const runtimeInfo = addon.getRuntimeInfo();

assert.equal(typeof runtimeInfo.platform, "string");
assert.equal(typeof runtimeInfo.selectedBackend, "string");

console.log("Node prebuilt runtime info:", runtimeInfo);
