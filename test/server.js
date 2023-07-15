#!/usr/bin/env node
import { Memif } from "../dist/memif.js";

if (!process.connected) {
  process.exit(1);
}

const socketName = process.argv[2];
const memif = new Memif({ socketName, role: "server" });

memif.on("data", (chunk) => {
  process.send(chunk);
});
process.on("message", (chunk) => {
  memif.write(chunk);
});

process.on("disconnect", () => {
  memif.destroy();
});
