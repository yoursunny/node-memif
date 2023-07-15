# Shared Memory Packet Interface (memif) for Node.js

[![npm package version](https://img.shields.io/npm/v/memif)](https://www.npmjs.com/package/memif) [![GitHub Workflow status](https://img.shields.io/github/actions/workflow/status/yoursunny/node-memif/build.yml?style=flat)](https://github.com/yoursunny/node-memif/actions) [![GitHub code size](https://img.shields.io/github/languages/code-size/yoursunny/node-memif?style=flat)](https://github.com/yoursunny/node-memif)

This package is a Node C++ addon of [libmemif](https://s3-docs.fd.io/vpp/23.06/interfacing/libmemif/), which provides high performance packet transmit and receive between Node.js and VPP/DPDK applications.
It works on Linux only and requires libmemif 4.0 installed at `/usr/local/lib/libmemif.so`.

## API Example

```js
import { Memif } from "memif";

// Memif class is a Node.js Duplex stream.
const memif = new Memif({
  role: "client",
  socketName: "/run/memif.sock",
  id: 0,
  dataroom: 2048,
  ringCapacity: 1024,
});

// Readable side of the stream gives access to received packets.
memif.on("data", (pkt) => {
  // pkt is a Uint8Array containing received packet.
  // Fragmented messages with MEMIF_BUFFER_FLAG_NEXT are concatenated.
});

// Writable side of the stream allows transmitting packets.
// It accepts ArrayBufferView (including Uint8Array and Buffer) and ArrayBuffer.
memif.send(Uint8Array.of(0x01, 0x02));

// Be sure to close the interface when no longer needed.
memif.close();
```

## Limitations

Each `Memif` instance must have a distinct `socketName`.
