# Yinit

Yinit, Linux için küçük ve denetlenebilir bir PID 1 init/service manager
projesidir. Hedefi systemd’nin tüm özelliklerini kopyalamak değil; minimal bir
Linux dağıtımını ve fiziksel PC boot sürecini anlaşılır bir servis modeliyle
çalıştırmaktır.

## Mevcut yetenekler

- Gerçek PID 1 olarak çalışır; PID 1 olmayan normal süreçte yanlışlıkla
  çalışmayı reddeder.
- `/proc`, `/sys`, `/dev`, `/run`, `/tmp`, `/dev/pts` API dosya sistemlerini
  yalnızca gerektiğinde mount eder.
- cgroup v2 varsa servis başına izolasyon ve bellek limiti uygular; yoksa
  process group temizliğiyle güvenli bir geri dönüş kullanır.
- `simple`, `forking` ve `oneshot` servis tipleri; `notify` ve `idle` şu an
  güvenli simple-supervision davranışına düşer.
- `After`, `Requires`, `Wants` bağımlılıkları ve döngü/missing dependency
  uyarıları.
- `SIGCHLD` ile child reaping, restart politikaları ve timeout.
- `User`, supplementary groups, `WorkingDirectory`, `Environment`, `Nice`.
- Servis process group/cgroup temizliği ve ters sırada kapanış.
- Root-only `/run/yinit/control.sock` ve `yinitctl` ile status/start/stop/
  restart/reload/reboot/poweroff.
- `--check` ile mount yapmadan servis yapılandırması doğrulama.
- Statik musl binary üretimi ve QEMU PID 1 smoke testi.

## Servis dosyası

Dosyalar `/etc/yinit/services/*.service` altında key-value biçimindedir:

```ini
Description=Console login
Type=simple
TTY=yes
ExecStart=/sbin/getty 38400 tty1
After=filesystems.service
Restart=on-failure
RestartSec=2
```

`ExecStart` birden fazla kez yazılırsa tüm satırlar yalnızca `Type=oneshot`
servislerinde sırayla çalıştırılır. Uzun çalışan servislerde ilk satır ana
komut kabul edilir. `TTY=yes`, getty gibi programların kendi `setsid()` çağrısını
process-group yönetimiyle çakıştırmaz.

## Derleme

Normal geliştirme derlemesi:

```bash
make
make check
```

Farklı x86-64 bilgisayarlarda ve minimal rootfs’lerde kullanılacak taşınabilir
binary için musl önerilir:

```bash
make MUSL_CC=/path/to/musl-gcc musl
```

`make` geliştirme için host libc ile derler; `make musl` ise minimal rootfs ve
farklı x86-64 makinelerde kullanılacak statik artefaktı üretir. Fiziksel
kurulumda ikinci komutun ürettiği binary kullanılmalıdır.

Kurulum varsayılan olarak `/sbin/yinit`, `/usr/local/bin/yinitctl` ve
`/etc/yinit/services/` oluşturur. Bu komut mevcut `/sbin/init` symlink’ini
değiştirmez:

```bash
sudo make install
```

Özel staging rootfs için:

```bash
make install DESTDIR=/tmp/yinit-root
```

Hazır bir rootfs’e bu repository’nin binary ve servis profilini güvenli şekilde
aktarmak için:

```bash
make MUSL_CC=/path/to/musl-gcc musl
./tools/install-rootfs.sh /path/to/rootfs
```

Bu komut mevcut `/sbin/init` dosyasına dokunmaz. Yinit’i o rootfs’in varsayılan
init’i yapmak açıkça istenirse, eski init’i `.yinit-backup` olarak saklayarak:

```bash
./tools/install-rootfs.sh /path/to/rootfs --make-default
```

Geri dönüş için `/sbin/init` symlink’ini kaldırıp
`/sbin/init.yinit-backup` dosyasını eski yerine taşımak yeterlidir.

## QEMU testi

Bu test Yinit’i izole bir QEMU konuğunda gerçek PID 1 olarak başlatır; host
diskine, bootloader’a veya host init sürecine dokunmaz.

YunusLinux’un küçük static rootfs’iyle:

```bash
make MUSL_CC=/path/to/musl-gcc musl
./tests/qemu/smoke.sh \
  /path/to/minimal-rootfs \
  /path/to/vmlinuz \
  ./yinit
```

Varsayılan Yinit servis profilini de denemek için sonuna `default` eklenebilir:

```bash
./tests/qemu/smoke.sh /path/to/rootfs /path/to/vmlinuz ./yinit default
```

## Gerçek PC’de boot etme

Yinit’i ana makinede doğrudan varsayılan init yapmadan önce aşağıdakiler
sağlanmalıdır:

1. Yinit’in musl ile statik binary’si üretilmeli.
2. Servis dosyaları hedef rootfs içindeki gerçek binary yollarına göre
   hazırlanmalı.
3. QEMU smoke ve default-service testleri geçmeli.
4. GRUB’da mevcut init girdisi korunmalı ve Yinit ayrı bir tek-seferlik giriş
   olarak eklenmeli.
5. İlk fiziksel test ayrı USB/SSD veya geri dönüşü hazır bir boot girdisiyle
   yapılmalı.

Yinit initramfs içindeki root diskini otomatik bulup `switch_root` yapan bir
initramfs yöneticisi değildir; bu işi mevcut initramfs’in yapması ve ardından
rootfs içindeki `/sbin/init` symlink’i üzerinden Yinit’i çalıştırması beklenir.
YunusLinux’un mevcut initramfs’i bu handoff’u zaten yapar.

Bu profil fiziksel makinede güvenli bir konsol açılışı içindir; grafik masaüstü,
udev/dbus/logind entegrasyonu ve ağ istemcisi hedef rootfs’teki gerçek yollar
mevcutsa başlatılır. KDE gibi grafik oturumları için ayrıca bir display/session
manager servisi tanımlanmalıdır.

## Kontrol

```bash
yinitctl status
yinitctl status network
yinitctl start network
yinitctl stop network
yinitctl restart network
yinitctl reload dbus
yinitctl reboot
yinitctl poweroff
```

## Lisans

GNU General Public License v2.0.
