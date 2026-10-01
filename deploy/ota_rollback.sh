#!/usr/bin/env bash
set -euo pipefail
install_root=${AES67_INSTALL_ROOT:-/opt/aes67}
previous_file="$install_root/rollback/previous"
[[ -s "$previous_file" ]] || { echo "no recorded release to roll back to" >&2; exit 1; }
previous=$(cat "$previous_file")
[[ -d "$install_root/$previous" ]] || { echo "rollback release is missing: $previous" >&2; exit 1; }
ln -sfn "$previous" "$install_root/next"
mv -Tf "$install_root/next" "$install_root/current"
systemctl daemon-reload
systemctl restart aes67-control.service
systemctl try-restart aes67-sender.service || true
