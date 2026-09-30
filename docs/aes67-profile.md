# AES67 Profile

This repository pins the following interoperable baseline for a speaker node.

| Item | Choice |
| --- | --- |
| PCM encoding | `L24`, signed linear PCM, network byte order |
| Sample rate | 48,000 Hz |
| Packet time | 1 ms, exactly 48 samples per channel |
| RTP | RTP over UDP; timestamp clock is 48 kHz |
| PTP | IEEE 1588-2008 (PTPv2), default profile (`domainNumber = 0`) |
| Multicast | IPv4 administratively scoped multicast; default example `239.69.0.1` |
| Control | Local HTTP JSON API, separate from the RTP media path |

## Packet sizing

The audio payload contains `channels * 48 * 3` bytes. A stereo packet therefore has 288 payload bytes, excluding RTP and UDP/IP headers. Samples are interleaved by channel and each signed 24-bit sample is serialized most significant byte first.

## Timing contract

The RTP timestamp advances by 48 for each packet. A production node must derive its media clock from the selected PTP grandmaster and expose lock/holdover state to the control plane. The C++ packetizer does not claim to implement PTP, clock recovery, RTP headers, or a physical audio device yet.

The current receive path now parses RTP v2 datagrams from UDP, validates the configured dynamic payload type, unpacks L24 into signed PCM, orders packets by RTP timestamp, and writes stereo PCM to ALSA as `S24_3LE`. The jitter buffer waits for an explicitly primed timestamp; it does not conceal missing packets or make a synchronization decision.

## Explicit non-goals for this skeleton

- PTP announce, sync, follow-up, delay request, and servo state machine
- RTP session management, SDP, SAP, and multicast socket lifecycle
- ALSA/CoreAudio/WASAPI device adapters
- Authentication, authorization, and encrypted control transport
