import { createHash } from "node:crypto";
import { createSocket, type Socket } from "node:dgram";
import { isIPv4 } from "node:net";
import type { NodeConfig } from "./index.js";

export interface DiscoveredStream {
  id: string;
  sourceAddress: string;
  sessionName: string;
  destination: string;
  destinationPort: number;
  channelCount: number;
  payloadType: number;
  lastSeen: string;
}

export interface DiscoveryRegistry {
  start(): Promise<void>;
  close(): void;
  list(): DiscoveredStream[];
  get(id: string): DiscoveredStream | undefined;
}

export interface DiscoveryPublisher {
  publish(config: NodeConfig): Promise<void>;
  withdraw(): Promise<void>;
  close(): void;
}

const sapAddress = "224.2.127.254";
const sapPort = 9875;

export class SapAnnouncer implements DiscoveryPublisher {
  private socket: Socket | undefined;
  private timer: NodeJS.Timeout | undefined;
  private packet: Buffer | undefined;

  public async publish(config: NodeConfig): Promise<void> {
    await this.withdraw();
    this.socket = createSocket("udp4");
    this.packet = createSapPacket(createSdp(config), config);
    await new Promise<void>((resolve, reject) => {
      this.socket!.once("error", reject);
      this.socket!.bind(() => {
        this.socket!.off("error", reject);
        this.announce();
        this.timer = setInterval(() => this.announce(), 30_000);
        this.timer.unref();
        resolve();
      });
    });
  }

  public async withdraw(): Promise<void> {
    if (this.timer !== undefined) clearInterval(this.timer);
    this.timer = undefined;
    this.packet = undefined;
    if (this.socket !== undefined) {
      await new Promise<void>((resolve) => this.socket!.close(() => resolve()));
      this.socket = undefined;
    }
  }

  public close(): void {
    void this.withdraw();
  }

  private announce(): void {
    if (this.socket === undefined || this.packet === undefined) return;
    this.socket.send(this.packet, sapPort, sapAddress);
  }
}

export class SapRegistry implements DiscoveryRegistry {
  private socket: Socket | undefined;
  private readonly streams = new Map<string, DiscoveredStream>();

  public async start(): Promise<void> {
    if (this.socket !== undefined) return;
    this.socket = createSocket({ type: "udp4", reuseAddr: true });
    this.socket.on("message", (message, remote) => this.receive(message, remote.address));
    await new Promise<void>((resolve, reject) => {
      this.socket!.once("error", reject);
      this.socket!.bind(sapPort, "0.0.0.0", () => {
        this.socket!.off("error", reject);
        try {
          this.socket!.addMembership(sapAddress);
          resolve();
        } catch (error) {
          reject(error);
        }
      });
    });
  }

  public close(): void {
    if (this.socket !== undefined) this.socket.close();
    this.socket = undefined;
    this.streams.clear();
  }

  public list(): DiscoveredStream[] {
    const expiry = Date.now() - 90_000;
    for (const [id, stream] of this.streams) {
      if (Date.parse(stream.lastSeen) < expiry) this.streams.delete(id);
    }
    return [...this.streams.values()];
  }

  public get(id: string): DiscoveredStream | undefined {
    return this.list().find((stream) => stream.id === id);
  }

  private receive(message: Buffer, sourceAddress: string): void {
    const stream = parseSapAnnouncement(message, sourceAddress);
    if (stream === undefined) return;
    this.streams.set(stream.id, stream);
  }
}

export function parseSapAnnouncement(message: Buffer, sourceAddress: string): DiscoveredStream | undefined {
  if (message.length < 8 || (message[0] & 0xe0) !== 0x20 || (message[0] & 0x10) !== 0 ||
    !isIPv4(sourceAddress)) return undefined;
    const messageId = message.readUInt16BE(2).toString(16).padStart(4, "0");
    const separator = message.indexOf(0, 8);
    if (separator < 0 || message.subarray(8, separator).toString("ascii") !== "application/sdp") return undefined;
    const sdp = message.subarray(separator + 1).toString("utf8");
    if (lineValue(sdp, "v") !== "0" || !lineValue(sdp, "o") || !lineValue(sdp, "t") ||
      lineValue(sdp, "a", "ts-refclk:") !== "ptp=IEEE1588-2008:0" ||
      lineValue(sdp, "a", "mediaclk:") !== "direct=0") return undefined;
    const sessionName = lineValue(sdp, "s");
    const connection = lineValue(sdp, "c").split(/\s+/);
    const media = lineValue(sdp, "m").split(/\s+/);
    const rtpmap = lineValue(sdp, "a", "rtpmap:").split(/\s+/);
    const codec = rtpmap[1]?.split("/");
    const port = Number(media[1]);
    const payloadType = Number(media[3]);
    const channels = Number(codec?.[2]);
    if (!sessionName || connection.length !== 3 || connection[0] !== "IN" || connection[1] !== "IP4" ||
      connection[2] === undefined || !isIPv4(connection[2]) || media.length !== 4 ||
      media[0] !== "audio" || media[2] !== "RTP/AVP" || !Number.isInteger(port) || port <= 0 || port >= 65536 ||
      !Number.isInteger(payloadType) || payloadType < 96 || payloadType > 127 ||
      rtpmap[0] !== String(payloadType) || codec?.[0] !== "L24" || codec?.[1] !== "48000" || channels !== 2) return undefined;
    const id = `${sourceAddress}-${messageId}`;
      return {
      id,
      sourceAddress,
      sessionName,
      destination: connection[2],
      destinationPort: port,
      channelCount: channels,
      payloadType,
      lastSeen: new Date().toISOString(),
    };
}

function lineValue(sdp: string, type: string, prefix = ""): string {
  const line = sdp.split(/\r?\n/).find((entry) => entry.startsWith(`${type}=`) && entry.slice(2).startsWith(prefix));
  return line?.slice(2 + prefix.length) ?? "";
}

function createSdp(config: NodeConfig): string {
  return [
    "v=0",
    "o=- 1 1 IN IP4 0.0.0.0",
    "s=AES67 stream",
    `c=IN IP4 ${config.destination}`,
    "t=0 0",
    "a=ts-refclk:ptp=IEEE1588-2008:0",
    `m=audio ${config.destinationPort} RTP/AVP ${config.payloadType}`,
    `a=rtpmap:${config.payloadType} L24/48000/${config.channelCount}`,
    "a=mediaclk:direct=0",
    "",
  ].join("\r\n");
}

function createSapPacket(sdp: string, config: NodeConfig): Buffer {
  const packet = Buffer.alloc(8);
  packet[0] = 0x20;
  const messageId = createHash("sha1")
    .update(`${config.nodeId}:${config.destination}:${config.destinationPort}`)
    .digest();
  packet.writeUInt16BE(messageId.readUInt16BE(0), 2);
  return Buffer.concat([packet, Buffer.from("application/sdp\0", "ascii"), Buffer.from(sdp, "ascii")]);
}
