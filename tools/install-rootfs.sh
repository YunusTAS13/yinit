#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
YINIT_BIN=${YINIT_BIN:-$PROJECT_ROOT/yinit}
YINITCTL_BIN=${YINITCTL_BIN:-$PROJECT_ROOT/yinitctl}

usage() {
    echo "Usage: $0 ROOTFS [--make-default]" >&2
    echo "  Stages Yinit and the repository service profile into ROOTFS." >&2
    echo "  --make-default replaces ROOTFS/sbin/init after saving a backup." >&2
}

die() {
    echo "install-rootfs: $*" >&2
    exit 1
}

[ "$#" -ge 1 ] || { usage; exit 2; }
ROOTFS=$1
shift
MAKE_DEFAULT=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --make-default) MAKE_DEFAULT=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; die "unknown option: $1" ;;
    esac
    shift
done

case "$ROOTFS" in
    /*) ;;
    *) die "ROOTFS must be an absolute path" ;;
esac
[ "$ROOTFS" != "/" ] || die "refusing to stage directly into /"
[ -d "$ROOTFS" ] || die "rootfs does not exist: $ROOTFS"
[ -x "$YINIT_BIN" ] || die "build a yinit binary first: $YINIT_BIN"
[ -x "$YINITCTL_BIN" ] || die "build yinitctl first: $YINITCTL_BIN"

SERVICE_DIR=$PROJECT_ROOT/etc/yinit/services
[ -d "$SERVICE_DIR" ] || die "service profile not found: $SERVICE_DIR"

install -d "$ROOTFS/sbin" \
    "$ROOTFS/usr/local/bin" \
    "$ROOTFS/etc/yinit/services" \
    "$ROOTFS/var/log" \
    "$ROOTFS/run/yinit"
install -m 0755 "$YINIT_BIN" "$ROOTFS/sbin/yinit"
install -m 0755 "$YINITCTL_BIN" "$ROOTFS/usr/local/bin/yinitctl"

for service in "$SERVICE_DIR"/*.service; do
    install -m 0644 "$service" "$ROOTFS/etc/yinit/services/"
done

if [ "$MAKE_DEFAULT" -eq 1 ]; then
    init_path=$ROOTFS/sbin/init
    backup_path=$ROOTFS/sbin/init.yinit-backup
    if [ -e "$backup_path" ] || [ -L "$backup_path" ]; then
        die "backup already exists; refusing to overwrite: $backup_path"
    fi
    if [ -e "$init_path" ] || [ -L "$init_path" ]; then
        mv "$init_path" "$backup_path"
    fi
    ln -s yinit "$init_path"
    echo "default init: $init_path -> yinit (backup: $backup_path)"
else
    echo "staged Yinit without changing $ROOTFS/sbin/init"
fi

echo "rootfs ready: $ROOTFS"
