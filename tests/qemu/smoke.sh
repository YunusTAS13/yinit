#!/bin/sh
set -eu

BASE_ROOT=${1:?usage: $0 <minimal-rootfs> <kernel> [yinit] [service-mode] [boot-mode]}
KERNEL=${2:?usage: $0 <minimal-rootfs> <kernel> [yinit] [service-mode] [boot-mode]}
YINIT_BIN=${3:-"$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)/yinit"}
SERVICE_MODE=${4:-smoke}
BOOT_MODE=${5:-direct}
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for tool in qemu-system-x86_64 cpio gzip; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool" >&2
        exit 2
    }
done
[ -d "$BASE_ROOT" ] || { echo "rootfs not found: $BASE_ROOT" >&2; exit 2; }
[ -f "$KERNEL" ] || { echo "kernel not found: $KERNEL" >&2; exit 2; }
[ -x "$YINIT_BIN" ] || { echo "yinit binary not found: $YINIT_BIN" >&2; exit 2; }

TMPDIR=$(mktemp -d /tmp/yinit-qemu.XXXXXX)
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT INT TERM

mkdir -p "$TMPDIR/root"
cp -a "$BASE_ROOT"/. "$TMPDIR/root"/
rm -rf "$TMPDIR/root/etc/yinit"
mkdir -p "$TMPDIR/root/etc/yinit/services" "$TMPDIR/root/var/log"
if [ "$BOOT_MODE" = handoff ]; then
    mkdir -p "$TMPDIR/root/sbin"
    cp "$YINIT_BIN" "$TMPDIR/root/sbin/init"
    chmod 0755 "$TMPDIR/root/sbin/init"
    cp "$PROJECT_ROOT/tests/qemu/init/handoff" "$TMPDIR/root/init"
else
    cp "$YINIT_BIN" "$TMPDIR/root/init"
fi
chmod 0755 "$TMPDIR/root/init"
if [ "$SERVICE_MODE" = default ]; then
    cp "$PROJECT_ROOT"/etc/yinit/services/*.service "$TMPDIR/root/etc/yinit/services/"
fi
cp "$(dirname -- "$0")/services/00-smoke.service" \
   "$TMPDIR/root/etc/yinit/services/00-smoke.service"

(cd "$TMPDIR/root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "$TMPDIR/initramfs.gz")

QEMU_ARGS="-M q35 -m 256M -nographic -no-reboot -kernel $KERNEL -initrd $TMPDIR/initramfs.gz -append console=ttyS0\ rdinit=/init\ panic=-1"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then QEMU_ARGS="-enable-kvm $QEMU_ARGS"; fi

set +e
OUTPUT=$(timeout 8s sh -c "qemu-system-x86_64 $QEMU_ARGS" 2>&1)
STATUS=$?
set -e
printf '%s\n' "$OUTPUT"

echo "$OUTPUT" | grep -q 'YINIT_QEMU_SMOKE_OK' || {
    echo "Yinit did not complete the PID 1 smoke test (qemu status $STATUS)" >&2
    exit 1
}
echo "Yinit QEMU PID 1 smoke test: PASS"
