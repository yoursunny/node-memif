#!/usr/bin/env node
import assert from "node:assert/strict";
import crypto from "node:crypto";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";

import { execaNode } from "execa";
import { pEvent, pEventMultiple } from "p-event";
import tmp from "tmp";

import { Memif } from "../dist/memif.js";

const tmpDir = tmp.dirSync({ unsafeCleanup: true });
process.on("beforeExit", tmpDir.removeCallback);

const socketName = `${tmpDir.name}/memif.sock`;
const helper = execaNode(fileURLToPath(new URL("server.js", import.meta.url)), [socketName], {
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
const msg1 = crypto.randomBytes(3000);
const msg2 = crypto.randomBytes(1000);
const msg3 = crypto.randomBytes(5500);
const msg4 = new ArrayBuffer(50);
crypto.randomFillSync(new Uint8Array(msg4));

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

assert(msg0.equals(rcv0));
assert(msg1.equals(rcv1));
assert(msg2.equals(rcv2));
assert(msg3.equals(rcv3));

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
