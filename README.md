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

The C++ receive path is composed as `UdpReceiver -> ReceivePath -> JitterBuffer -> PlayoutController -> AlsaSink`. The playout controller reads `CLOCK_REALTIME`, which must be disciplined by linuxptp; it maps RTP timestamps to PTP time, applies a playout delay, and adaptively resamples around 48 kHz to compensate for clock and buffer drift.

On Linux, run linuxptp on the media interface before starting the speaker. `ptp4l` exchanges PTP messages and `phc2sys` disciplines `CLOCK_REALTIME` from the interface PHC:

```sh
sudo ptp4l -i eth0 -m -2
sudo phc2sys -s eth0 -c CLOCK_REALTIME -m -O 0
```

Replace `eth0` with the interface carrying the PTP and AES67 traffic. The speaker does not implement PTP messages or a servo itself.

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

## Profile

See [docs/aes67-profile.md](docs/aes67-profile.md) for the normative choices made by this repository and the items that remain outside this skeleton.
