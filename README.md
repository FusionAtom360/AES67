# AES67 Speaker Node

Reference repository for a networked speaker node using a deliberately narrow AES67 profile:

- Linear PCM, 24-bit (`L24`)
- 48 kHz sample rate
- 1 ms RTP audio packets (48 samples per channel)
- RTP over UDP with an AES67-compatible payload shape
- PTPv2 default profile for clock synchronization
- C++ audio packet path and a TypeScript control plane

The current implementation is a testable foundation, not a production network stack. It validates the audio payload representation and provides a small HTTP control API for node status and configuration. Transport sockets, PTP servo integration, device I/O, and security policy are intentionally explicit follow-up boundaries.

### Note on LLM usage
This is a self-directed learning project, so I wrote the code myself to make sure I understand every part of it. I used GitHub Copilot as an adversarial reviewer: it critiqued the implementation and the test suite and pointed out gaps. I implemented most of the resulting fixes myself. LLMs also assisted in the editing and formatting of this README. GitHub Copilot wrote the code for `audio_packet_test.cpp` and `sender_receiver_test.cpp`, which are marked in [690b0e516770a1834363d54895e08c9f66fa17fc](https://github.com/FusionAtom360/AES67/commit/bb97ea3a51bdef584d4441e90adb9c3da33d77cd) and [bb97ea3a51bdef584d4441e90adb9c3da33d77cd](https://github.com/FusionAtom360/AES67/commit/690b0e516770a1834363d54895e08c9f66fa17fc). I have reviewed and can explain all of it.

## Layout

```text
audio/       C++ RTP receive path, jitter buffer, L24 packetizer, and ALSA sink
control/     TypeScript HTTP control plane
docs/        pinned profile and implementation notes
```

## Build and test

Requirements: CMake 3.20+, a C++20 compiler, ALSA development/runtime packages
(`libasound2-dev` and `libasound2` on Debian-based Linux), Node.js 20+, and npm.

```sh
cmake -S . -B build -DAES67_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure

cd control
npm ci
npm test
npm run start
```

The C++ tests include a loopback sender/receiver integration test, so `ctest` verifies that the repository's RTP sender feeds its own UDP receiver without requiring ALSA hardware.

The control service listens on `http://127.0.0.1:8080` by default. Set `CONTROL_PORT` or `CONTROL_HOST` to change the listener. To control a speaker from another host, bind it to the speaker network interface and restrict access with the host firewall:

```sh
CONTROL_HOST=0.0.0.0 CONTROL_PORT=8080 npm run start
```

The control daemon exposes:

- `GET /health` for process, PTP, and stream health
- `GET /v1/status` for complete node state
- `PUT /v1/config` to update node configuration
- `POST /v1/subscribe` and `POST /v1/unsubscribe` for stream lifecycle
- `GET /v1/volume` and `PUT /v1/volume` with `{ "volume": 0.0..1.0 }`
- WebSocket `GET /v1/events` for status events and `subscribe`, `unsubscribe`, `setVolume`, and `status` commands

When a stream is subscribed, the control daemon publishes its SDP through SAP on
`224.2.127.254:9875` and repeats the announcement every 30 seconds. Unsubscribing stops the announcements.
The publisher is isolated behind a `DiscoveryPublisher` interface so the SAP implementation can be replaced by
an NMOS IS-04 registry/query client and IS-05 connection manager without changing the control API.

When started from `control`, the daemon launches `../build/aes67_speaker_node` for subscriptions. Set `AES67_SPEAKER_EXECUTABLE` to override the executable path and `AES67_ALSA_DEVICE` to select the playback device. For example:

```sh
AES67_ALSA_DEVICE=hw:1,0 npm run start
curl -X POST http://127.0.0.1:8080/v1/subscribe \
	-H 'content-type: application/json' \
	-d '{"destination":"239.69.0.1","destinationPort":5004}'
curl -X PUT http://127.0.0.1:8080/v1/volume \
	-H 'content-type: application/json' -d '{"volume":0.65}'
curl -X POST http://127.0.0.1:8080/v1/unsubscribe
```

The C++ receive path is composed as `UdpReceiver -> ReceivePath -> JitterBuffer -> PlayoutController -> AlsaSink`. The playout controller reads `CLOCK_REALTIME`, which must be disciplined by linuxptp; it maps RTP timestamps to PTP time, applies a playout delay, and adaptively resamples around 48 kHz to compensate for clock and buffer drift.

On Linux, run linuxptp on the media interface before starting the speaker. `ptp4l` exchanges PTP messages and `phc2sys` disciplines `CLOCK_REALTIME` from the interface PHC:

```sh
sudo ptp4l -i eth0 -m -2
sudo phc2sys -s eth0 -c CLOCK_REALTIME -m -O 0
```

Replace `eth0` with the interface carrying the PTP and AES67 traffic. The speaker does not implement PTP messages or a servo itself.

An optional ALSA capture-to-RTP sender publishes the matching SDP file and sends 1 ms stereo L24 packets:

```sh
./build/aes67_sender_node --device hw:2,0 \
	--destination 239.69.0.1 --port 5004 --sdp stream.sdp
```

Start the sender after linuxptp is locked, then start one or more speakers with the generated `stream.sdp`.

On Linux, after identifying the playback device with `aplay -l`, run:

```sh
./build/aes67_speaker_node --bind 0.0.0.0 --port 5004 \
	--payload-type 96 --first-timestamp 0 --device hw:1,0
```

For an IPv4 multicast stream, provide the group and optional receiving interface:

```sh
./build/aes67_speaker_node --multicast 239.69.0.1 --interface 192.168.1.20 \
	--port 5004 --payload-type 96 --device hw:1,0
```

The receiver joins the group with IGMP and marks its IPv4 socket with DSCP EF (46).
`aes67::SdpSession` parses and generates the matching single-audio-line SDP profile.

For synchronized SDP mode, the sender must generate RTP timestamps from the PTP sample clock (the `a=mediaclk:direct=0` declaration), increment by 48, and send 288-byte stereo L24 payloads every 1 ms. All speakers then schedule the same RTP timestamp against the shared PTP epoch. Use `--help` to list the executable options.

The receiver can instead derive the media and network settings from an SDP file:

```sh
./build/aes67_speaker_node --sdp stream.sdp --device hw:1,0
```

In SDP mode, the first valid RTP timestamp primes the jitter buffer automatically. The default playout delay is 20 ms and can be changed with `--playout-delay-ms`.

Operational hardening tools, systemd units, latency checks, and atomic OTA update/rollback scripts are documented in [docs/operations.md](docs/operations.md).

## Profile

See [docs/aes67-profile.md](docs/aes67-profile.md) for the normative choices made by this repository and the items that remain outside this skeleton.
