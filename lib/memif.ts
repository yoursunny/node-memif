import * as fs from "fs";
import * as path from "path";
import { Duplex } from "stream";

interface NativeMemif {
  readonly counters: Memif.Counters;
  send: (b: Uint8Array) => void;
  close: () => void;
}

interface NativeMemifOptions {
  socketName: string;
  id: number;
  dataroom: number;
  ringCapacityLog2: number;
  isServer: boolean;
  rx: (b: Uint8Array) => void;
  state: (up: boolean) => void;
}

type NativeMemifConstructor = new(opts: NativeMemifOptions) => NativeMemif;

function newNativeMemif(opts: NativeMemifOptions): NativeMemif {
  let addon: { Memif: NativeMemifConstructor };
  try {
    addon = require("../build/Release/memif-native");
  } catch (err: unknown) {
    let suggest = "(no specific suggestion)";
    switch (true) {
      case process.platform !== "linux" || process.arch !== "x64":
        suggest = `os=${process.platform} cpu=${process.arch} are not supported`;
        break;
      case !fs.existsSync("/usr/local/lib/libmemif.so"):
        suggest = "/usr/local/lib/libmemif.so does not exist, reinstall libmemif";
        break;
      case !fs.existsSync("../build/Release/memif-native.node"):
        suggest = "memif-native.node does not exist, reinstall node-memif";
        break;
    }
    throw new Error(`cannot load memif C++ addon: ${suggest}\n${err}`);
  }
  return new addon.Memif(opts);
}

const activeSocketNames = new Set<string>();

/**
 * Shared Memory Packet Interface (memif).
 *
 * This class wraps libmemif as a Duplex stream in object mode.
 * Received packets can be read from the stream as Uint8Array.
 * To transmit a packet, write a ArrayBufferView or ArrayBuffer to the stream.
 */
export class Memif extends Duplex {
  constructor({
    socketName,
    id = 0,
    dataroom = 2048,
    ringCapacity = 1024,
    role = "client",
  }: Memif.Options) {
    super({
      allowHalfOpen: false,
      objectMode: true,
    });

    socketName = path.resolve(socketName);
    if (activeSocketNames.has(socketName)) {
      throw new Error("socketName is in use");
    }
    this.socketName = socketName;
    if (!(Number.isInteger(id) && id >= 0 && id <= 0xFFFFFFFF)) {
      throw new RangeError("id out of range");
    }
    dataroom = 2 ** Math.ceil(Math.log2(dataroom));
    if (!(dataroom >= 512 && dataroom <= 0xFFFF)) {
      throw new RangeError("dataroom out of range");
    }
    const ringCapacityLog2 = Math.ceil(Math.log2(ringCapacity));
    if (!(ringCapacityLog2 >= 4 && ringCapacityLog2 <= 15)) {
      throw new RangeError("ringCapacity out of range");
    }

    this.native = newNativeMemif({
      socketName,
      id,
      dataroom,
      ringCapacityLog2,
      isServer: role === "server",
      rx: this.handleRx,
      state: this.handleState,
    });
    activeSocketNames.add(socketName);
  }

  /**
   * Determine whether memif is connected to the peer.
   * You may listen for "up" and "down" events to get updates on connectivity change.
   */
  public get connected(): boolean {
    return this.connected_;
  }

  /** Retrieve counters of incoming and outgoing packets. */
  public get counters(): Memif.Counters {
    return this.native.counters;
  }

  override _read(size: number): void {
    void size;
  }

  override _write(chunk: any, encoding: BufferEncoding, callback: (error?: Error | null) => void): void {
    void encoding;

    let u8: Uint8Array;
    if (chunk instanceof Uint8Array) {
      u8 = chunk;
    } else if (chunk instanceof ArrayBuffer) {
      u8 = new Uint8Array(chunk);
    } else if (ArrayBuffer.isView(chunk)) {
      u8 = new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    } else {
      callback(new TypeError("chunk must be ArrayBufferView or ArrayBuffer"));
      return;
    }

    try {
      this.native.send(u8);
    } catch (err: unknown) {
      callback(err as Error);
      return;
    }
    callback();
  }

  override _destroy(error: Error | null, callback: (error: Error | null) => void): void {
    this.native.close();
    activeSocketNames.delete(this.socketName);
    callback(error);
  }

  private readonly native: NativeMemif;
  private readonly socketName: string;
  private connected_ = false;

  private readonly handleRx = (b: Uint8Array) => {
    this.push(b);
  };

  private readonly handleState = (up: boolean) => {
    this.connected_ = up;
    this.emit(up ? "up" : "down");
  };
}

export namespace Memif {
  export type Role = "client" | "server";

  export interface Options {
    /** Control socket role. */
    role?: Role;

    /** Control socket filename. */
    socketName: string;

    /**
     * Interface ID.
     * This must be between 0 and 0xFFFFFFFF. Default is 0.
     */
    id?: number;

    /**
     * Packet buffer size, automatically adjusted to next power of 2.
     * This must be between 512 and 32768. Default is 2048.
     */
    dataroom?: number;

    /**
     * Ring capacity, automatically adjusted to next power of 2.
     * This must be between 16 and 32768. Default is 1024.
     */
    ringCapacity?: number;
  }

  export interface Counters {
    nRxDelivered: bigint;
    nRxDropped: bigint;
    nTxDelivered: bigint;
    nTxDropped: bigint;
  }
}
