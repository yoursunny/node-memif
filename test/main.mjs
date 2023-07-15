#!/usr/bin/env node
import assert from "node:assert/strict";
import crypto from "node:crypto";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { execaNode } from "execa";
import { pEvent } from "p-event";

const require = createRequire(import.meta.url);

const { Memif } = require("..");
const tmp = require("tmp");

const tmpDir = tmp.dirSync({ unsafeCleanup: true });
process.on("beforeExit", tmpDir.removeCallback);

const socketName = `${tmpDir.name}/memif.sock`;
const helper = execaNode(path.resolve(path.dirname(fileURLToPath(import.meta.url)), "server.cjs"), [socketName], {
  stdin: "ignore",
  stdout: "inherit",
  stderr: "inherit",
  serialization: "advanced",
});

const memif = new Memif({ socketName });
assert(!memif.connected);
await pEvent(memif, "memif:up");
assert(memif.connected);

const c2s = crypto.randomBytes(1024);
const s2c = crypto.randomBytes(1024);

memif.write(c2s);
helper.send(s2c);
const [c2sR, s2cR] = await Promise.all([
  pEvent(helper, "message"),
  pEvent(memif, "data"),
]);

assert(c2s.equals(c2sR));
assert(s2c.equals(s2cR));

assert(memif.connected);
helper.disconnect();
await Promise.all([
  pEvent(memif, "memif:down"),
  pEvent(helper, "exit"),
]);
assert(!memif.connected);

memif.destroy();
