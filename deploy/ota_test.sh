#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
temporary_root=$(mktemp -d)
fake_bin="$temporary_root/bin"
install_root="$temporary_root/install"
release_one="$temporary_root/release-one"
release_two="$temporary_root/release-two"
trap 'rm -rf "$temporary_root"' EXIT

mkdir -p "$fake_bin" "$release_one/build" "$release_two/build"
cat > "$fake_bin/systemctl" <<'EOF'
#!/usr/bin/env bash
command=""
for argument in "$@"; do
  case "$argument" in
    daemon-reload|restart|try-restart|status|is-active)
      command="$argument"
      break
      ;;
  esac
done
case "$command" in
  daemon-reload|restart|try-restart|status) exit 0 ;;
  is-active) exit 0 ;;
  *) echo "unexpected systemctl command: $*" >&2; exit 1 ;;
esac
EOF
chmod +x "$fake_bin/systemctl"

printf 'speaker-one\n' > "$release_one/build/aes67_speaker_node"
printf 'speaker-two\n' > "$release_two/build/aes67_speaker_node"
(cd "$release_one" && sha256sum build/aes67_speaker_node > SHA256SUMS)
(cd "$release_two" && sha256sum build/aes67_speaker_node > SHA256SUMS)

PATH="$fake_bin:$PATH" AES67_INSTALL_ROOT="$install_root" \
  "$script_dir/ota_update.sh" "$release_one"
[[ "$(readlink -f "$install_root/current")" == "$install_root/releases/release-one" ]]
[[ ! -e "$install_root/releases/.release-one.staging" ]]

PATH="$fake_bin:$PATH" AES67_INSTALL_ROOT="$install_root" \
  "$script_dir/ota_update.sh" "$release_two"
[[ "$(readlink -f "$install_root/current")" == "$install_root/releases/release-two" ]]
[[ "$(cat "$install_root/rollback/previous")" == "releases/release-one" ]]

PATH="$fake_bin:$PATH" AES67_INSTALL_ROOT="$install_root" \
  "$script_dir/ota_rollback.sh"
[[ "$(readlink -f "$install_root/current")" == "$install_root/releases/release-one" ]]
[[ "$(cat "$install_root/rollback/previous")" == "releases/release-two" ]]

printf 'OTA update and rollback test passed\n'
