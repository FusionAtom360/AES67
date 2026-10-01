#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 apply|clear INTERFACE [LOSS] [DELAY] [JITTER]" >&2
  echo "Example: $0 apply eth0 5% 20ms 5ms" >&2
}

[[ $# -ge 2 ]] || { usage; exit 2; }
action=$1
interface=$2
loss=${3:-5%}
delay=${4:-20ms}
jitter=${5:-5ms}

command -v tc >/dev/null || { echo "tc is required" >&2; exit 1; }

case "$action" in
  apply)
    tc qdisc replace dev "$interface" root netem \
      loss "$loss" delay "$delay" "$jitter" distribution normal
    echo "Applied netem on $interface: loss=$loss delay=$delay jitter=$jitter"
    ;;
  clear)
    tc qdisc del dev "$interface" root 2>/dev/null || true
    echo "Cleared netem on $interface"
    ;;
  *) usage; exit 2 ;;
esac
