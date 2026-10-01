import { createHash, randomUUID } from "node:crypto";
import { spawn, type ChildProcess } from "node:child_process";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { connect as connectSocket, isIPv4 } from "node:net";
import { dirname, resolve } from "node:path";
import type { Duplex } from "node:stream";
import { fileURLToPath, pathToFileURL } from "node:url";
import { SapAnnouncer, SapRegistry, type DiscoveryPublisher, type DiscoveryRegistry } from "./discovery.js";

const webUiPath = resolve(fileURLToPath(new URL(".", import.meta.url)), "../../public/index.html");
const maxRequestBytes = 1 << 20;
const maxWebSocketPayload = 1 << 20;

export type NodeRole = "sender" | "receiver";

export interface NodeConfig {
  nodeId: string;
  channelCount: number;
  payloadType: number;
  destination: string;
  destinationPort: number;
  role?: NodeRole;
}

export interface NodeStatus {
  profile: "AES67-L24-48k-1ms";
  ptp: "locked" | "unlocked";
  stream: "stopped" | "running";
  role: NodeRole;
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
    discoveryAvailable: boolean;
  };
}

export interface EqBand {
  frequencyHz: number;
  gainDb: number;
  q: number;
}

export interface DspProfile {
  gainDb: number;
  delayMs: number;
  eq: EqBand[];
}

export interface SpeakerGroup {
  id: string;
  name: string;
  speakerIds: string[];
}

export interface RoutingState {
  groups: SpeakerGroup[];
  dsp: Record<string, DspProfile>;
  routes: Record<string, string[]>;
}

export const defaultConfig: NodeConfig = {
  nodeId: "speaker-node-01",
  channelCount: 2,
  payloadType: 96,
  destination: "239.69.0.1",
  destinationPort: 5004,
  role: "receiver",
};

interface PersistedState {
  config: NodeConfig;
  routing: RoutingState;
  volume: number;
}

class StateStore {
  public constructor(private readonly path: string | undefined) {}

  public load(initialConfig: NodeConfig): PersistedState {
    const fallback = { config: { ...initialConfig, role: initialConfig.role ?? "receiver" }, routing: { groups: [], dsp: {}, routes: {} }, volume: 1 };
    if (this.path === undefined) return fallback;
    try {
      const stored = JSON.parse(readFileSync(this.path, "utf8")) as Partial<PersistedState>;
      if (!isNodeConfig(stored.config) || !isRoutingState(stored.routing) ||
          typeof stored.volume !== "number" || !Number.isFinite(stored.volume) || stored.volume < 0 || stored.volume > 1) {
        throw new Error("invalid persisted control state");
      }
      return { config: stored.config, routing: stored.routing, volume: stored.volume };
    } catch (error: unknown) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") {
        console.error(`could not load control state: ${errorMessage(error)}`);
      }
      return fallback;
    }
  }

  public save(status: NodeStatus, routing: RoutingState): void {
    if (this.path === undefined) return;
    const temporaryPath = `${this.path}.${process.pid}.tmp`;
    try {
      mkdirSync(dirname(this.path), { recursive: true });
      writeFileSync(temporaryPath, JSON.stringify({ config: status.config, routing, volume: status.volume }) + "\n");
      renameSync(temporaryPath, this.path);
    } catch (error: unknown) {
      console.error(`could not persist control state: ${errorMessage(error)}`);
    }
  }
}

class MutationQueue {
  private tail = Promise.resolve();

  public run<T>(operation: () => Promise<T>): Promise<T> {
    const result = this.tail.then(operation, operation);
    this.tail = result.then(() => undefined, () => undefined);
    return result;
  }
}

export interface SpeakerRuntime {
  subscribe(config: NodeConfig): Promise<void>;
  unsubscribe(): Promise<void>;
  setVolume(volume: number): Promise<void>;
  startSender?(config: NodeConfig): Promise<void>;
  stopSender?(): Promise<void>;
  setRole?(role: NodeRole, config: NodeConfig): Promise<void>;
  getRole?(): NodeRole;
  isRunning(): boolean;
  onStateChange?(listener: () => void): () => void;
}

export class ProcessSpeakerRuntime implements SpeakerRuntime {
  private child: ChildProcess | undefined;
  private socketPath: string | undefined;
  private role: NodeRole = "receiver";
  private readonly stateListeners = new Set<() => void>();

  public async subscribe(config: NodeConfig): Promise<void> {
    this.role = config.role ?? "receiver";
    await this.stopChild();
    const executable = process.env.AES67_SPEAKER_EXECUTABLE ?? resolve(process.cwd(), "../build/aes67_speaker_node");
    const socketName = config.nodeId.replace(/[^a-zA-Z0-9_.-]/g, "_");
    this.socketPath = process.env.AES67_CONTROL_SOCKET ?? `/tmp/aes67-${socketName}.sock`;
    const args = isMulticast(config.destination)
      ? ["--multicast", config.destination]
      : ["--bind", "0.0.0.0"];
    args.push("--port", String(config.destinationPort), "--payload-type", String(config.payloadType),
      "--device", process.env.AES67_ALSA_DEVICE ?? "default", "--control-socket", this.socketPath);
    const child = spawn(executable, args, { stdio: "ignore" });
    this.child = child;
    child.once("exit", () => {
      if (this.child === child) this.child = undefined;
      console.error(`speaker process exited: code=${child.exitCode ?? "none"} signal=${child.signalCode ?? "none"}`);
      this.notifyStateChange();
    });
    await new Promise<void>((resolvePromise, reject) => {
      const onError = (error: Error) => {
        if (this.child === child) this.child = undefined;
        this.notifyStateChange();
        reject(error);
      };
      child.once("error", onError);
      child.once("spawn", () => {
        child.off("error", onError);
        resolvePromise();
      });
    });
  }

  public async startSender(config: NodeConfig): Promise<void> {
    this.role = "sender";
    await this.stopChild();
    const executable = process.env.AES67_SENDER_EXECUTABLE ?? resolve(process.cwd(), "../build/aes67_sender_node");
    const args = ["--device", process.env.AES67_ALSA_DEVICE ?? "default",
      "--destination", config.destination, "--port", String(config.destinationPort)];
    if (process.env.AES67_STREAM_SDP) args.push("--sdp", process.env.AES67_STREAM_SDP);
    const child = spawn(executable, args, { stdio: "ignore" });
    this.child = child;
    child.once("exit", () => {
      if (this.child === child) this.child = undefined;
      console.error(`sender process exited: code=${child.exitCode ?? "none"} signal=${child.signalCode ?? "none"}`);
      this.notifyStateChange();
    });
    await new Promise<void>((resolvePromise, reject) => {
      const onError = (error: Error) => {
        if (this.child === child) this.child = undefined;
        this.notifyStateChange();
        reject(error);
      };
      child.once("error", onError);
      child.once("spawn", () => {
        child.off("error", onError);
        resolvePromise();
      });
    });
  }

  public async stopSender(): Promise<void> {
    await this.stopChild();
  }

  public async setRole(role: NodeRole, config: NodeConfig): Promise<void> {
    this.role = role;
    if (role === "sender") {
      await this.startSender(config);
      return;
    }
    await this.subscribe(config);
  }

  public getRole(): NodeRole {
    return this.role;
  }

  public async unsubscribe(): Promise<void> {
    await this.stopChild();
  }

  private async stopChild(): Promise<void> {
    const child = this.child;
    if (child === undefined) return;
    this.child = undefined;
    this.notifyStateChange();
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

  public onStateChange(listener: () => void): () => void {
    this.stateListeners.add(listener);
    return () => this.stateListeners.delete(listener);
  }

  private notifyStateChange(): void {
    for (const listener of this.stateListeners) listener();
  }
}

export function createControlServer(initialConfig: NodeConfig = defaultConfig,
                                    runtime: SpeakerRuntime = new ProcessSpeakerRuntime(),
                                    discovery: DiscoveryPublisher = new SapAnnouncer(),
                                    registry: DiscoveryRegistry = new SapRegistry()): Server {
  const stateStore = new StateStore(process.env.AES67_STATE_FILE);
  const persisted = stateStore.load(initialConfig);
  const role = persisted.config.role ?? "receiver";
  const status: NodeStatus = {
    profile: "AES67-L24-48k-1ms",
    ptp: "unlocked",
    stream: "stopped",
    role,
    config: { ...persisted.config, role },
    subscription: {
      active: false,
      destination: persisted.config.destination,
      destinationPort: persisted.config.destinationPort,
    },
    volume: persisted.volume,
    health: { ok: true, ptpLocked: false, streamRunning: false, discoveryAvailable: false },
  };
  const mutations = new MutationQueue();
  const routing: RoutingState = persisted.routing;
  const persist = () => stateStore.save(status, routing);
  const websocket = new WebSocketHub(status, runtime, discovery, mutations, persist);
  const refreshHealth = () => {
    const streamRunning = status.subscription.active && runtime.isRunning();
    status.health.streamRunning = streamRunning;
    status.stream = streamRunning ? "running" : "stopped";
    status.health.ok = (!status.subscription.active || streamRunning) && status.health.discoveryAvailable;
    websocket.broadcast(status);
  };
  const removeRuntimeListener = runtime.onStateChange?.(refreshHealth);
  const server = createServer((request, response) =>
    handleRequest(request, response, status, websocket, runtime, discovery, registry, routing, mutations, persist));
  server.on("upgrade", (request, socket) => websocket.accept(request, socket));
  server.on("close", () => {
    removeRuntimeListener?.();
    websocket.close();
    discovery.close();
    registry.close();
  });
  void registry.start().then(() => {
    status.health.discoveryAvailable = true;
    status.health.ok = (!status.subscription.active || runtime.isRunning());
    websocket.broadcast(status);
  }).catch((error: unknown) => {
    status.health.discoveryAvailable = false;
    status.health.ok = false;
    console.error(`discovery registry failed: ${errorMessage(error)}`);
    websocket.broadcast(status);
  });
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

async function activateSubscription(config: NodeConfig, status: NodeStatus,
                                    runtime: SpeakerRuntime, discovery: DiscoveryPublisher): Promise<void> {
  const previousConfig = { ...status.config };
  const previousRole = status.role;
  const wasActive = status.subscription.active;
  const targetRole = config.role ?? previousRole;
  try {
    if (runtime.setRole !== undefined) {
      await runtime.setRole(targetRole, config);
    } else if (targetRole === "sender") {
      if (runtime.startSender !== undefined) {
        await runtime.startSender(config);
      } else {
        await runtime.subscribe(config);
      }
    } else {
      await runtime.subscribe(config);
    }
    await discovery.publish(config);
  } catch (error: unknown) {
    try {
      if (wasActive) {
        if (runtime.setRole !== undefined) {
          await runtime.setRole(previousRole, previousConfig);
        } else if (previousRole === "sender") {
          if (runtime.startSender !== undefined) {
            await runtime.startSender(previousConfig);
          } else {
            await runtime.subscribe(previousConfig);
          }
        } else {
          await runtime.subscribe(previousConfig);
        }
        await discovery.publish(previousConfig);
      } else {
        await runtime.unsubscribe();
        if (runtime.stopSender !== undefined) await runtime.stopSender();
        await discovery.withdraw();
      }
    } catch {
      // Preserve the original operation failure; health reconciliation reports the runtime state.
    }
    throw error;
  }
  status.role = targetRole;
  status.config = { ...config, role: targetRole };
  status.subscription = {
    active: true,
    destination: config.destination,
    destinationPort: config.destinationPort,
  };
  status.stream = "running";
  status.health.streamRunning = true;
  status.health.ok = status.health.discoveryAvailable;
}

async function deactivateSubscription(status: NodeStatus, runtime: SpeakerRuntime,
                                      discovery: DiscoveryPublisher): Promise<void> {
  const previousConfig = { ...status.config };
  await discovery.withdraw();
  try {
    if (status.role === "sender" && runtime.stopSender !== undefined) {
      await runtime.stopSender();
    } else {
      await runtime.unsubscribe();
    }
  } catch (error: unknown) {
    try {
      if (status.subscription.active) await discovery.publish(previousConfig);
    } catch {
      // Preserve the original operation failure; health reconciliation reports the runtime state.
    }
    throw error;
  }
  status.subscription.active = false;
  status.stream = "stopped";
  status.health.streamRunning = false;
  status.health.ok = status.health.discoveryAvailable;
}

async function setNodeMode(mode: NodeRole, status: NodeStatus,
                          runtime: SpeakerRuntime, discovery: DiscoveryPublisher): Promise<void> {
  const proposed = { ...status.config, role: mode };
  status.role = mode;
  status.config = proposed;
  if (status.subscription.active) {
    await activateSubscription(proposed, status, runtime, discovery);
    return;
  }
  if (runtime.setRole !== undefined) {
    await runtime.setRole(mode, proposed);
  } else if (mode === "sender") {
    if (runtime.startSender !== undefined) {
      await runtime.startSender(proposed);
    } else {
      await runtime.subscribe(proposed);
    }
  } else {
    await runtime.subscribe(proposed);
  }
  status.subscription = { active: true, destination: proposed.destination, destinationPort: proposed.destinationPort };
  status.stream = "running";
  status.health.streamRunning = true;
  status.health.ok = status.health.discoveryAvailable;
}

function handleRequest(request: IncomingMessage, response: ServerResponse, status: NodeStatus,
                       websocket: WebSocketHub, runtime: SpeakerRuntime,
                       discovery: DiscoveryPublisher, registry: DiscoveryRegistry,
                       routing: RoutingState, mutations: MutationQueue, persist: () => void): void {
  const path = request.url?.split("?", 1)[0] ?? "/";
  if (request.method === "GET" && path === "/") {
    response.statusCode = 200;
    response.setHeader("content-type", "text/html; charset=utf-8");
    response.end(readFileSync(webUiPath, "utf8"));
    return;
  }
  response.setHeader("content-type", "application/json");
  if (request.method === "GET" && path === "/health") {
    send(response, 200, status.health);
    return;
  }
  if (request.method === "GET" && path === "/v1/status") {
    send(response, 200, snapshot(status));
    return;
  }
  if (request.method === "GET" && path === "/v1/discovery/streams") {
    send(response, 200, { streams: registry.list() });
    return;
  }
  if (request.method === "GET" && path === "/v1/routing") {
    send(response, 200, { ...routing, streams: registry.list(), localSpeakerId: status.config.nodeId });
    return;
  }
  if (request.method === "PUT" && path === "/v1/config") {
    collectJson(request, response, (body) => {
      if (!isNodeConfig(body)) {
        send(response, 400, { error: "invalid node configuration" });
        return;
      }
      void mutations.run(async () => {
        const proposed = { ...body, role: body.role ?? status.role };
        if (status.subscription.active) {
          await activateSubscription(proposed, status, runtime, discovery);
        }
        status.config = proposed;
        status.role = proposed.role ?? status.role;
        send(response, 200, { ...status.config, role: status.role });
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    });
    return;
  }
  if ((request.method === "PUT" || request.method === "POST") && (path === "/v1/mode" || path === "/v1/role")) {
    collectJson(request, response, (body) => {
      if (!isNodeRole((body as Record<string, unknown>)?.role)) {
        send(response, 400, { error: "role must be sender or receiver" });
        return;
      }
      void mutations.run(async () => {
        const mode = (body as { role: NodeRole }).role;
        await setNodeMode(mode, status, runtime, discovery);
        status.config = { ...status.config, role: mode };
        send(response, 200, { role: status.role, config: status.config });
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    });
    return;
  }
  if (request.method === "POST" && path === "/v1/subscribe") {
    collectJson(request, response, (body) => {
      if (body !== undefined && !isSubscriptionRequest(body)) {
        send(response, 400, { error: "invalid subscription" });
        return;
      }
      const subscription = body as Partial<NodeConfig> | undefined;
      const proposed = { ...status.config,
        ...(subscription?.destination === undefined ? {} : { destination: subscription.destination }),
        ...(subscription?.destinationPort === undefined ? {} : { destinationPort: subscription.destinationPort }),
      };
      void mutations.run(async () => {
        await activateSubscription(proposed, status, runtime, discovery);
        status.config = proposed;
        send(response, 200, snapshot(status));
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    });
    return;
  }
  if (request.method === "POST" && path === "/v1/routes") {
    collectJson(request, response, (body) => {
      const streamId = isRouteRequest(body) ? body.streamId : undefined;
      const stream = streamId === undefined ? undefined : registry.get(streamId);
      if (stream === undefined) {
        send(response, 404, { error: "discovered stream not found" });
        return;
      }
      const routedConfig = { ...status.config, destination: stream.destination,
        destinationPort: stream.destinationPort, channelCount: stream.channelCount,
        payloadType: stream.payloadType };
      void mutations.run(async () => {
        await activateSubscription(routedConfig, status, runtime, discovery);
        status.config = routedConfig;
        send(response, 200, snapshot(status));
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    });
    return;
  }
  if (request.method === "PUT" && path === "/v1/routes") {
    collectJson(request, response, (body) => {
      if (!isRouteMatrix(body)) {
        send(response, 400, { error: "routes must map a streamId to speakerIds" });
        return;
      }
      void mutations.run(async () => {
        const localSpeakerId = status.config.nodeId;
        const stream = registry.get(body.streamId);
        const localSelected = body.speakerIds.includes(localSpeakerId);
        if (localSelected && stream !== undefined) {
          const routedConfig = { ...status.config, destination: stream.destination,
            destinationPort: stream.destinationPort, channelCount: stream.channelCount,
            payloadType: stream.payloadType };
          await activateSubscription(routedConfig, status, runtime, discovery);
          status.config = routedConfig;
        } else {
          await deactivateSubscription(status, runtime, discovery);
        }
        routing.routes[body.streamId] = body.speakerIds;
        status.subscription.active = localSelected;
        send(response, 200, { ...routing, streams: registry.list(), localSpeakerId });
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    });
    return;
  }
  if (request.method === "PUT" && path.match(/^\/v1\/groups\/[^/]+$/)) {
    const id = path.split("/").pop()!;
    collectJson(request, response, (body) => {
      if (!isSpeakerGroup(body) || body.id !== id) {
        send(response, 400, { error: "invalid speaker group" });
        return;
      }
      void mutations.run(async () => {
        routing.groups = [...routing.groups.filter((group) => group.id !== id), body];
        send(response, 200, body);
        websocket.broadcast(status);
      });
    });
    return;
  }
  if (request.method === "DELETE" && path.match(/^\/v1\/groups\/[^/]+$/)) {
    const id = path.split("/").pop()!;
    void mutations.run(async () => {
      routing.groups = routing.groups.filter((group) => group.id !== id);
      send(response, 200, { groups: routing.groups });
      websocket.broadcast(status);
    });
    return;
  }
  if (request.method === "PUT" && path.match(/^\/v1\/speakers\/[^/]+\/dsp$/)) {
    const speakerId = path.split("/")[3];
    collectJson(request, response, (body) => {
      if (!isDspProfile(body)) {
        send(response, 400, { error: "invalid DSP profile" });
        return;
      }
      if (body.gainDb > 0 || body.delayMs !== 0 || body.eq.length !== 0) {
        send(response, 422, { error: "only gain attenuation is supported by the audio runtime" });
        return;
      }
      void mutations.run(async () => {
        if (speakerId === status.config.nodeId) {
          await runtime.setVolume(Math.pow(10, body.gainDb / 20));
          status.volume = Math.pow(10, body.gainDb / 20);
        }
          routing.dsp[speakerId] = body;
          send(response, 200, body);
          websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 409, { error: errorMessage(error) }));
    });
    return;
  }
  if (request.method === "POST" && path === "/v1/unsubscribe") {
    void mutations.run(async () => {
      await deactivateSubscription(status, runtime, discovery);
      send(response, 200, snapshot(status));
      websocket.broadcast(status);
    }).catch((error: unknown) => send(response, 502, { error: errorMessage(error) }));
    return;
  }
  if (request.method === "GET" && path === "/v1/volume") {
    send(response, 200, { volume: status.volume });
    return;
  }
  if (request.method === "PUT" && path === "/v1/volume") {
    collectJson(request, response, (body) => {
      if (!isVolumeRequest(body)) {
        send(response, 400, { error: "volume must be a number from 0 to 1" });
        return;
      }
      void mutations.run(async () => {
        await runtime.setVolume(body.volume);
        status.volume = body.volume;
        send(response, 200, { volume: status.volume });
        websocket.broadcast(status);
      }).catch((error: unknown) => send(response, 409, { error: errorMessage(error) }));
    });
    return;
  }
  send(response, 404, { error: "not found" });
}

function collectJson(request: IncomingMessage, response: ServerResponse,
                    callback: (body: unknown) => void): void {
  let content = "";
  let size = 0;
  let tooLarge = false;
  request.setEncoding("utf8");
  request.on("data", (chunk: string) => {
    size += Buffer.byteLength(chunk);
    if (size > maxRequestBytes) {
      tooLarge = true;
      return;
    }
    content += chunk;
  });
  request.on("end", () => {
    if (tooLarge) {
      send(response, 413, { error: "request body is too large" });
      return;
    }
    try { callback(JSON.parse(content)); } catch { callback(undefined); }
  });
}

function isNodeRole(value: unknown): value is NodeRole {
  return value === "sender" || value === "receiver";
}

function isNodeConfig(value: unknown): value is NodeConfig {
  if (typeof value !== "object" || value === null) return false;
  const config = value as Record<string, unknown>;
  return isNodeId(config.nodeId) && typeof config.channelCount === "number" &&
    Number.isInteger(config.channelCount) && config.channelCount === 2 &&
    typeof config.payloadType === "number" && Number.isInteger(config.payloadType) &&
    config.payloadType >= 96 && config.payloadType <= 127 &&
    isDestination(config.destination) && typeof config.destinationPort === "number" &&
    Number.isInteger(config.destinationPort) && config.destinationPort > 0 && config.destinationPort < 65536 &&
    (config.role === undefined || isNodeRole(config.role));
}

function isRoutingState(value: unknown): value is RoutingState {
  if (typeof value !== "object" || value === null) return false;
  const routing = value as Record<string, unknown>;
  return Array.isArray(routing.groups) && typeof routing.dsp === "object" && routing.dsp !== null &&
    typeof routing.routes === "object" && routing.routes !== null;
}

function isSubscriptionRequest(value: unknown): value is Partial<NodeConfig> {
  if (typeof value !== "object" || value === null) return false;
  const request = value as Record<string, unknown>;
  return (request.destination === undefined || isDestination(request.destination)) &&
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

function isRouteMatrix(value: unknown): value is { streamId: string; speakerIds: string[] } {
  if (!isRouteRequest(value)) return false;
  const speakerIds = (value as Record<string, unknown>).speakerIds;
  return Array.isArray(speakerIds) && speakerIds.length > 0 &&
    speakerIds.every((speaker: unknown) => isNodeId(speaker)) && new Set(speakerIds).size === speakerIds.length;
}

function isSpeakerGroup(value: unknown): value is SpeakerGroup {
  if (typeof value !== "object" || value === null) return false;
  const group = value as Record<string, unknown>;
  return typeof group.id === "string" && group.id.length > 0 && typeof group.name === "string" &&
    Array.isArray(group.speakerIds) && group.speakerIds.every((speaker) => isNodeId(speaker)) &&
    new Set(group.speakerIds).size === group.speakerIds.length;
}

function isNodeId(value: unknown): value is string {
  return typeof value === "string" && /^[a-zA-Z0-9_.-]{1,64}$/.test(value);
}

function isDestination(value: unknown): value is string {
  return typeof value === "string" && isIPv4(value) && value !== "0.0.0.0";
}

function isDspProfile(value: unknown): value is DspProfile {
  if (typeof value !== "object" || value === null) return false;
  const dsp = value as Record<string, unknown>;
  return typeof dsp.gainDb === "number" && Number.isFinite(dsp.gainDb) && dsp.gainDb >= -60 && dsp.gainDb <= 12 &&
    typeof dsp.delayMs === "number" && Number.isFinite(dsp.delayMs) && dsp.delayMs >= 0 && dsp.delayMs <= 500 &&
    Array.isArray(dsp.eq) && dsp.eq.every((band) => {
      if (typeof band !== "object" || band === null) return false;
      const eq = band as Record<string, unknown>;
      return typeof eq.frequencyHz === "number" && eq.frequencyHz > 0 && eq.frequencyHz <= 24000 &&
        typeof eq.gainDb === "number" && eq.gainDb >= -24 && eq.gainDb <= 24 &&
        typeof eq.q === "number" && eq.q > 0 && eq.q <= 20;
    });
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
  if (typeof body === "object" && body !== null && "error" in body) {
    const operationId = randomUUID();
    console.error(`[${operationId}] ${JSON.stringify(body)}`);
    body = { ...body, operationId };
  }
  response.statusCode = statusCode;
  response.end(JSON.stringify(body));
}

class WebSocketHub {
  private readonly clients = new Set<Duplex>();
  private readonly buffers = new Map<Duplex, Buffer>();

  public constructor(private readonly status: NodeStatus, private readonly runtime: SpeakerRuntime,
                     private readonly discovery: DiscoveryPublisher,
                     private readonly mutations: MutationQueue,
                     private readonly persist: () => void) {}

  public accept(request: IncomingMessage, socket: Duplex): void {
    if (request.url !== "/v1/events" || request.headers.upgrade?.toLowerCase() !== "websocket" ||
        request.headers["sec-websocket-version"] !== "13") {
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
    this.persist();
    for (const client of this.clients) this.send(client, { type: "status", data: snapshot(status) });
  }

  public close(): void {
    for (const client of this.clients) client.destroy();
    this.clients.clear();
    this.buffers.clear();
  }

  private receive(socket: Duplex, data: Buffer): void {
    const buffered = Buffer.concat([this.buffers.get(socket) ?? Buffer.alloc(0), data]);
    if (buffered.length > maxWebSocketPayload + 14) {
      socket.destroy();
      return;
    }
    this.buffers.set(socket, buffered);
    const frameData = buffered;
    let offset = 0;
    while (offset + 2 <= frameData.length) {
      const first = frameData[offset];
      const second = frameData[offset + 1];
      const fin = (first & 0x80) !== 0;
      const opcode = first & 0x0f;
      const isControl = opcode >= 0x8;
      if ((first & 0x70) !== 0 || opcode === 0x0 || (opcode !== 0x1 && opcode !== 0x8 && opcode !== 0x9 && opcode !== 0xa) ||
          (!fin && (isControl || opcode === 0x1)) || (isControl && (second & 0x7f) > 125) || !(second & 0x80)) {
        socket.destroy();
        return;
      }
      let length = second & 0x7f;
      let headerLength = 2;
      if (length === 126) {
        if (offset + 4 > frameData.length) return;
        length = frameData.readUInt16BE(offset + 2);
        headerLength = 4;
      } else if (length === 127) {
        if (offset + 10 > frameData.length) return;
        const extendedLength = frameData.readBigUInt64BE(offset + 2);
        if (extendedLength > BigInt(maxWebSocketPayload)) {
          socket.destroy();
          return;
        }
        length = Number(extendedLength);
        headerLength = 10;
      }
      if (length > maxWebSocketPayload || (opcode === 0x8 && length === 1)) {
        socket.destroy();
        return;
      }
      if (offset + headerLength + 4 + length > frameData.length) return;
      const mask = frameData.subarray(offset + headerLength, offset + headerLength + 4);
      const payload = frameData.subarray(offset + headerLength + 4, offset + headerLength + 4 + length);
      const decoded = Buffer.from(payload);
      for (let index = 0; index < decoded.length; ++index) decoded[index] ^= mask[index % 4];
      offset += headerLength + 4 + length;
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
      const command = JSON.parse(text) as Record<string, unknown>;
      const action = typeof command.action === "string" ? command.action : undefined;
      const destination = typeof command.destination === "string" ? command.destination : undefined;
      const destinationPort = typeof command.destinationPort === "number" ? command.destinationPort : undefined;
      const volume = typeof command.volume === "number" ? command.volume : undefined;
      const roleValue = command.role;
      const role = isNodeRole(roleValue) ? roleValue : undefined;
      if (action === "status") {
        this.send(socket, { type: "status", data: snapshot(this.status) });
        return;
      }
      if (action === "subscribe" && !isSubscriptionRequest(command)) {
        this.send(socket, { type: "error", error: "invalid subscription" });
        return;
      }
      if (action === "setVolume" && !isVolumeRequest(command)) {
        this.send(socket, { type: "error", error: "volume must be a number from 0 to 1" });
        return;
      }
      if ((action === "setMode" || action === "setRole") && role === undefined) {
        this.send(socket, { type: "error", error: "role must be sender or receiver" });
        return;
      }
      await this.mutations.run(async () => {
        if (action === "subscribe") {
          const proposed = { ...this.status.config,
            ...(destination === undefined ? {} : { destination }),
            ...(destinationPort === undefined ? {} : { destinationPort }),
          };
          await activateSubscription(proposed, this.status, this.runtime, this.discovery);
          this.status.config = proposed;
          this.broadcast(this.status);
        } else if (action === "unsubscribe") {
          await deactivateSubscription(this.status, this.runtime, this.discovery);
          this.broadcast(this.status);
        } else if (action === "setMode" || action === "setRole") {
          await setNodeMode(role!, this.status, this.runtime, this.discovery);
          this.status.config = { ...this.status.config, role: role! };
          this.broadcast(this.status);
        } else if (action === "setVolume") {
          await this.runtime.setVolume(volume!);
          this.status.volume = volume!;
          this.broadcast(this.status);
        } else {
          this.send(socket, { type: "error", error: "invalid WebSocket command" });
        }
      });
    } catch (error: unknown) {
      this.send(socket, { type: "error", error: errorMessage(error) });
    }
  }

  private send(socket: Duplex, message: unknown): void {
    this.sendFrame(socket, 0x1, Buffer.from(JSON.stringify(message)));
  }

  private sendFrame(socket: Duplex, opcode: number, payload: Buffer): void {
    if (payload.length > maxWebSocketPayload) {
      socket.destroy();
      return;
    }
    if (payload.length >= 126) {
      const headerLength = payload.length <= 0xffff ? 4 : 10;
      const frame = Buffer.alloc(headerLength + payload.length);
      frame[0] = 0x80 | opcode;
      if (headerLength === 4) {
        frame[1] = 126;
        frame.writeUInt16BE(payload.length, 2);
      } else {
        frame[1] = 127;
        frame.writeBigUInt64BE(BigInt(payload.length), 2);
      }
      payload.copy(frame, headerLength);
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
