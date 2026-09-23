# Yinit tests

## Configuration check

The check mode does not mount anything and can run as a normal user:

```sh
./yinit --check ./etc/yinit/services
```

## QEMU PID 1 smoke test

This boots Yinit as the real PID 1 in an isolated QEMU guest. It needs a small
rootfs containing a static `/bin/sh`; the YunusLinux build already produces one:

```sh
./tests/qemu/smoke.sh \
  /home/yunustas/yunuslinux/build/rootfs \
  /home/yunustas/yunuslinux/build/iso/boot/vmlinuz

# Also boot the repository's default service profile:
./tests/qemu/smoke.sh \
  /home/yunustas/yunuslinux/build/rootfs \
  /home/yunustas/yunuslinux/build/iso/boot/vmlinuz \
  ./yinit default

# Also exercise the rootfs /sbin/init handoff path:
./tests/qemu/smoke.sh \
  /home/yunustas/yunuslinux/build/rootfs \
  /home/yunustas/yunuslinux/build/iso/boot/vmlinuz \
  ./yinit default handoff

```

The test verifies that Yinit can mount the early API filesystems, attach to the
guest console, load a service file, run an oneshot service, and remain alive as
PID 1. The `handoff` variant starts it through `/sbin/init`, matching the
rootfs-side handoff expected after an initramfs `switch_root`. It never changes
the host bootloader, disks, or init process.
