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
final="$install_root/releases/$version"
rm -rf "$staging"
[[ ! -e "$final" && ! -L "$final" ]] || {
  echo "release already exists: $final" >&2
  exit 1
}
mkdir "$staging"
cp -a "$release"/. "$staging"/
mv -T "$staging" "$final"

new_link="$install_root/.current.$$.new"
trap 'rm -f "$new_link"' EXIT

if [[ -L "$install_root/current" ]]; then
  current=$(readlink -f "$install_root/current")
  [[ "$current" == "$install_root/releases/"* ]] || {
    echo "current does not point into the releases directory: $current" >&2
    exit 1
  }
  printf '%s\n' "${current#"$install_root/"}" > "$install_root/rollback/previous"
fi
ln -s "releases/$version" "$new_link"
mv -Tf "$new_link" "$install_root/current"
systemctl daemon-reload
systemctl restart aes67-control.service
systemctl is-active --quiet aes67-control.service
systemctl try-restart aes67-sender.service || true
systemctl --no-pager --full status aes67-control.service
