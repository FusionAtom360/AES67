import assert from "node:assert/strict";
import { once } from "node:events";
import { connect } from "node:net";
import { request } from "node:http";
import test from "node:test";
import { createControlServer, type NodeConfig, type SpeakerRuntime } from "../src/index.js";

class FakeRuntime implements SpeakerRuntime {
  public running = false;
  public volume = 1;

  public async subscribe(_config: NodeConfig): Promise<void> { this.running = true; }
  public async unsubscribe(): Promise<void> { this.running = false; }
  public async setVolume(volume: number): Promise<void> { this.volume = volume; }
  public isRunning(): boolean { return this.running; }
}

test("control plane reports the pinned profile and accepts configuration", async () => {
  const runtime = new FakeRuntime();
  const server = createControlServer(undefined, runtime);
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const address = server.address();
  assert.ok(address && typeof address !== "string");

  const status = await call(address.port, "GET", "/v1/status");
  assert.equal(status.statusCode, 200);
  assert.equal((status.body as { profile: string }).profile, "AES67-L24-48k-1ms");

  const updated = await call(address.port, "PUT", "/v1/config", {
    nodeId: "stage-left",
    channelCount: 2,
    destination: "239.69.0.2",
    destinationPort: 5004,
  });
  assert.equal(updated.statusCode, 200);
  assert.equal((updated.body as { nodeId: string }).nodeId, "stage-left");

  const health = await call(address.port, "GET", "/health");
  assert.equal(health.statusCode, 200);
  assert.equal((health.body as { ok: boolean }).ok, true);

  const subscribed = await call(address.port, "POST", "/v1/subscribe", {
    destination: "239.69.0.2",
    destinationPort: 5006,
  });
  assert.equal(subscribed.statusCode, 200);
  assert.equal((subscribed.body as { subscription: { active: boolean } }).subscription.active, true);
  assert.equal((subscribed.body as { stream: string }).stream, "running");

  const volume = await call(address.port, "PUT", "/v1/volume", { volume: 0.65 });
  assert.equal(volume.statusCode, 200);
  assert.equal((volume.body as { volume: number }).volume, 0.65);

  const unsubscribed = await call(address.port, "POST", "/v1/unsubscribe");
  assert.equal(unsubscribed.statusCode, 200);
  assert.equal((unsubscribed.body as { subscription: { active: boolean } }).subscription.active, false);

  const websocket = await openWebSocket(address.port);
  assert.match(websocket, /HTTP\/1\.1 101 Switching Protocols/);
  assert.match(websocket, /"type":"status"/);
  server.close();
});

async function openWebSocket(port: number): Promise<string> {
  const client = connect(port, "127.0.0.1");
  await once(client, "connect");
  client.write("GET /v1/events HTTP/1.1\r\n" +
    "Host: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGVzdC1rZXk=\r\n\r\n");
  const [data] = await once(client, "data") as [Buffer];
  client.destroy();
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
