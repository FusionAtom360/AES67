# AES67 Speaker Node

Reference repository for a networked speaker node using a deliberately narrow AES67 profile:

- Linear PCM, 24-bit (`L24`)
- 48 kHz sample rate
- 1 ms RTP audio packets (48 samples per channel)
- RTP over UDP with an AES67-compatible payload shape
- PTPv2 default profile for clock synchronization
- C++ audio packet path and a TypeScript control plane

The current implementation is a testable foundation, not a production network stack. It validates the audio payload representation and provides a small HTTP control API for node status and configuration. Transport sockets, PTP servo integration, device I/O, and security policy are intentionally explicit follow-up boundaries.

## Layout

```text
audio/       C++ RTP receive path, jitter buffer, L24 packetizer, and ALSA sink
control/     TypeScript HTTP control plane
docs/        pinned profile and implementation notes
```

## Build and test

Requirements: CMake 3.20+, a C++20 compiler, Node.js 20+, and npm.

```sh
cmake -S . -B build -DAES67_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure

cd control
npm ci
npm test
npm run start
```

The control service listens on `http://127.0.0.1:8080` by default. Set `CONTROL_PORT` to change the port.

The C++ receive path is composed as `UdpReceiver -> ReceivePath -> JitterBuffer -> AlsaSink`. The `aes67_speaker_node` executable wires these components together. PTP is deliberately not used for clock recovery yet.

On Linux, after identifying the playback device with `aplay -l`, run:

```sh
./build/aes67_speaker_node --bind 0.0.0.0 --port 5004 \
	--payload-type 96 --first-timestamp 0 --device hw:1,0
```

The test sender must begin with RTP timestamp `0` for the default `--first-timestamp`, increment timestamps by 48, and send 288-byte stereo L24 payloads every 1 ms. Use `--help` to list the executable options.

## Profile

See [docs/aes67-profile.md](docs/aes67-profile.md) for the normative choices made by this repository and the items that remain outside this skeleton.
