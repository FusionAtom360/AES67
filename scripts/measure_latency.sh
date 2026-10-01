#!/usr/bin/env bash
set -euo pipefail

url=${1:-http://127.0.0.1:8080/health}
count=${2:-20}
command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }

for ((index=0; index<count; ++index)); do
  curl --fail --silent --output /dev/null --write-out '%{time_total}\n' "$url"
done | awk '{ total += $1; if (min == 0 || $1 < min) min = $1; if ($1 > max) max = $1 } END { if (NR > 0) printf "requests=%d min=%.6fs max=%.6fs avg=%.6fs\n", NR, min, max, total/NR }'
