# Yinit

**Sürüm: 1.1**

Lightweight, fast init system for Linux. Follows the Unix philosophy - do one thing and do it well.

Linux için hafif ve hızlı init sistemi. Unix felsefesini takip eder: bir şey yap, iyi yap.

## Features / Özellikler

- **Service management** - Start, stop, restart, reload services
- **Dependency resolution** - After, Requires, Wants directives with topological sort
- **Process supervision** - Auto-restart on failure with configurable limits
- **cgroup v2** - Resource limits per service (memory, CPU)
- **Control socket** - Unix datagram IPC for yinitctl
- **Multiple service types** - simple, forking, oneshot, notify, idle
- **Graceful shutdown** - Reverse dependency order, SIGTERM then SIGKILL
- **Static binary** - Single ~1MB statically linked binary, no runtime dependencies

- **Servis yönetimi** - Servisleri başlat, durdur, yeniden başlat, yeniden yükle
- **Bağımlılık çözümlemesi** - After, Requires, Wants yönergeleri ile topolojik sıralama
- **Süreç denetimi** - Hata durumunda otomatik yeniden başlatma, yapılandırılabilir limitler
- **cgroup v2** - Servis başına kaynak sınırları (bellek, CPU)
- **Kontrol soketi** - yinitctl için Unix datagram IPC
- **Çoklu servis tipi** - simple, forking, oneshot, notify, idle
- **Yumuşak kapatma** - Ters bağımlılık sırası, önce SIGTERM sonra SIGKILL
- **Statik binary** - Tek ~1MB statik bağlı binary, runtime bağımlılığı yok

## Service File Format / Servis Dosyası Formatı

Service files are key=value format (similar to systemd but simpler):

Servis dosyaları key=value formatındadır (systemd'e benzer ama daha basit):

```
Description=D-Bus message bus
Type=simple
ExecStart=/usr/bin/dbus-daemon --system --nofork
After=02-udevd.service
Restart=on-failure
RestartSec=3
StartLimitBurst=5
```

### Directives / Yönergeler

| Directive | Description / Açıklama |
|-----------|-------------|
| `Description` | Human-readable description / İnsan tarafından okunabilir açıklama |
| `Type` | Service type: simple, forking, oneshot, notify, idle / Servis tipi |
| `ExecStart` | Command to start (can have multiple lines) / Başlatma komutu |
| `ExecStartPre` | Command to run before start / Başlatmadan önce çalıştırılacak komut |
| `ExecStartPost` | Command to run after start / Başlatmadan sonra çalıştırılacak komut |
| `ExecStop` | Command to run on stop / Durdurma komutu |
| `WorkingDirectory` | Working directory / Çalışma dizini |
| `User` | Run as user / Kullanıcı olarak çalıştır |
| `Environment` | KEY=VALUE pairs / Ortam değişkenleri |
| `CGroup` | cgroup path for resource limits / cgroup yolu |
| `After` | Start after these services / Bu servislerden sonra başlat |
| `Requires` | Hard dependencies / Zorunlu bağımlılıklar |
| `Wants` | Soft dependencies / İsteğe bağlı bağımlılıklar |
| `Restart` | Restart policy: no, always, on-failure, on-abort / Yeniden başlatma politikası |
| `RestartSec` | Seconds between restarts / Yeniden başlatmalar arası saniye |
| `StartLimitBurst` | Max restarts before failure / Başarısızlık öncesi maks yeniden başlatma |
| `TimeoutStartSec` | Timeout for start / Başlatma zaman aşımı |
| `Nice` | Nice priority / Nice önceliği |
| `MemoryLimit` | Memory limit in bytes / Bellek limiti (byte) |

## Usage / Kullanım

### As PID 1 (init) / PID 1 olarak

```bash
# Compile / Derle
make

# Install / Kur
sudo make install

# Set as init in kernel command line / Kernel komut satırında init olarak ayarla
# In GRUB: init=/sbin/yinit
# In /etc/inittab: ::respawn:/sbin/yinit
```

### Control / Kontrol

```bash
# List all services / Tüm servisleri listele
yinitctl status

# Service status / Servis durumu
yinitctl status network

# Start/stop/restart / Başlat/durdur/yeniden başlat
yinitctl start network
yinitctl stop network
yinitctl restart network

# Reload (send SIGHUP) / Yeniden yükle (SIGHUP gönder)
yinitctl reload dbus

# System control / Sistem kontrolü
yinitctl poweroff
yinitctl reboot
```

## Example Service Files / Örnek Servis Dosyaları

### Filesystem mounting (oneshot) / Dosya sistemi mount (oneshot)
```
Description=Mount filesystems
Type=oneshot
ExecStart=/sbin/mount -t proc proc /proc
ExecStart=/sbin/mount -t sysfs sysfs /sys
Restart=no
```

### Network (with dependencies) / Ağ (bağımlılıklar ile)
```
Description=DHCP network
Type=simple
ExecStart=/sbin/udhcpc -i eth0 -n -q -f
After=loopback.service udevd.service
Restart=on-failure
RestartSec=5
```

### Getty with cgroup limits / cgroup limitli Getty
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

## Architecture / Mimari

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

  /etc/yinit/services/*.service dosyalarını okur
  Bağımlılıklara göre sıralar (topolojik sıralama)
  Servisleri sırayla başlatır
  SIGCHLD ile izler
  Hata durumunda otomatik yeniden başlatır
  /run/yinit/control.sock üzerinden dinler
  poweroff/reboot sinyallerini yönetir
  Temel dosya sistemlerini mount eder
  Cihaz düğümleri oluşturur
  cgroup'ları yönetir
```

## Building / Derleme

```bash
# Dependencies / Bağımlılıklar
# - gcc
# - make
# - linux-headers (for sys/reboot.h)

make          # Build / Derle
make install  # Install to /usr/local / /usr/local'a kur
make clean    # Clean / Temizle
```

## Alpine Linux Test

```bash
# Build / Derle
make

# Create test rootfs / Test rootfs oluştur
mkdir -p test-root/etc/yinit/services
cp yinit test-root/sbin/
cp etc/yinit/services/*.service test-root/etc/yinit/services/

# Boot with QEMU / QEMU ile boot et
qemu-system-x86_64 \
    -enable-kvm -cpu host \
    -kernel /boot/vmlinuz-virt \
    -initrd test-root.cpio.gz \
    -append "console=ttyS0" \
    -nographic -m 256M
```

## Contributors / Katkıda Bulunanlar

- **YunusTAS13** - Author / Yazar

## License / Lisans

GNU General Public License v2.0 - See [LICENSE](LICENSE) for details.

GNU Genel Kamu Lisansı v2.0 - Detaylar için [LICENSE](LICENSE) dosyasına bakın.
