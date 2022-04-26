#!/usr/bin/env node
const { Memif } = require("..");
const { strict: assert } = require("node:assert");
const crypto = require("node:crypto");
const execa = require("execa");
const path = require("path");
const pEvent = require("p-event");
const tmp = require("tmp");

const tmpDir = tmp.dirSync({ unsafeCleanup: true });
process.on("beforeExit", tmpDir.removeCallback);

const socketName = `${tmpDir.name}/memif.sock`;
const helper = execa.node(path.resolve(__dirname, "server.js"), [socketName], {
  stdin: "ignore",
  stdout: "inherit",
  stderr: "inherit",
  serialization: "advanced",
});

(async () => {
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
})();
