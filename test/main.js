#!/usr/bin/env node
import assert from "node:assert/strict";
import crypto from "node:crypto";
import path from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";

import { execaNode } from "execa";
import { pEvent, pEventMultiple } from "p-event";
import tmp from "tmp";

import { Memif } from "../dist/memif.js";

const tmpDir = tmp.dirSync({ unsafeCleanup: true });
process.on("beforeExit", tmpDir.removeCallback);

const socketName = `${tmpDir.name}/memif.sock`;
const helper = execaNode(path.resolve(path.dirname(fileURLToPath(import.meta.url)), "server.js"), [socketName], {
  stdin: "ignore",
  stdout: "inherit",
  stderr: "inherit",
  serialization: "advanced",
});

const memif = new Memif({ socketName });
assert(!memif.connected);
await pEvent(memif, "memif:up");
assert(memif.connected);

const msg0 = crypto.randomBytes(1500);
const msg1 = crypto.randomBytes(2000);
const msg2 = crypto.randomBytes(1000);

setTimeout(async () => {
  memif.write(msg0);
  await delay(10);

  // @ts-expect-error
  const native = memif.native;
  const chunk0 = msg1.subarray(0, 1200);
  const chunk1 = msg1.subarray(1200, 2000);
  native.send(chunk0.buffer, chunk0.byteOffset, chunk0.byteLength, true);
  native.send(chunk1.buffer, chunk1.byteOffset, chunk1.byteLength, false);
}, 0);
setTimeout(() => {
  helper.send(msg2);
}, 0);
const [[rcv0, rcv1], rcv2] = await Promise.all([
  pEventMultiple(helper, "message", { count: 2 }),
  pEvent(memif, "data"),
]);

assert(msg0.equals(rcv0));
assert(msg1.equals(rcv1));
assert(msg2.equals(rcv2));

assert(memif.connected);
helper.disconnect();
await Promise.all([
  pEvent(memif, "memif:down"),
  pEvent(helper, "exit"),
]);
assert(!memif.connected);

const cnt = memif.counters;
assert.equal(cnt.nRxPackets, 1n);
assert.equal(cnt.nRxFragments, 1n);
assert.equal(cnt.nTxPackets, 2n);
assert.equal(cnt.nTxFragments, 3n);
assert.equal(cnt.nTxDropped, 0n);

memif.destroy();
