#!/usr/bin/env bash
set -euo pipefail

release=${1:-}
install_root=${AES67_INSTALL_ROOT:-/opt/aes67}
[[ -n "$release" && -d "$release" ]] || { echo "Usage: $0 RELEASE_DIRECTORY" >&2; exit 2; }
[[ -f "$release/SHA256SUMS" ]] || { echo "release must contain SHA256SUMS" >&2; exit 1; }

(cd "$release" && sha256sum --check --strict SHA256SUMS)
mkdir -p "$install_root/releases" "$install_root/rollback"
version=$(basename "$(realpath "$release")")
staging="$install_root/releases/.${version}.staging"
rm -rf "$staging"
mkdir "$staging"
cp -a "$release"/. "$staging"/
ln -sfn "../releases/$version" "$install_root/next"

if [[ -L "$install_root/current" ]]; then
  current=$(readlink "$install_root/current")
  printf '%s\n' "$current" > "$install_root/rollback/previous"
fi
mv -Tf "$install_root/next" "$install_root/current"
systemctl daemon-reload
systemctl restart aes67-control.service
systemctl try-restart aes67-sender.service || true
systemctl --no-pager --full status aes67-control.service
