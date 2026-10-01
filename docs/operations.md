# Operations

## Fault injection

Run as root on the interface carrying RTP and PTP traffic:

```sh
sudo ./scripts/netem_fault.sh apply eth0 5% 20ms 5ms
sudo ./scripts/netem_fault.sh clear eth0
```

The script uses Linux `tc netem` and supports packet loss, base latency, and normally distributed jitter. Keep PTP traffic off the injected interface when testing media-only behavior, or apply the impairment intentionally to validate holdover and recovery.

The receiver treats link errors, buffer pressure, and interrupted reads as transient. It periodically reasserts multicast membership while idle; the sender reopens its UDP socket and retries after recoverable route/interface errors. Neither path requires a process restart for a normal interface flap.

## Latency and logs

The local sender/receiver CTest reports whether RTP arrives and decodes. For a running control daemon, measure HTTP control-plane latency with:

```sh
./scripts/measure_latency.sh http://127.0.0.1:8080/health 100
```

Service output is sent to journald:

```sh
journalctl -u aes67-control.service -f
journalctl -u aes67-sender.service -f
journalctl -u aes67-ptp4l.service -f
journalctl -u aes67-phc2sys.service -f
```

## systemd

Copy `deploy/aes67.env.example` to `/etc/aes67/aes67.env`, set the interface and ALSA devices, then install the unit files:

```sh
sudo install -d /etc/aes67
sudo cp deploy/aes67.env.example /etc/aes67/aes67.env
sudo cp deploy/*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now aes67-ptp4l.service aes67-phc2sys.service aes67-control.service
```

Enable `aes67-sender.service` only on a capture/source node.

## OTA updates

A release directory must contain a complete staged tree and a `SHA256SUMS` file covering its files. The update script verifies checksums, copies into a versioned release directory, atomically switches `/opt/aes67/current`, and restarts services:

```sh
(cd release && sha256sum build/aes67_speaker_node control/package-lock.json > SHA256SUMS)
sudo ./deploy/ota_update.sh release-2026-09-30
```

Rollback uses the previous symlink recorded by the updater:

```sh
sudo ./deploy/ota_rollback.sh
```
