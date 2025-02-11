#!/usr/bin/env node
import assert from "node:assert/strict";
import crypto from "node:crypto";
import path from "node:path";
import { setTimeout as delay } from "node:timers/promises";

import { execaNode } from "execa";
import { pEvent, pEventMultiple } from "p-event";
import tmp from "tmp";

import { Memif } from "../dist/memif.js";

const tmpDir = tmp.dirSync({ unsafeCleanup: true });
process.on("beforeExit", tmpDir.removeCallback);

const socketName = `${tmpDir.name}/memif.sock`;
const helper = execaNode(path.join(import.meta.dirname, "server.js"), [socketName], {
  stdin: "ignore",
  stdout: "inherit",
  stderr: "inherit",
  serialization: "advanced",
});

const memif = new Memif({ socketName });
assert.equal(memif.dataroom, 2048);
assert(!memif.connected);
await pEvent(memif, "memif:up");
assert(memif.connected);

const msg0 = crypto.randomBytes(1500);
const msg1 = crypto.randomFillSync(new Uint8Array(new SharedArrayBuffer(3000)));
const msg2 = crypto.randomBytes(1000);
const msg3 = crypto.randomBytes(5500);
const msg4 = crypto.randomBytes(50);

setTimeout(async () => {
  memif.write(msg0);
  await delay(10);
  memif.write(msg1);
}, 0);
setTimeout(async () => {
  helper.send(msg2);
  await delay(10);
  helper.send(msg3);
}, 0);
const [[rcv0, rcv1], [rcv2, rcv3]] = await Promise.all([
  pEventMultiple(helper, "message", { count: 2, timeout: 2000 }),
  pEventMultiple(memif, "data", { count: 2, timeout: 2000 }),
]);

assert.equal(Buffer.compare(msg0, rcv0), 0);
assert.equal(Buffer.compare(msg1, rcv1), 0);
assert.equal(Buffer.compare(msg2, rcv2), 0);
assert.equal(Buffer.compare(msg3, rcv3), 0);

assert(memif.connected);
helper.disconnect();
await Promise.all([
  pEvent(memif, "memif:down"),
  pEvent(helper, "exit"),
]);
assert(!memif.connected);
memif.write(msg4);

const cnt = memif.counters;
assert.equal(cnt.nRxPackets, 2n);
assert.equal(cnt.nRxFragments, 4n);
assert.equal(cnt.nTxPackets, 2n);
assert.equal(cnt.nTxFragments, 3n);
assert.equal(cnt.nTxDropped, 1n);

memif.destroy();
