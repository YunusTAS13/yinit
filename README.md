# Yinit

A lightweight, fast init system for Linux. Follows the Unix philosophy - do one thing and do it well.

## Features

- **Service management** - Start, stop, restart, reload services
- **Dependency resolution** - After, Requires, Wants directives with topological sort
- **Process supervision** - Auto-restart on failure with configurable limits
- **cgroup v2** - Resource limits per service (memory, CPU)
- **Control socket** - Unix datagram IPC for yinitctl
- **Multiple service types** - simple, forking, oneshot, notify, idle
- **Graceful shutdown** - Reverse dependency order, SIGTERM then SIGKILL
- **Static binary** - Single ~1MB statically linked binary, no runtime dependencies

## Service File Format

Service files are key=value format (similar to systemd but simpler):

```
Description=D-Bus message bus
Type=simple
ExecStart=/usr/bin/dbus-daemon --system --nofork
After=02-udevd.service
Restart=on-failure
RestartSec=3
StartLimitBurst=5
```

### Directives

| Directive | Description |
|-----------|-------------|
| `Description` | Human-readable description |
| `Type` | Service type: simple, forking, oneshot, notify, idle |
| `ExecStart` | Command to start (can have multiple lines) |
| `ExecStartPre` | Command to run before start |
| `ExecStartPost` | Command to run after start |
| `ExecStop` | Command to run on stop |
| `WorkingDirectory` | Working directory |
| `User` | Run as user |
| `Environment` | KEY=VALUE pairs |
| `CGroup` | cgroup path for resource limits |
| `After` | Start after these services |
| `Requires` | Hard dependencies |
| `Wants` | Soft dependencies |
| `Restart` | Restart policy: no, always, on-failure, on-abort |
| `RestartSec` | Seconds between restarts |
| `StartLimitBurst` | Max restarts before failure |
| `TimeoutStartSec` | Timeout for start |
| `Nice` | Nice priority |
| `MemoryLimit` | Memory limit in bytes |

## Usage

### As PID 1 (init)

```bash
# Compile
make

# Install
sudo make install

# Set as init in kernel command line
# In GRUB: init=/sbin/yinit
# In /etc/inittab: ::respawn:/sbin/yinit
```

### Control

```bash
# List all services
yinitctl status

# Service status
yinitctl status network

# Start/stop/restart
yinitctl start network
yinitctl stop network
yinitctl restart network

# Reload (send SIGHUP)
yinitctl reload dbus

# System control
yinitctl poweroff
yinitctl reboot
```

## Example Service Files

### Filesystem mounting (oneshot)
```
Description=Mount filesystems
Type=oneshot
ExecStart=/sbin/mount -t proc proc /proc
ExecStart=/sbin/mount -t sysfs sysfs /sys
Restart=no
```

### Network (with dependencies)
```
Description=DHCP network
Type=simple
ExecStart=/sbin/udhcpc -i eth0 -n -q -f
After=loopback.service udevd.service
Restart=on-failure
RestartSec=5
```

### Getty with cgroup limits
```
Description=Console login
Type=simple
ExecStart=/sbin/getty 38400 tty1
CGroup=login
MemoryLimit=67108864
After=hostname.service loopback.service
Restart=on-failure
RestartSec=2
```

## Architecture

```
yinit (PID 1)
  |-- Reads /etc/yinit/services/*.service
  |-- Sorts by dependencies (topological sort)
  |-- Starts services in order
  |-- Monitors with SIGCHLD
  |-- Auto-restarts on failure
  |-- Listens on /run/yinit/control.sock
  |-- Handles poweroff/reboot signals
  |-- Mounts essential filesystems
  |-- Creates device nodes
  `-- Manages cgroups
```

## Building

```bash
# Dependencies
# - gcc
# - make
# - linux-headers (for sys/reboot.h)

make          # Build
make install  # Install to /usr/local
make clean    # Clean
```

## Alpine Linux Test

```bash
# Build
make

# Create test rootfs
mkdir -p test-root/etc/yinit/services
cp yinit test-root/sbin/
cp etc/yinit/services/*.service test-root/etc/yinit/services/

# Boot with QEMU
qemu-system-x86_64 \
    -enable-kvm -cpu host \
    -kernel /boot/vmlinuz-virt \
    -initrd test-root.cpio.gz \
    -append "console=ttyS0" \
    -nographic -m 256M
```

## License

MIT
