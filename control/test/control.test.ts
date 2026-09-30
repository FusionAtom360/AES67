import assert from "node:assert/strict";
import { once } from "node:events";
import { request } from "node:http";
import test from "node:test";
import { createControlServer } from "../src/index.js";

test("control plane reports the pinned profile and accepts configuration", async () => {
  const server = createControlServer();
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
  server.close();
});

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
