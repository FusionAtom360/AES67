import assert from "node:assert/strict";
import test from "node:test";
import { parseSapAnnouncement } from "../src/discovery.js";

test("SAP parser extracts an AES67 stream", () => {
  const sdp = [
    "v=0", "o=- 1 1 IN IP4 0.0.0.0", "s=Lab source", "c=IN IP4 239.69.0.1",
    "t=0 0", "m=audio 5004 RTP/AVP 96", "a=rtpmap:96 L24/48000/2", "",
  ].join("\r\n");
  const packet = Buffer.concat([Buffer.from([0x20, 0, 0x12, 0x34, 0, 0, 0, 0]),
    Buffer.from("application/sdp\0"), Buffer.from(sdp)]);
  const stream = parseSapAnnouncement(packet, "192.0.2.10");
  assert.equal(stream?.id, "192.0.2.10-1234");
  assert.equal(stream?.destinationPort, 5004);
  assert.equal(stream?.channelCount, 2);
});

test("SAP parser rejects malformed payloads", () => {
  assert.equal(parseSapAnnouncement(Buffer.from("bad"), "192.0.2.10"), undefined);
  assert.equal(parseSapAnnouncement(Buffer.from([0x20, 0, 0, 1, 0, 0, 0, 0, 1]), "192.0.2.10"), undefined);
});