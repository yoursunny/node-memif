import * as path from "path";
import { Duplex } from "stream";

// @ts-expect-error native module
import addon = require("../build/Release/memif-native");

interface AddonMemif {
  send: (b: Uint8Array) => void;
  close: () => void;
}

interface AddonMemifOptions {
  socketName: string;
  id: number;
  dataroom: number;
  ringSizeLog2: number;
  isServer: boolean;
  rx: (b: Uint8Array) => void;
  state: (up: boolean) => void;
}

type AddonMemifConstructor = new(opts: AddonMemifOptions) => AddonMemif;

const AddonMemif = addon.Memif as AddonMemifConstructor;

const activeSocketNames = new Set<string>();

export class Memif extends Duplex {
  constructor({
    socketName,
    id = 0,
    dataroom = 2048,
    ringSize = 1024,
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
    if (!(dataroom >= 64 && dataroom <= 0xFFFF)) {
      throw new RangeError("dataroom out of range");
    }
    const ringSizeLog2 = Math.ceil(Math.log2(ringSize));
    if (!(ringSizeLog2 >= 4 && ringSizeLog2 <= 15)) {
      throw new RangeError("ringSize out of range");
    }

    this.native = new AddonMemif({
      socketName,
      id,
      dataroom,
      ringSizeLog2,
      isServer: role === "server",
      rx: this.handleRx,
      state: this.handleState,
    });
    activeSocketNames.add(socketName);
  }

  public get connected(): boolean {
    return this.connected_;
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
      callback(err as Error | undefined);
    }
  }

  override _destroy(error: Error | null, callback: (error: Error | null) => void): void {
    this.native.close();
    activeSocketNames.delete(this.socketName);
    callback(error);
  }

  private readonly native: AddonMemif;
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
    socketName: string;
    id?: number;
    dataroom?: number;
    ringSize?: number;
    role?: Role;
  }
}
