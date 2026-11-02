#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fw_dir=$(cd -- "$script_dir/.." && pwd)
image="$fw_dir/build/HIFIEC70.IMG"
host=${ECHO_MINI_SSH_HOST:-}
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=5 -o ConnectionAttempts=1)

printf 'Building firmware in %s\n' "$fw_dir"
make -C "$fw_dir"
if [[ ! -s "$image" ]]; then
    printf 'Build did not produce %s\n' "$image" >&2
    exit 1
fi
printf 'Built image: %s\n' "$image"
sha256sum "$image"

if [[ -z "$host" ]]; then
    printf 'Copy skipped: set ECHO_MINI_SSH_HOST to enable optional device copy.\n'
    exit 0
fi
if ! command -v ssh >/dev/null 2>&1 || ! command -v scp >/dev/null 2>&1; then
    printf 'Copy skipped: ssh/scp is unavailable; image remains at %s\n' "$image"
    exit 0
fi
if ! ssh "${ssh_opts[@]}" "$host" true >/dev/null 2>&1; then
    printf 'Copy skipped: host %s is unreachable; image remains at %s\n' "$host" "$image"
    exit 0
fi

upload=".NEWFW100.IMG.upload.$$"
if ! scp -p -o BatchMode=yes -o ConnectTimeout=5 "$image" "$host:$upload"; then
    printf 'Upload to %s failed; built image remains at %s\n' "$host" "$image" >&2
    exit 1
fi

set +e
ssh "${ssh_opts[@]}" "$host" bash -s -- "$upload" <<'REMOTE'
set -euo pipefail
upload=$1
device='/dev/disk/by-label/ECHO\x20MINI'
mount_dir=$(findmnt -rn -S 'LABEL=ECHO MINI' -o TARGET | head -n 1 || true)
mounted_here=0
tmp=''
cleanup() {
    [[ -z "$tmp" ]] || rm -f -- "$tmp"
    rm -f -- "$HOME/$upload"
    if [[ $mounted_here -eq 1 ]]; then
        udisksctl unmount -b "$device" >/dev/null 2>&1 ||
            printf 'Warning: could not unmount the ECHO MINI volume.\n' >&2
    fi
}
trap cleanup EXIT
if [[ -z "$mount_dir" ]]; then
    if ! udisksctl mount -b "$device" >/dev/null 2>&1; then
        printf 'NO_DEVICE\n'
        exit 2
    fi
    mounted_here=1
    mount_dir=$(findmnt -rn -S 'LABEL=ECHO MINI' -o TARGET | head -n 1 || true)
fi
if [[ -z "$mount_dir" || ! -w "$mount_dir" ]]; then
    printf 'NO_DEVICE\n'
    exit 2
fi
target="$mount_dir/NEWFW100.IMG"
tmp="$mount_dir/.NEWFW100.IMG.tmp.$$"
cp -- "$HOME/$upload" "$tmp"
mv -- "$tmp" "$target"
tmp=''
sync
printf 'COPIED %s\n' "$target"
sha256sum "$target"
REMOTE
copy_status=$?
set -e
if [[ $copy_status -eq 2 ]]; then
    printf 'No ECHO MINI volume found on %s; build only, image remains at %s\n' "$host" "$image"
elif [[ $copy_status -ne 0 ]]; then
    printf 'Copy to %s failed; built image remains at %s\n' "$host" "$image" >&2
    exit "$copy_status"
fi
