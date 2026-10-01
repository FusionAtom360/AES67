import { createHash } from "node:crypto";
import { spawn, type ChildProcess } from "node:child_process";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { connect as connectSocket } from "node:net";
import { resolve } from "node:path";
import type { Duplex } from "node:stream";
import { pathToFileURL } from "node:url";
import { SapRegistry, type DiscoveryPublisher, type DiscoveryRegistry } from "./discovery.js";

export interface NodeConfig {
  nodeId: string;
  channelCount: number;
  destination: string;
  destinationPort: number;
}

export interface NodeStatus {
  profile: "AES67-L24-48k-1ms";
  ptp: "locked" | "unlocked";
  stream: "stopped" | "running";
  config: NodeConfig;
  subscription: {
    active: boolean;
    destination: string;
    destinationPort: number;
  };
  volume: number;
  health: {
    ok: boolean;
    ptpLocked: boolean;
    streamRunning: boolean;
  };
}

export const defaultConfig: NodeConfig = {
  nodeId: "speaker-node-01",
  channelCount: 2,
  destination: "239.69.0.1",
  destinationPort: 5004,
};

export interface SpeakerRuntime {
  subscribe(config: NodeConfig): Promise<void>;
  unsubscribe(): Promise<void>;
  setVolume(volume: number): Promise<void>;
  isRunning(): boolean;
}

export class ProcessSpeakerRuntime implements SpeakerRuntime {
  private child: ChildProcess | undefined;
  private socketPath: string | undefined;

  public async subscribe(config: NodeConfig): Promise<void> {
    await this.unsubscribe();
    const executable = process.env.AES67_SPEAKER_EXECUTABLE ?? resolve(process.cwd(), "../build/aes67_speaker_node");
    const socketName = config.nodeId.replace(/[^a-zA-Z0-9_.-]/g, "_");
    this.socketPath = process.env.AES67_CONTROL_SOCKET ?? `/tmp/aes67-${socketName}.sock`;
    const args = isMulticast(config.destination)
      ? ["--multicast", config.destination]
      : ["--bind", "0.0.0.0"];
    args.push("--port", String(config.destinationPort), "--payload-type", "96",
      "--device", process.env.AES67_ALSA_DEVICE ?? "default", "--control-socket", this.socketPath);
    const child = spawn(executable, args, { stdio: "ignore" });
    this.child = child;
    await new Promise<void>((resolvePromise, reject) => {
      const onError = (error: Error) => reject(error);
      child.once("error", onError);
      child.once("spawn", () => {
        child.off("error", onError);
        resolvePromise();
      });
    });
    child.once("exit", () => {
      if (this.child === child) this.child = undefined;
    });
  }

  public async unsubscribe(): Promise<void> {
    const child = this.child;
    if (child === undefined) return;
    this.child = undefined;
    if (child.exitCode !== null || child.signalCode !== null) return;
    await new Promise<void>((resolvePromise) => {
      child.once("exit", () => resolvePromise());
      child.kill("SIGTERM");
    });
  }

  public async setVolume(volume: number): Promise<void> {
    if (!this.isRunning() || this.socketPath === undefined) {
      throw new Error("speaker is not subscribed");
    }
    let lastError: unknown;
    for (let attempt = 0; attempt < 20; ++attempt) {
      try {
        await new Promise<void>((resolvePromise, reject) => {
          const socket = connectSocket(this.socketPath!, () => {
            socket.end(`volume ${volume}\n`, resolvePromise);
          });
          socket.once("error", reject);
        });
        return;
      } catch (error: unknown) {
        lastError = error;
        await new Promise((resolvePromise) => setTimeout(resolvePromise, 25));
      }
    }
    throw lastError instanceof Error ? lastError : new Error("could not contact speaker control socket");
  }

  public isRunning(): boolean {
    return this.child !== undefined && this.child.exitCode === null && this.child.signalCode === null;
  }
}

export function createControlServer(initialConfig: NodeConfig = defaultConfig,
                                    runtime: SpeakerRuntime = new ProcessSpeakerRuntime(),
                                    discovery: DiscoveryPublisher = new NoopDiscoveryPublisher(),
                                    registry: DiscoveryRegistry = new SapRegistry()): Server {
  const status: NodeStatus = {
    profile: "AES67-L24-48k-1ms",
    ptp: "unlocked",
    stream: "stopped",
    config: { ...initialConfig },
    subscription: {
      active: false,
      destination: initialConfig.destination,
      destinationPort: initialConfig.destinationPort,
    },
    volume: 1,
    health: { ok: true, ptpLocked: false, streamRunning: false },
  };
  const websocket = new WebSocketHub(status, runtime, discovery);
  const server = createServer((request, response) =>
    handleRequest(request, response, status, websocket, runtime, discovery, registry));
  server.on("upgrade", (request, socket) => websocket.accept(request, socket));
  server.on("close", () => { websocket.close(); discovery.close(); registry.close(); });
  void registry.start().catch(() => undefined);
  return server;
}

function snapshot(status: NodeStatus): NodeStatus {
  return {
    ...status,
    config: { ...status.config },
    subscription: { ...status.subscription },
    health: { ...status.health },
  };
}

function handleRequest(request: IncomingMessage, response: ServerResponse, status: NodeStatus,
                       websocket: WebSocketHub, runtime: SpeakerRuntime,
                       discovery: DiscoveryPublisher, registry: DiscoveryRegistry): void {
  response.setHeader("content-type", "application/json");
  if (request.method === "GET" && request.url === "/health") {
    send(response, 200, status.health);
    return;
  }
  if (request.method === "GET" && request.url === "/v1/status") {
    send(response, 200, snapshot(status));
    return;
  }
  if (request.method === "GET" && request.url === "/v1/discovery/streams") {
    send(response, 200, { streams: registry.list() });
    return;
  }
  if (request.method === "PUT" && request.url === "/v1/config") {
    collectJson(request, (body) => {
      if (!isNodeConfig(body)) {
        send(response, 400, { error: "invalid node configuration" });
        return;
      }
      status.config = body;
      send(response, 200, status.config);
      websocket.broadcast(status);
    });
    return;
  }
  if (request.method === "POST" && request.url === "/v1/subscribe") {
    collectJson(request, (body) => {
      if (body !== undefined && !isSubscriptionRequest(body)) {
        send(response, 400, { error: "invalid subscription" });
        return;
      }
      const subscription = body as Partial<NodeConfig> | undefined;
      if (subscription?.destination !== undefined) status.config.destination = subscription.destination;
      if (subscription?.destinationPort !== undefined) status.config.destinationPort = subscription.destinationPort;
      void runtime.subscribe(status.config).then(() => discovery.publish(status.config)).then(() => {
        status.subscription = {
          active: true,
          destination: status.config.destination,
          destinationPort: status.config.destinationPort,
        };
        status.stream = "running";
        status.health.streamRunning = true;
        send(response, 200, snapshot(status));
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    });
    return;
  }
  if (request.method === "POST" && request.url === "/v1/routes") {
    collectJson(request, (body) => {
      const streamId = isRouteRequest(body) ? body.streamId : undefined;
      const stream = streamId === undefined ? undefined : registry.get(streamId);
      if (stream === undefined) {
        send(response, 404, { error: "discovered stream not found" });
        return;
      }
      const routedConfig = { ...status.config, destination: stream.destination,
        destinationPort: stream.destinationPort, channelCount: stream.channelCount };
      void runtime.subscribe(routedConfig).then(() => {
        status.config = routedConfig;
        status.subscription = { active: true, destination: stream.destination,
          destinationPort: stream.destinationPort };
        status.stream = "running";
        status.health.streamRunning = true;
        send(response, 200, snapshot(status));
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    });
    return;
  }
  if (request.method === "POST" && request.url === "/v1/unsubscribe") {
    void runtime.unsubscribe().then(() => discovery.withdraw()).then(() => {
      status.subscription.active = false;
      status.stream = "stopped";
      status.health.streamRunning = false;
      send(response, 200, snapshot(status));
      websocket.broadcast(status);
    }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    return;
  }
  if (request.method === "GET" && request.url === "/v1/volume") {
    send(response, 200, { volume: status.volume });
    return;
  }
  if (request.method === "PUT" && request.url === "/v1/volume") {
    collectJson(request, (body) => {
      if (!isVolumeRequest(body)) {
        send(response, 400, { error: "volume must be a number from 0 to 1" });
        return;
      }
      void runtime.setVolume(body.volume).then(() => {
        status.volume = body.volume;
        send(response, 200, { volume: status.volume });
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 409, { error: errorMessage(error) }));
    });
    return;
  }
  send(response, 404, { error: "not found" });
}

function collectJson(request: IncomingMessage, callback: (body: unknown) => void): void {
  let content = "";
  request.setEncoding("utf8");
  request.on("data", (chunk: string) => { content += chunk; });
  request.on("end", () => {
    try { callback(JSON.parse(content)); } catch { callback(undefined); }
  });
}

function isNodeConfig(value: unknown): value is NodeConfig {
  if (typeof value !== "object" || value === null) return false;
  const config = value as Record<string, unknown>;
  return typeof config.nodeId === "string" && typeof config.channelCount === "number" &&
    Number.isInteger(config.channelCount) && config.channelCount > 0 &&
    typeof config.destination === "string" && typeof config.destinationPort === "number" &&
    Number.isInteger(config.destinationPort) && config.destinationPort > 0 && config.destinationPort < 65536;
}

function isSubscriptionRequest(value: unknown): value is Partial<NodeConfig> {
  if (typeof value !== "object" || value === null) return false;
  const request = value as Record<string, unknown>;
  return (request.destination === undefined || typeof request.destination === "string") &&
    (request.destinationPort === undefined || (typeof request.destinationPort === "number" &&
      Number.isInteger(request.destinationPort) && request.destinationPort > 0 && request.destinationPort < 65536));
}

function isVolumeRequest(value: unknown): value is { volume: number } {
  if (typeof value !== "object" || value === null) return false;
  const volume = (value as Record<string, unknown>).volume;
  return typeof volume === "number" && Number.isFinite(volume) && volume >= 0 && volume <= 1;
}

function isRouteRequest(value: unknown): value is { streamId: string } {
  return typeof value === "object" && value !== null && typeof (value as Record<string, unknown>).streamId === "string";
}

function isMulticast(address: string): boolean {
  const firstOctet = Number(address.split(".", 1)[0]);
  return Number.isInteger(firstOctet) && firstOctet >= 224 && firstOctet <= 239;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : "speaker runtime failed";
}

class NoopDiscoveryPublisher implements DiscoveryPublisher {
  public async publish(_config: NodeConfig): Promise<void> {}
  public async withdraw(): Promise<void> {}
  public close(): void {}
}

function send(response: ServerResponse, statusCode: number, body: unknown): void {
  response.statusCode = statusCode;
  response.end(JSON.stringify(body));
}

class WebSocketHub {
  private readonly clients = new Set<Duplex>();
  private readonly buffers = new Map<Duplex, Buffer>();

  public constructor(private readonly status: NodeStatus, private readonly runtime: SpeakerRuntime,
                     private readonly discovery: DiscoveryPublisher) {}

  public accept(request: IncomingMessage, socket: Duplex): void {
    if (request.url !== "/v1/events" || request.headers.upgrade?.toLowerCase() !== "websocket") {
      socket.destroy();
      return;
    }
    const key = request.headers["sec-websocket-key"];
    if (typeof key !== "string") {
      socket.destroy();
      return;
    }
    const accept = createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    socket.write("HTTP/1.1 101 Switching Protocols\r\n" +
      "Upgrade: websocket\r\nConnection: Upgrade\r\n" +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    this.clients.add(socket);
    this.buffers.set(socket, Buffer.alloc(0));
    socket.on("data", (data: Buffer) => this.receive(socket, data));
    socket.on("close", () => { this.clients.delete(socket); this.buffers.delete(socket); });
    socket.on("error", () => { this.clients.delete(socket); this.buffers.delete(socket); });
    this.send(socket, { type: "status", data: snapshot(this.status) });
  }

  public broadcast(status: NodeStatus): void {
    for (const client of this.clients) this.send(client, { type: "status", data: snapshot(status) });
  }

  public close(): void {
    for (const client of this.clients) client.destroy();
    this.clients.clear();
    this.buffers.clear();
  }

  private receive(socket: Duplex, data: Buffer): void {
    const buffered = Buffer.concat([this.buffers.get(socket) ?? Buffer.alloc(0), data]);
    this.buffers.set(socket, buffered);
    const frameData = buffered;
    let offset = 0;
    while (offset + 2 <= frameData.length) {
      const first = frameData[offset];
      const second = frameData[offset + 1];
      let length = second & 0x7f;
      let headerLength = 2;
      if (length === 126) {
        if (offset + 4 > frameData.length) return;
        length = frameData.readUInt16BE(offset + 2);
        headerLength = 4;
      } else if (length === 127 || !(second & 0x80)) {
        socket.destroy();
        return;
      }
      if (offset + headerLength + 4 + length > frameData.length) return;
      const mask = frameData.subarray(offset + headerLength, offset + headerLength + 4);
      const payload = frameData.subarray(offset + headerLength + 4, offset + headerLength + 4 + length);
      const decoded = Buffer.from(payload);
      for (let index = 0; index < decoded.length; ++index) decoded[index] ^= mask[index % 4];
      offset += headerLength + 4 + length;
      const opcode = first & 0x0f;
      if (opcode === 0x8) {
        socket.end();
        return;
      }
      if (opcode === 0x9) {
        this.sendFrame(socket, 0xA, decoded);
        continue;
      }
      if (opcode === 0x1) void this.handleCommand(socket, decoded.toString("utf8"));
    }
    this.buffers.set(socket, frameData.subarray(offset));
  }

  private async handleCommand(socket: Duplex, text: string): Promise<void> {
    try {
      const command = JSON.parse(text) as { action?: string; destination?: string; destinationPort?: number; volume?: number };
      if (command.action === "subscribe" && isSubscriptionRequest(command)) {
        if (command.destination !== undefined) this.status.config.destination = command.destination;
        if (command.destinationPort !== undefined) this.status.config.destinationPort = command.destinationPort;
        await this.runtime.subscribe(this.status.config);
        await this.discovery.publish(this.status.config);
        this.status.subscription = { active: true, destination: this.status.config.destination,
          destinationPort: this.status.config.destinationPort };
        this.status.stream = "running";
        this.status.health.streamRunning = true;
        this.broadcast(this.status);
      } else if (command.action === "unsubscribe") {
        await this.runtime.unsubscribe();
        await this.discovery.withdraw();
        this.status.subscription.active = false;
        this.status.stream = "stopped";
        this.status.health.streamRunning = false;
        this.broadcast(this.status);
      } else if (command.action === "setVolume" && isVolumeRequest(command)) {
        await this.runtime.setVolume(command.volume);
        this.status.volume = command.volume;
        this.broadcast(this.status);
      } else if (command.action === "status") {
        this.send(socket, { type: "status", data: snapshot(this.status) });
      } else {
        this.send(socket, { type: "error", error: "invalid WebSocket command" });
      }
    } catch (error: unknown) {
      this.send(socket, { type: "error", error: errorMessage(error) });
    }
  }

  private send(socket: Duplex, message: unknown): void {
    this.sendFrame(socket, 0x1, Buffer.from(JSON.stringify(message)));
  }

  private sendFrame(socket: Duplex, opcode: number, payload: Buffer): void {
    if (payload.length >= 126) {
      const frame = Buffer.alloc(4 + payload.length);
      frame[0] = 0x80 | opcode;
      frame[1] = 126;
      frame.writeUInt16BE(payload.length, 2);
      payload.copy(frame, 4);
      socket.write(frame);
      return;
    }
    const frame = Buffer.alloc(2 + payload.length);
    frame[0] = 0x80 | opcode;
    frame[1] = payload.length;
    payload.copy(frame, 2);
    socket.write(frame);
  }
}

if (process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const port = Number(process.env.CONTROL_PORT ?? 8080);
  const host = process.env.CONTROL_HOST ?? "127.0.0.1";
  createControlServer().listen(port, host, () => {
    console.log(`AES67 control plane listening on http://${host}:${port}`);
  });
}
