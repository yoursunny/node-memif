# Shared Memory Packet Interface (memif) for Node.js

This package is Node-API wrapper of [libmemif](https://docs.fd.io/vpp/21.06/dc/dea/libmemif_doc.html), which provides high performance packet transmit and receive between Node.js and VPP/DPDK applications.
It works on Linux only and requires libmemif installed at `/usr/local/lib/libmemif.so`.

## API Sample

```js
const { Memif } = require("memif");

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
});

// Writable side of the stream allows transmitting packets.
// It accepts ArrayBufferView (including Uint8Array and Buffer) and ArrayBuffer.
memif.send(Uint8Array.of(0x01, 0x02));

// Be sure to close the interface when no longer needed.
memif.close();
```

## Limitations

Only one `Memif` instance is allowed in each Node.js process.
