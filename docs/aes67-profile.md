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

The current receive path parses RTP v2 datagrams from UDP, validates the configured dynamic payload type and stereo L24 profile, locks to one RTP source, unpacks L24 into signed PCM, orders packets by wrap-safe RTP timestamp, maps the stream to the PTP-disciplined system clock, applies configurable playout delay, adaptively resamples for clock/buffer drift, and writes stereo PCM to ALSA as `S24_3LE`. PTP protocol exchange and servo operation are delegated to linuxptp (`ptp4l` and `phc2sys`). Backward RTP/PTP clock steps remap the playout reference.

## Explicit non-goals for this skeleton

- PTP announce, sync, follow-up, delay request, and servo state machine in this process
- RTP session management, SAP, and multicast sender lifecycle
- CoreAudio/WASAPI device adapters
- Authentication, authorization, and encrypted control transport
