import { rm } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");

await Promise.all([
  rm(resolve(projectRoot, "dist"), { force: true, recursive: true }),
  rm(resolve(projectRoot, "build"), { force: true, recursive: true })
]);
