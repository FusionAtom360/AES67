import assert from "node:assert/strict";
import { once } from "node:events";
import { connect } from "node:net";
import { request } from "node:http";
import test from "node:test";
import { createControlServer, type NodeConfig, type SpeakerRuntime } from "../src/index.js";
import type { DiscoveredStream, DiscoveryPublisher, DiscoveryRegistry } from "../src/discovery.js";

class FakeRuntime implements SpeakerRuntime {
  public running = false;
  public volume = 1;
  public lastConfig: NodeConfig | undefined;
  public role: "sender" | "receiver" = "receiver";
  private readonly listeners = new Set<() => void>();

  public async subscribe(config: NodeConfig): Promise<void> { this.running = true; this.role = config.role ?? "receiver"; this.lastConfig = config; this.notify(); }
  public async unsubscribe(): Promise<void> { this.running = false; this.notify(); }
  public async setVolume(volume: number): Promise<void> { this.volume = volume; }
  public async startSender(config: NodeConfig): Promise<void> { this.running = true; this.role = "sender"; this.lastConfig = config; this.notify(); }
  public async stopSender(): Promise<void> { this.running = false; this.notify(); }
  public async setRole(role: "sender" | "receiver", config: NodeConfig): Promise<void> {
    this.role = role; this.running = true; this.lastConfig = config; this.notify();
  }
  public getRole(): "sender" | "receiver" { return this.role; }
  public isRunning(): boolean { return this.running; }
  public onStateChange(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
  public exit(): void { this.running = false; this.notify(); }
  private notify(): void { for (const listener of this.listeners) listener(); }
}

class FakeDiscovery implements DiscoveryPublisher {
  public active = false;
  public async publish(_config: NodeConfig): Promise<void> { this.active = true; }
  public async withdraw(): Promise<void> { this.active = false; }
  public close(): void { this.active = false; }
}

class FakeRegistry implements DiscoveryRegistry {
  private readonly stream: DiscoveredStream = {
    id: "sender-1",
    sourceAddress: "192.0.2.10",
    sessionName: "AES67 stream",
    destination: "239.69.0.1",
    destinationPort: 5004,
    channelCount: 2,
    payloadType: 96,
    lastSeen: new Date().toISOString(),
  };
  public async start(): Promise<void> {}
  public close(): void {}
  public list(): DiscoveredStream[] { return [this.stream]; }
  public get(id: string): DiscoveredStream | undefined { return id === this.stream.id ? this.stream : undefined; }
}

test("role-aware control can switch a node between sender and receiver modes", async () => {
  const runtime = new FakeRuntime();
  const discovery = new FakeDiscovery();
  const registry = new FakeRegistry();
  const server = createControlServer({
    nodeId: "node-1",
    channelCount: 2,
    payloadType: 96,
    destination: "239.69.0.1",
    destinationPort: 5004,
    role: "receiver",
  }, runtime, discovery, registry);
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const address = server.address();
  assert.ok(address && typeof address !== "string");

  const modeSet = await call(address.port, "PUT", "/v1/mode", { role: "sender" });
  assert.equal(modeSet.statusCode, 200);
  assert.equal((modeSet.body as { role: string }).role, "sender");
  const status = await call(address.port, "GET", "/v1/status");
  assert.equal((status.body as { role: string }).role, "sender");

  const configSet = await call(address.port, "PUT", "/v1/config", {
    nodeId: "node-1",
    channelCount: 2,
    payloadType: 96,
    destination: "239.69.0.2",
    destinationPort: 5005,
    role: "receiver",
  });
  assert.equal(configSet.statusCode, 200);
  assert.equal((configSet.body as { role: string }).role, "receiver");

  await closeServer(server);
});

test("control plane reports the pinned profile and accepts configuration", async () => {
  const runtime = new FakeRuntime();
  const discovery = new FakeDiscovery();
  const registry = new FakeRegistry();
  const server = createControlServer(undefined, runtime, discovery, registry);
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const address = server.address();
  assert.ok(address && typeof address !== "string");

  const status = await call(address.port, "GET", "/v1/status");
  assert.equal(status.statusCode, 200);
  assert.equal((status.body as { profile: string }).profile, "AES67-L24-48k-1ms");
  const page = await callText(address.port, "GET", "/");
  assert.equal(page.statusCode, 200);
  assert.match(page.body, /AES67 Control/);

  const updated = await call(address.port, "PUT", "/v1/config", {
    nodeId: "stage-left",
    channelCount: 2,
    payloadType: 97,
    destination: "239.69.0.2",
    destinationPort: 5004,
  });
  assert.equal(updated.statusCode, 200);
  assert.equal((updated.body as { nodeId: string }).nodeId, "stage-left");

  const health = await call(address.port, "GET", "/health");
  assert.equal(health.statusCode, 200);
  assert.equal((health.body as { ok: boolean }).ok, true);
  assert.equal((health.body as { discoveryAvailable: boolean }).discoveryAvailable, true);
  const discovered = await call(address.port, "GET", "/v1/discovery/streams");
  assert.equal((discovered.body as { streams: Array<{ id: string }> }).streams[0]?.id, "sender-1");
  const routing = await call(address.port, "GET", "/v1/routing");
  assert.equal((routing.body as { localSpeakerId: string }).localSpeakerId, "stage-left");

  const subscribed = await call(address.port, "POST", "/v1/subscribe", {
    destination: "239.69.0.2",
    destinationPort: 5006,
  });
  assert.equal(subscribed.statusCode, 200);
  assert.equal((subscribed.body as { subscription: { active: boolean } }).subscription.active, true);
  assert.equal((subscribed.body as { stream: string }).stream, "running");
  assert.equal(discovery.active, true);

  const liveConfig = await call(address.port, "PUT", "/v1/config", {
    nodeId: "stage-left",
    channelCount: 2,
    payloadType: 98,
    destination: "239.69.0.3",
    destinationPort: 5007,
  });
  assert.equal(liveConfig.statusCode, 200);
  assert.equal(runtime.lastConfig?.destination, "239.69.0.3");
  assert.equal(runtime.lastConfig?.payloadType, 98);

  runtime.exit();
  const failedHealth = await call(address.port, "GET", "/health");
  assert.equal((failedHealth.body as { ok: boolean }).ok, false);
  assert.equal((failedHealth.body as { streamRunning: boolean }).streamRunning, false);
  const failedStatus = await call(address.port, "GET", "/v1/status");
  assert.equal((failedStatus.body as { stream: string }).stream, "stopped");

  const volume = await call(address.port, "PUT", "/v1/volume", { volume: 0.65 });
  assert.equal(volume.statusCode, 200);
  assert.equal((volume.body as { volume: number }).volume, 0.65);

  const unsubscribed = await call(address.port, "POST", "/v1/unsubscribe");
  assert.equal(unsubscribed.statusCode, 200);
  assert.equal((unsubscribed.body as { subscription: { active: boolean } }).subscription.active, false);
  assert.equal(discovery.active, false);

  const routed = await call(address.port, "POST", "/v1/routes", { streamId: "sender-1" });
  assert.equal(routed.statusCode, 200);
  assert.equal(runtime.lastConfig?.destination, "239.69.0.1");

  const group = await call(address.port, "PUT", "/v1/groups/front", {
    id: "front", name: "Front wall", speakerIds: ["stage-left"],
  });
  assert.equal(group.statusCode, 200);
  const unsupportedDsp = await call(address.port, "PUT", "/v1/speakers/stage-left/dsp", {
    gainDb: -3, delayMs: 2.5, eq: [{ frequencyHz: 1000, gainDb: 2, q: 1 }],
  });
  assert.equal(unsupportedDsp.statusCode, 422);
  const dsp = await call(address.port, "PUT", "/v1/speakers/stage-left/dsp", {
    gainDb: -3, delayMs: 0, eq: [],
  });
  assert.equal(dsp.statusCode, 200);

  const websocket = await openWebSocket(address.port);
  assert.match(websocket, /HTTP\/1\.1 101 Switching Protocols/);
  assert.match(websocket, /"type":"status"/);
  await closeServer(server);
});

function closeServer(server: ReturnType<typeof createControlServer>): Promise<void> {
  return new Promise((resolvePromise, reject) => {
    server.close((error) => error === undefined ? resolvePromise() : reject(error));
  });
}

async function openWebSocket(port: number): Promise<string> {
  const client = connect(port, "127.0.0.1");
  await once(client, "connect");
  client.write("GET /v1/events HTTP/1.1\r\n" +
    "Host: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGVzdC1rZXk=\r\n\r\n");
  const [data] = await once(client, "data") as [Buffer];
  client.write(Buffer.from([0x88, 0x80, 0x01, 0x02, 0x03, 0x04]));
  await once(client, "close");
  return data.toString("utf8");
}

function call(port: number, method: string, path: string, body?: unknown): Promise<{ statusCode?: number; body: any }> {
  return new Promise((resolve, reject) => {
    const requestBody = body === undefined ? undefined : JSON.stringify(body);
    const clientRequest = request({ hostname: "127.0.0.1", port, method, path,
      headers: requestBody === undefined ? {} : { "content-type": "application/json", "content-length": Buffer.byteLength(requestBody) } },
      (response) => {
        let content = "";
        response.setEncoding("utf8");
        response.on("data", (chunk) => { content += chunk; });
        response.on("end", () => resolve({ statusCode: response.statusCode, body: JSON.parse(content) }));
      });
    clientRequest.on("error", reject);
    if (requestBody !== undefined) clientRequest.end(requestBody); else clientRequest.end();
  });
}

function callText(port: number, method: string, path: string): Promise<{ statusCode?: number; body: string }> {
  return new Promise((resolve, reject) => {
    const clientRequest = request({ hostname: "127.0.0.1", port, method, path }, (response) => {
      let content = "";
      response.setEncoding("utf8");
      response.on("data", (chunk) => { content += chunk; });
      response.on("end", () => resolve({ statusCode: response.statusCode, body: content }));
    });
    clientRequest.on("error", reject);
    clientRequest.end();
  });
}
