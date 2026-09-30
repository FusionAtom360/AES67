import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { pathToFileURL } from "node:url";

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
}

export const defaultConfig: NodeConfig = {
  nodeId: "speaker-node-01",
  channelCount: 2,
  destination: "239.69.0.1",
  destinationPort: 5004,
};

export function createControlServer(initialConfig: NodeConfig = defaultConfig): Server {
  const status: NodeStatus = {
    profile: "AES67-L24-48k-1ms",
    ptp: "unlocked",
    stream: "stopped",
    config: { ...initialConfig },
  };

  return createServer((request, response) => handleRequest(request, response, status));
}

function handleRequest(request: IncomingMessage, response: ServerResponse, status: NodeStatus): void {
  response.setHeader("content-type", "application/json");
  if (request.method === "GET" && request.url === "/health") {
    send(response, 200, { ok: true });
    return;
  }
  if (request.method === "GET" && request.url === "/v1/status") {
    send(response, 200, status);
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

function send(response: ServerResponse, statusCode: number, body: unknown): void {
  response.statusCode = statusCode;
  response.end(JSON.stringify(body));
}

if (process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const port = Number(process.env.CONTROL_PORT ?? 8080);
  createControlServer().listen(port, "127.0.0.1", () => {
    console.log(`AES67 control plane listening on http://127.0.0.1:${port}`);
  });
}
