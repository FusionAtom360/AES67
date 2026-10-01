#!/usr/bin/env bash
set -euo pipefail
install_root=${AES67_INSTALL_ROOT:-/opt/aes67}
previous_file="$install_root/rollback/previous"
[[ -s "$previous_file" ]] || { echo "no recorded release to roll back to" >&2; exit 1; }
previous=$(cat "$previous_file")
previous_path=$(realpath -e -- "$install_root/$previous")
[[ "$previous_path" == "$install_root/releases/"* && -d "$previous_path" ]] || {
	echo "rollback release is invalid: $previous" >&2
	exit 1
}

if [[ -L "$install_root/current" ]]; then
	current=$(readlink -f "$install_root/current")
	[[ "$current" == "$install_root/releases/"* ]] || {
		echo "current does not point into the releases directory: $current" >&2
		exit 1
	}
	printf '%s\n' "${current#"$install_root/"}" > "$previous_file"
fi

new_link="$install_root/.current.$$.new"
trap 'rm -f "$new_link"' EXIT
ln -s "${previous#./}" "$new_link"
mv -Tf "$new_link" "$install_root/current"
systemctl daemon-reload
systemctl restart aes67-control.service
systemctl is-active --quiet aes67-control.service
systemctl try-restart aes67-sender.service || true
