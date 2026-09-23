# Yinit

Yinit, Linux için küçük ve denetlenebilir bir PID 1 init/service manager
projesidir. Hedefi systemd’nin tüm özelliklerini kopyalamak değil; minimal bir
Linux dağıtımını ve fiziksel PC boot sürecini anlaşılır bir servis modeliyle
çalıştırmaktır.

**Sürüm: 1.2** — Ayrıntılı değişiklikler için
[1.2 sürüm notlarına](docs/RELEASE_NOTES_1.2.md) bakın.

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

## 1.2’de eskisine göre düzeltilenler

Yinit 1.2 yalnızca sürüm numarası değişikliği değildir; önceki init prototipinin
gerçek boot için kritik eksiklerini kapatan bir hardening sürümüdür.

| Önceki sorun | 1.2’deki davranış |
|---|---|
| Birden fazla `ExecStart` satırından yalnızca ilki çalışıyordu | `Type=oneshot` için en fazla 32 komut sırayla çalışır, her dönüş kodu kontrol edilir |
| `User=` okunuyor fakat uygulanmıyordu | UID, primary GID ve supplementary group’lar child süreçte uygulanır |
| `WorkingDirectory`, `Environment`, `Nice` eksik/etkisizdi | Servis child’ında uygulanır |
| `After`/`Requires` adları `.service` yüzünden eşleşmeyebiliyordu | İsimler normalize edilir; uzantılı ve uzantısız adlar çalışır |
| Dependency sırası ve döngüler güvenilir değildi | Deterministik dependency sıralaması, eksik bağımlılık ve cycle uyarıları vardır |
| Restart beklemesi PID 1’i `sleep()` ile kilitleyebiliyordu | Restart zamanı planlanır; ana loop çalışmaya devam eder |
| Yalnızca ana PID öldürülüyordu | Process group ve cgroup içindeki child’lar temizlenir |
| cgroup hataları göz ardı ediliyordu | cgroup v2 kurulur; yoksa açık warning ile process-group fallback kullanılır |
| `oneshot`, `forking`, timeout ve post/stop kodları eksikti | Tipler, dönüş kodları, timeout ve lifecycle hook’ları denetlenir |
| Getty’nin `setsid()` çağrısı process-group ile çakışabiliyordu | `TTY=yes` servisleri getty’nin kendi session’ını kurmasına izin verir |
| Control socket herkese açık olabiliyordu | `/run/yinit/control.sock` `0600` root-only oluşturulur |
| `yinitctl` hatalarda başarı döndürebiliyordu | Socket hatasında non-zero exit code ve hata cevabı verir |
| Minimal rootfs mount/device durumuna bağımlıydı | proc/sys/dev/run/tmp/devpts ve temel device node fallback’leri hazırlanır |
| Farklı CPU’larda static glibc binary sorun çıkarabiliyordu | Minimal sistemler için statik musl build akışı eklendi |
| Gerçek PID 1 testi ve rollback akışı yoktu | QEMU PID 1/handoff testleri, rootfs staging ve backup’lı rollback eklendi |

## Nasıl çalışır?

Yinit PID 1 olarak başladığında aşağıdaki sırayı izler:

1. `--version` ve `--check` gibi tanı modlarını kontrol eder.
2. PID’in gerçekten `1` olduğunu doğrular; normal süreçte çalışmayı reddeder.
3. `/var/log/yinit.log` açar ve `umask(022)` ayarlar.
4. `/proc`, `/sys`, `/dev`, `/run`, `/tmp` ve `/dev/pts` mount noktalarını
   hazırlar. Initramfs’in zaten mount ettiği noktaları `EBUSY` nedeniyle bozmaz.
5. `/dev/null`, `/dev/zero`, `/dev/tty`, `/dev/console`, `/dev/random`,
   `/dev/urandom` ve `/dev/ptmx` için eksik temel device node’ları oluşturmayı
   dener.
6. `/dev/console`’u standart input/output/error stream’lerine bağlar.
7. `SIGCHLD`, shutdown sinyalleri ve `SIGHUP` handler’larını kurar; child
   reaping için subreaper etkinleştirir.
8. cgroup v2 servis izolasyonunu kurar; kernel desteklemiyorsa process-group
   fallback’ine geçer.
9. `0600` izinli `/run/yinit/control.sock` socket’ini açar.
10. `/etc/yinit/services/*.service` dosyalarını parse eder, isimleri normalize
    eder ve dependency sırasına koyar.
11. Servisleri `After`/`Requires`/`Wants` ilişkilerine göre başlatır.
12. Ana loop’ta child’ları reaped eder, planlı restart’ları başlatır, control
    socket komutlarını ve reload isteklerini işler.
13. Reboot/poweroff geldiğinde servisleri ters sırada durdurur, süreçleri temizler,
    `sync()` çağırır ve Linux reboot ABI’ını kullanır.

## Servis yaşam döngüsü

Her servis için state değerleri `inactive`, `starting`, `active`, `stopping` ve
`failed` olabilir.

- `Type=simple`: ilk `ExecStart` process’i izlenir.
- `Type=forking`: parent başarılı biçimde çıktıktan sonra servis active sayılır.
- `Type=oneshot`: bütün `ExecStart` satırları beklenir; başarılı servis
  dependency’ler için active/remaining state’te tutulur.
- `Type=notify`: şu an gerçek `READY=1` protokolü yerine simple supervision
  fallback’i kullanır ve warning loglar.
- `Type=idle`: şu an simple supervision davranışına düşer.
- `ExecStartPre` başarısızsa ana komut çalıştırılmaz.
- `ExecStartPost` oneshot cgroup’u kaldırılmadan önce çalışır.
- `ExecStop` stop sırasında çalışır.
- `Restart=always`, `on-failure`, `on-abort` ve `no` desteklenir.
- `RestartSec` ana PID 1 loop’unu bloklamadan bekleme planlar.
- `StartLimitBurst` sonsuz crash-loop’u sınırlar.
- `TimeoutStartSec` aşılırsa process group/cgroup graceful ve forceful olarak
  sonlandırılır.

## Süreç, kullanıcı ve cgroup modeli

Normal servisler ayrı process group’ta başlatılır. Child süreçte şu ayarlar
uygulanabilir:

- `User=` ile kullanıcı/UID değişimi
- `/etc/passwd` primary GID ve `/etc/group` supplementary groups
- `WorkingDirectory=`
- `Environment=KEY=VALUE ...`
- `Nice=`
- cgroup v2 varsa `MemoryLimit=`

cgroup v2 mevcutsa her servis `/sys/fs/cgroup/yinit/<service>` altında tutulur.
`cgroup.procs` ile child eklenir; stop sırasında `cgroup.kill` veya PID listesi
kullanılır. cgroup v2 yoksa Yinit boot’u kesmez, process group’ları kullanır.
Bu fallback bellek limiti ve tam cgroup izolasyonu sağlayamaz.

## Desteklenen servis alanları

| Alan | Açıklama |
|---|---|
| `Description` | `status` çıktısındaki açıklama |
| `ExecStart` | Ana komut; oneshot servislerde çoklu satır |
| `ExecStartPre` | Başlangıç öncesi senkron komut |
| `ExecStartPost` | Başlangıç sonrası komut |
| `ExecStop` | Stop sırasında komut |
| `Type` | `simple`, `forking`, `oneshot`, `notify`, `idle` |
| `After` | Başlatma sıralaması |
| `Requires` | Gerekli servis active değilse başlangıç başarısızlığı |
| `Wants` | Varsa sıralama; yoksa warning |
| `Restart` | Restart politikası |
| `RestartSec` | Planlı restart gecikmesi |
| `StartLimitBurst` | Restart üst sınırı |
| `TimeoutStartSec` | Senkron komut/stop timeout’u |
| `User` | Kullanıcı adı veya UID |
| `WorkingDirectory` | Child çalışma dizini |
| `Environment` | Boşlukla ayrılmış environment değerleri |
| `CGroup` | Özel güvenli cgroup alt adı |
| `MemoryLimit` | cgroup v2 `memory.max` değeri |
| `Nice` | Child nice değeri |
| `TTY` | `yes/true` ile getty session istisnası |

Komutlar rootfs’in `/bin/sh -c` kabuğu üzerinden çalıştırılır. Bu nedenle servis
dosyaları güvenilir root sahibi tarafından korunmalıdır.

## Güvenlik ve kontrol

Yinit’in yanlışlıkla host üzerinde normal servis olarak çalışmasını önlemek için
PID 1 dışındaki çalıştırma reddedilir. Yapılandırma kontrolü için:

```sh
./yinit --check ./etc/yinit/services
```

Control socket yalnızca root erişimine açıktır:

```text
/run/yinit/control.sock  mode 0600
```

Desteklenen `yinitctl` komutları:

```sh
yinitctl status [service]
yinitctl start <service>
yinitctl stop <service>
yinitctl restart <service>
yinitctl reload <service>
yinitctl version
yinitctl reboot
yinitctl poweroff
```

Boş komut, eksik servis adı, bilinmeyen servis veya cevap alınamaması hata
olarak raporlanır.

## Repository yapısı

```text
src/yinit.c                 PID 1 ve servis yöneticisi
src/yinit.h                 Veri yapıları, sabitler ve yardımcılar
src/yinitctl.c              Kontrol istemcisi
etc/yinit/services/         Varsayılan 10 servislik profil
tools/install-rootfs.sh     Rootfs staging ve rollback destekli kurulum
tests/qemu/smoke.sh         İzole QEMU PID 1 testi
tests/qemu/init/handoff     /sbin/init handoff fixture’ı
tests/qemu/services/        Smoke test servisi
docs/PHYSICAL_BOOT.md       Fiziksel PC rollout prosedürü
docs/RELEASE_NOTES_1.2.md  Tam 1.2 teknik sürüm notları
Makefile                    Host, musl, check ve install hedefleri
```

## Test kanıtı

1. Host derlemesi ve config check:

   ```sh
   make
   make check
   ```

   Sonuç: `Configuration OK: 10 services`.

2. PID 1 koruması:

   ```sh
   ./yinit
   ```

   Sonuç: exit code `2` ve güvenli ret mesajı.

3. Portable binary:

   ```sh
   make MUSL_CC=/home/yunustas/yunuslinux/build/musl/bin/musl-gcc musl
   ./yinit --version
   ```

   Sonuç: `Yinit 1.2`, statik musl ELF.

4. QEMU doğrudan PID 1, default servis profili ve `/sbin/init` handoff
   testleri `YINIT_QEMU_SMOKE_OK` ve `Yinit QEMU PID 1 smoke test: PASS` ile
   tamamlandı.

5. Rootfs staging aracı; mevcut init’i koruma, backup’lı `--make-default` ve
   mevcut backup’ı ezmeme davranışlarıyla test edildi.

6. `git diff --check`, `sh -n tests/qemu/smoke.sh` ve
   `sh -n tools/install-rootfs.sh` temiz geçti.

## Gerçek PC durumu ve sınırlamalar

Yinit 1.2 minimal/özel Linux dağıtımlarında konsol boot’u için hazırlandı.
YunusLinux’un mevcut initramfs’i root diski bulup `switch_root` yaptıktan sonra
rootfs `/sbin/init` üzerinden Yinit’i başlatabilir.

İlk fiziksel boot için:

- musl ile static binary üret;
- hedef rootfs’teki binary yollarını kontrol et;
- mevcut GRUB girdisini koru;
- ayrı USB/SSD veya geri dönüşlü boot girdisi kullan;
- ilk beklentiyi TTY/seri login olarak belirle;
- grafik masaüstü için display manager/session servislerini ayrıca ekle.

Yinit kendi başına initramfs root disk keşfi veya `switch_root` yöneticisi
değildir. `Type=notify` tam readiness protokolü değildir. Tam systemd unit
uyumluluğu, grafik oturum, polkit ve her donanımda fiziksel boot garantisi
verilmez. Gerçek fiziksel cihaz testi bu çalışma kapsamında yapılmadı; QEMU’da
PID 1 ve `/sbin/init` handoff doğrulandı.

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
