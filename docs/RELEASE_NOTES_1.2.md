# Yinit 1.2 — tam teknik sürüm notları

Yinit 1.2, Linux açılışında PID 1 olarak çalışan küçük bir init ve servis
yöneticisidir. Bu sürümün odağı, Yinit’i systemd’nin bütün API’lerini taklit
eden bir ürün yapmak değil; minimal bir Linux rootfs’inde fiziksel PC boot
sürecini, servis yaşam döngüsünü, konsol login’ini ve kapanışı güvenli,
gözlemlenebilir ve geri dönüşü mümkün hale getirmektir.

Bu belge yalnızca yeni özellikleri değil, önceki kodda bulunan sorunları,
1.2’deki davranış değişikliklerini, dosya bazında yapılanları, test kanıtını ve
bilinçli olarak desteklenmeyen alanları da açıklar.

## 1. Sürüm özeti

- Sürüm çıktısı: `Yinit 1.2`
- Git etiketi: `v1.2`
- Hedef: gerçek PID 1, minimal rootfs, TTY/seri konsol ve fiziksel PC boot
- Doğrulama: host dışı QEMU PID 1 smoke, default servis profili ve `/sbin/init`
  handoff testi
- Lisans: GNU GPLv2

Yinit 1.2, “her dağıtımda hiçbir ayar gerektirmeden systemd’nin yerine geçer”
iddiası taşımaz. Servis dosyaları Yinit’in sade key-value formatındadır ve hedef
rootfs’teki program yolları ayrıca kontrol edilmelidir.

## 2. 1.2 öncesindeki durum ve eski buglar

Önceki kod küçük bir init prototipiydi. Gerçek donanım boot’u için kritik olan
birçok davranış ya eksikti ya da hata sonucunu yok sayıyordu.

### Servis komutları

Önceki davranışta aynı serviste birden fazla `ExecStart=` satırı olsa bile
yalnızca ilk satır çalışıyordu. Bu, özellikle hazırlık komutlarının ve çok
aşamalı `oneshot` servislerin sessizce eksik çalışmasına neden oluyordu.

1.2’de:

- En fazla 32 `ExecStart` satırı parse edilir.
- `Type=oneshot` için bütün satırlar sırayla ve ayrı ayrı çalıştırılır.
- Her komutun dönüş kodu kontrol edilir.
- Bir komut başarısız olursa sonraki komutlar çalıştırılmaz ve servis `failed`
  durumuna geçer.
- Uzun çalışan servislerde ilk `ExecStart` ana süreç olarak izlenir.

### Kullanıcı ve izinler

Önceki kod `User=` alanını okuyabiliyor görünse de servisi istenen kullanıcıya
geçirmiyordu. Sonuç olarak servisin root olarak çalışması ve beklenmeyen izin
alanına sahip olması mümkündü.

1.2’de:

- `User=yunus` veya `User=1000` desteklenir.
- `/etc/passwd` içinden UID ve primary GID bulunur.
- `/etc/group` içinden supplementary group’lar toplanır.
- `setgroups()`, `setgid()` ve `setuid()` servis child sürecinde uygulanır.
- Kullanıcı bulunamazsa servis 126 benzeri başarısızlıkla başlatılmaz.
- Root olmayan bir Yinit’in `User=` uygulamaya çalışması hata olarak loglanır.

Buna ek olarak önceki eksik davranışlar tamamlandı:

- `WorkingDirectory=` child süreçte uygulanır.
- `Environment=` değerleri child ortamına eklenir.
- `Nice=` ile öncelik ayarı denenir.
- `MemoryLimit=` cgroup v2 mevcutsa `memory.max` üzerine yazılır.

### Servis isimleri ve bağımlılıklar

Önceki kodda dosya adı `network.service`, bağımlılık adı ise `network` olarak
geldiğinde eşleşme sorunları oluşabiliyordu. Bu durum `After=` ve `Requires=`
ilişkilerini boşa çıkarabiliyordu.

1.2’de:

- Servis isimleri yüklenirken `.service` uzantısı normalize edilir.
- Bağımlılık adlarının `.service` uzantılı veya uzantısız yazılması desteklenir.
- Servis dosyaları deterministik alfabetik sıraya alınır.
- Bağımlılıklar DFS/topolojik sıralama ile önce başlatılır.
- Eksik bağımlılık loglanır.
- Eksik `Requires=` bağımlılığı servis başlatılırken başarısızlık üretir.
- Bağımlılık döngüsü loglanır; süreç kontrolsüz sonsuz döngüye girmez.
- `yinitctl status network` gibi kısa adlar numaralı dosyalara eşlenir; örneğin
  `07-network.service` için `network` kullanılabilir.

### Restart ve PID 1 ana döngüsü

Önceki restart yaklaşımında PID 1, restart beklerken doğrudan `sleep()` ile
bloklanabiliyordu. Bu sırada başka servislerin child’ları reapedilemez,
kontrol socket’i işlenemez ve shutdown gecikebilirdi.

1.2’de:

- `Restart=no`, `always`, `on-failure` ve `on-abort` desteklenir.
- `RestartSec=` bekleme süresini belirler.
- `StartLimitBurst=` üst üste restart sayısını sınırlar.
- Restart zamanı `next_restart_at` ile planlanır.
- PID 1 ana döngüsü bloklanmadan diğer servisleri, socket’i ve sinyalleri
  işlemeye devam eder.
- Süre dolduğunda servis yeniden başlatılır.
- Servis manuel `start` edilirse bekleyen restart planı temizlenir.
- `stop` ve shutdown restart planını iptal eder.

### Process temizliği

Önceki kod çoğunlukla yalnızca ana PID’i sonlandırıyordu. Shell wrapper’ları,
child process’ler veya servis tarafından başlatılmış yardımcı süreçler geride
kalabiliyordu.

1.2’de:

- Her normal servis kendi process group’unda başlatılır.
- Stop sırasında önce process group’a `SIGTERM`, timeout sonrası `SIGKILL`
  gönderilir.
- cgroup v2 varsa cgroup içindeki bütün PID’ler ayrıca sinyallenir.
- `cgroup.kill` yoksa `cgroup.procs` okunarak fallback uygulanır.
- `PR_SET_PDEATHSIG` ile parent Yinit’ten önce ölürse child’a `SIGTERM` istenir.
- `SIGCHLD` ile child’lar düzenli biçimde reapedilir.
- Orphan child’ların PID 1 tarafından sahiplenilmesi için subreaper ayarlanır.

### cgroup ve hata kontrolü

Önceki kod cgroup’ların mevcut olduğunu varsayabiliyor, mount/creation
hatalarını önemsemiyor veya servis izolasyonu kurmuyordu.

1.2 cgroup davranışı:

1. `/sys/fs/cgroup/cgroup.controllers` okunabiliyorsa mevcut cgroup v2 kökü
   kullanılır.
2. Bu yol yoksa `/run/yinit/cgroup` altında cgroup2 mount edilmeyi denenir.
3. `yinit` cgroup kökü ve servis başına alt dizin oluşturulur.
4. `+cpu +memory +pids` subtree controller’ları best-effort açılır.
5. Servis child’ı `cgroup.procs` içine alınır.
6. Stop/shutdown sırasında cgroup içi süreçler temizlenir.
7. Kernel cgroup2 sunmuyorsa Yinit boot’u durdurmaz; process-group fallback’e
   geçer ve bunu açıkça warning olarak loglar.

Servis adı cgroup path’i içinde kullanıldığı için `/`, `..`, `../` ve benzeri
path traversal biçimleri reddedilir.

### Servis tipleri

Önceki kod servis tipi alanını parse etse de tiplerin yaşam döngüsü davranışları
eksikti.

1.2 davranışı:

- `simple`: ilk `ExecStart` child süreç olarak izlenir.
- `forking`: parent süreç başarıyla çıktıktan sonra servis active kabul edilir.
- `oneshot`: bütün `ExecStart` satırları beklenir ve başarılıysa active/remaining
  state olarak tutulur.
- `notify`: gerçek readiness socket’i henüz yoktur; güvenli simple supervision
  fallback’i kullanılır ve warning loglanır.
- `idle`: şu an simple supervision davranışına düşer.

`ExecStartPre=`, `ExecStartPost=` ve `ExecStop=` komutları da servis
timeout/başarı akışına bağlandı. `ExecStartPost` oneshot cgroup’u kaldırılmadan
önce çalıştırılır.

### Mount ve erken boot

Önceki kod rootfs’in mount durumuna fazla bağımlıydı ve minimal sistemlerde
`/run`, `/dev/pts` veya device node eksiklikleri login’i bozabiliyordu.

1.2 PID 1 başlangıcında:

- `/proc` procfs olarak denenir.
- `/sys` sysfs olarak denenir.
- `/dev` devtmpfs olarak denenir.
- `/run` nosuid/nodev tmpfs olarak denenir.
- `/tmp` nosuid/nodev tmpfs olarak denenir.
- `/dev/pts` devpts olarak denenir.
- Hedef zaten mount edilmişse `EBUSY` hata olarak boot’u kesmez.
- Gerekli temel character device’lar yoksa oluşturulmaları denenir.
- `/var/log/yinit.log` açılır; PID 1 console’a da log yazar.
- `/dev/console` açılarak standart input/output/error hazırlanır.

Yinit shutdown sırasında rootfs mount’larını körlemesine unmount etmez. Böylece
initramfs’in, rootfs’in veya başka bir yöneticinin sağladığı mount’ları bozma
riski azaltılır.

### Getty ve TTY

Önceki process-group davranışı getty’nin kendi `setsid()` çağrısıyla
çatışabiliyordu. Bu da `setsid: Operation not permitted` benzeri hatalara yol
açabiliyordu.

1.2’de `TTY=yes` olan servislerde Yinit child’a zorla process group kurmaz.
Getty kendi session’ını kurabilir. Varsayılan profilde:

- `10-getty-tty1.service` sanal TTY1’i açar.
- `11-getty-serial.service` seri console’u açar.
- `/sbin/getty` varsa kullanılır.
- Yoksa `/usr/bin/agetty` denenir.
- Getty programı bulunamazsa servis durumu loglanır.

### Ağ ve temel servis profili

Önceki servis dosyaları sabit binary yollarına ve `eth0` adına fazla bağımlıydı.
Minimal rootfs’te olmayan bir udev/dbus/logind binary’si boot davranışını
gereksiz yere bozabiliyordu.

1.2 profilindeki servisler:

- `01-filesystems`: mount işini PID 1 yaptığı için yalnızca başarılı bir no-op
  oneshot olarak bulunur.
- `02-udevd`: `/sbin/udevd` veya systemd-udevd varsa başlatır.
- `03-modprobe`: modprobe varsa loop modülünü best-effort yükler.
- `04-dbus`: dbus-daemon varsa system bus’ı çalıştırır.
- `05-hostname`: hostname’i `yinit` yapar.
- `06-loopback`: `ip` veya `ifconfig` ile loopback’i kaldırır.
- `07-network`: YunusLinux network helper’ını veya udhcpc/dhcpcd’yi kullanır.
- `08-logind`: elogind veya systemd-logind varsa başlatır.
- `10-getty-tty1`: sanal konsol login’i.
- `11-getty-serial`: seri konsol login’i.

Ağ servisi `/sys/class/net` içinden `lo` dışındaki interface’i arar. Interface
yoksa `exit 0` ile çıkar; bu durum boot’u beş saniyelik restart döngüsüne sokmaz.
YunusLinux `udhcpc.script` varsa DHCP lease sonrası IP, route ve DNS ayarlarına
yardım eder.

## 3. Yeni güvenlik ve kontrol davranışı

### PID 1 koruması

```sh
./yinit
```

normal süreçte çalıştırıldığında şu davranışı verir:

```text
Yinit refuses to run outside PID 1; use --check for validation.
```

Bu, yanlışlıkla çalışan bir Yinit’in host servislerini veya süreçlerini
sonlandırmasını önler.

### Control socket

Socket yolu:

```text
/run/yinit/control.sock
```

Socket `0600` izinle oluşturulur. Komutlar:

```sh
yinitctl status
yinitctl status network
yinitctl start network
yinitctl stop network
yinitctl restart network
yinitctl reload dbus
yinitctl version
yinitctl reboot
yinitctl poweroff
```

Boş komutlar, eksik servis adı ve bilinmeyen servisler hata cevabı alır.
`yinitctl` cevap alamazsa sıfır dışı kod döndürür; önceki sessiz başarı davranışı
düzeltilmiştir.

### Shutdown

Shutdown iki yoldan tetiklenebilir:

- `SIGTERM`, `SIGINT`, `SIGQUIT`, `SIGPWR`
- control socket üzerinden `reboot` veya `poweroff`

Akış:

1. Servisler ters başlatma sırasında stop edilir.
2. Önce graceful sinyal gönderilir.
3. Timeout aşılırsa cgroup/process group kill edilir.
4. Kalan PID 1 child’ları `SIGTERM` ve sonra `SIGKILL` ile temizlenir.
5. `sync()` çağrılır.
6. Linux reboot ABI ile reboot veya poweroff istenir.

Reboot syscall’i beklenmedik biçimde başarıyla geri dönerse Yinit PID 1 olarak
çıkmak yerine canlı kalır; PID 1’in ölmesi engellenir.

## 4. Servis dosyası formatı

Dosyalar:

```text
/etc/yinit/services/*.service
```

Örnek:

```ini
Description=Console login
Type=simple
TTY=yes
ExecStart=/sbin/getty 38400 tty1
After=filesystems.service
Restart=on-failure
RestartSec=2
TimeoutStartSec=90
User=root
WorkingDirectory=/
Environment=TERM=linux
Nice=0
```

Desteklenen alanlar:

| Alan | Davranış |
|---|---|
| `Description` | status çıktısındaki açıklama |
| `ExecStart` | ana komut; oneshot’ta birden çok satır desteklenir |
| `ExecStartPre` | ana komuttan önce bir kez çalışır |
| `ExecStartPost` | start sonrası çalışır |
| `ExecStop` | stop sırasında çalışır |
| `Type` | `simple`, `forking`, `oneshot`, `notify`, `idle` |
| `After` | yalnızca başlatma sırası |
| `Requires` | bağımlı servis active değilse start başarısız |
| `Wants` | varsa sıralama; eksikse warning |
| `Restart` | restart politikası |
| `RestartSec` | planlı restart beklemesi |
| `StartLimitBurst` | restart üst sınırı |
| `TimeoutStartSec` | sync/pre/stop timeout’u |
| `User` | UID/user ve group geçişi |
| `WorkingDirectory` | child çalışma dizini |
| `Environment` | boşlukla ayrılmış `KEY=VALUE` değerleri |
| `CGroup` | özel cgroup alt adı |
| `MemoryLimit` | cgroup v2 `memory.max` değeri |
| `Nice` | child nice ayarı |
| `TTY` | getty/session sahibi süreçler için `yes/true` |

Komutlar doğrudan `execve` ile değil, rootfs’in `/bin/sh -c` kabuğu üzerinden
çalıştırılır. Bu mevcut servis profilindeki shell conditional’larını mümkün
kılar; servis dosyaları güvenilir root sahibi tarafından korunmalıdır.

## 5. Boot sırası

Yinit PID 1 olarak çalıştığında genel sıra şöyledir:

1. `--version` veya `--check` özel modları kontrol edilir.
2. PID’in 1 olup olmadığı doğrulanır.
3. `umask(022)` ayarlanır.
4. `/var/log/yinit.log` açılır.
5. Erken API filesystem’leri mount edilir.
6. `/dev/console` standart stream’lere bağlanır.
7. Sinyal handler’ları kurulup child subreaper etkinleştirilir.
8. cgroup v2 veya process-group fallback hazırlanır.
9. `/run/yinit/control.sock` root-only olarak açılır.
10. Servis dosyaları parse edilir ve sıralanır.
11. Servisler dependency sırasıyla başlatılır.
12. Ana loop child reaping, planlı restart, control socket ve reload olaylarını
    işler.
13. Shutdown sinyali veya control komutu gelirse ters sırada kapanış başlar.

## 6. Dosya bazında yapılanlar

### `src/yinit.c`

Ana PID 1 uygulaması kapsamlı biçimde yeniden düzenlendi:

- güvenli log buffer yönetimi
- path ve directory yardımcıları
- servis parser ve isim normalize etme
- dependency sort/cycle uyarıları
- mount hazırlığı
- device node fallback’i
- cgroup v2 setup/attach/kill/destroy
- passwd/group tabanlı kullanıcı geçişi
- process group, timeout ve exit status takibi
- oneshot/forking/simple supervision
- scheduled restart
- root-only UNIX datagram socket
- status/start/stop/restart/reload/reboot/poweroff
- ters sıra shutdown ve PID 1 koruması

### `src/yinit.h`

- Servis yapısı genişletildi.
- `exec[32]`, dependency, cgroup, user, environment, timers, PID/PGID ve
  restart zaman alanları eklendi.
- Standalone musl derlemesi için Linux reboot ABI sabitleri fallback olarak
  tanımlandı.
- Sınırlandırılmış string kopyalama yardımcıları kullanıldı.

### `src/yinitctl.c`

- Geçici client socket’i `umask(077)` ve `0600` ile oluşturulur.
- Socket cevabı için timeout vardır.
- Send/receive başarısızlığı artık exit code `1` döndürür.
- `version`, `status`, `start`, `stop`, `restart`, `reload`, `reboot` ve
  `poweroff` komutları bulunur.

### `Makefile`

- Host geliştirme derlemesi ile portable static derleme ayrıldı.
- `PREFIX`, `SBINDIR`, `SYSCONFDIR`, `MUSL_CC` override edilebilir.
- `make check` eklendi.
- `make musl` statik musl binary üretir.
- `make install-musl` musl binary’yi kurar.
- Normal `make install` mevcut `/sbin/init` symlink’ini değiştirmez.

### `etc/yinit/services/`

Gerçek minimal rootfs ve host binary farklarını tolere eden 10 servislik
başlangıç profili eklendi/düzeltildi. `01-filesystems` özellikle mount işinin
PID 1’e ait olduğunu açıkça ifade eden oneshot profildir.

### `tools/install-rootfs.sh`

Rootfs staging aracı:

```sh
./tools/install-rootfs.sh /path/to/rootfs
./tools/install-rootfs.sh /path/to/rootfs --make-default
```

İlk komut:

- `/sbin/yinit`
- `/usr/local/bin/yinitctl`
- `/etc/yinit/services/*.service`
- `/var/log`
- `/run/yinit`

oluşturur ve mevcut init’e dokunmaz.

`--make-default` açıkça verilirse mevcut `/sbin/init` dosyası
`/sbin/init.yinit-backup` olarak taşınır ve `init -> yinit` symlink’i oluşturulur.
İkinci kez backup’ı ezmemek için araç durur.

### `tests/qemu/smoke.sh`

İzole initramfs oluşturur, Yinit’i gerçek PID 1 olarak QEMU’da boot eder ve
`YINIT_QEMU_SMOKE_OK` marker’ını arar.

Desteklenen modlar:

```sh
./tests/qemu/smoke.sh ROOTFS KERNEL ./yinit
./tests/qemu/smoke.sh ROOTFS KERNEL ./yinit default
./tests/qemu/smoke.sh ROOTFS KERNEL ./yinit default handoff
```

`default` repository servis profilini ekler. `handoff`, Yinit’i doğrudan
initramfs `/init` yerine rootfs `/sbin/init` yolundan çalıştırır.

## 7. Derleme ve kurulum

Host üzerinde kontrol:

```sh
make
make check
./yinit --version
```

Minimal rootfs veya farklı x86-64 makineler için:

```sh
make MUSL_CC=/path/to/musl-gcc musl
file yinit
```

Beklenen sonuç `statically linked` bir ELF binary’dir. Fiziksel init olarak
host glibc ile üretilen static binary yerine musl binary kullanılması gerekir;
host glibc’nin CPU optimizasyonları farklı QEMU/PC modellerinde taşınabilirlik
sorunu çıkarabilir.

Rootfs’e güvenli staging:

```sh
./tools/install-rootfs.sh /path/to/rootfs
```

Varsayılan init’e açıkça geçirmek:

```sh
./tools/install-rootfs.sh /path/to/rootfs --make-default
```

Host’a sistem kurulumu için:

```sh
sudo make install
```

Bu komut `/sbin/yinit` kurar; `/sbin/init` linkini kendi başına değiştirmez.

## 8. Test matrisi ve sonuçlar

1. Konfigürasyon kontrolü:

   ```sh
   make check
   ```

   Sonuç: `Configuration OK: 10 services`

2. PID 1 dışı koruma:

   ```sh
   ./yinit
   ```

   Sonuç: exit code `2` ve güvenli ret mesajı.

3. Taşınabilir binary:

   ```sh
   make MUSL_CC=/home/yunustas/yunuslinux/build/musl/bin/musl-gcc musl
   ./yinit --version
   ```

   Sonuç: `Yinit 1.2`, statik musl ELF.

4. Minimal QEMU PID 1 smoke:

   `YINIT_QEMU_SMOKE_OK` marker’ı üretildi ve test `PASS` verdi.

5. Default servis profili:

   10 repository servisi ve smoke oneshot servisi yüklendi; TTY login ve DHCP
   akışı gözlendi.

6. `/sbin/init` handoff:

   Yinit rootfs init yolundan çalıştırıldı; mount, console, service start ve
   PID 1 yaşamı doğrulandı.

7. Staging install:

   `install-rootfs.sh` mevcut init’i koruma modu, backup’lı `--make-default`
   modu ve ikinci backup’ı ezmeme davranışıyla test edildi.

8. Kod/shell biçim kontrolleri:

   `git diff --check`, `sh -n tests/qemu/smoke.sh` ve
   `sh -n tools/install-rootfs.sh` temiz geçti.

## 9. Gerçek PC boot akışı

Yinit initramfs root diskini bulup `switch_root` yapan bir initramfs değildir.
Beklenen akış:

1. Kernel initramfs `/init` ile başlar.
2. Mevcut initramfs root diskini bulur ve mount eder.
3. Initramfs `switch_root /newroot /sbin/init` yapar.
4. Rootfs içindeki `/sbin/init` Yinit’e işaret eder.
5. Yinit PID 1 olarak mount, servis ve konsol sürecini devralır.

İlk gerçek donanım testi:

- ayrı USB/SSD veya sanal disk kullanmalı,
- mevcut GRUB girdisini korumalı,
- Yinit için ayrı ve geri dönüşlü bir boot girdisi taşımalı,
- ilk beklentiyi TTY/seri login olarak belirlemeli,
- grafik masaüstünü sonraki servis entegrasyonu olarak ele almalıdır.

YunusLinux rollout prosedürü:

```sh
make MUSL_CC=/home/yunustas/yunuslinux/build/musl/bin/musl-gcc musl
./yinit --check ./etc/yinit/services
./tools/install-rootfs.sh \
  /home/yunustas/yunuslinux/build/rootfs \
  --make-default
```

Eski init şu dosyada kalır:

```text
/home/yunustas/yunuslinux/build/rootfs/sbin/init.yinit-backup
```

## 10. Bilinçli sınırlamalar ve riskler

- Bu çalışma kapsamında gerçek fiziksel PC üzerinde boot testi yapılmadı; QEMU
  gerçek PID 1 ve handoff testleri yapıldı.
- Yinit tam systemd replacement değildir; systemd unit parser’ı yoktur.
- `Type=notify` gerçek `READY=1` readiness protokolü uygulamaz.
- `Type=forking` daemon’larının tüm alt süreçlerini en güvenilir biçimde takip
  etmek için cgroup v2 önerilir. Eski kernel’de daemon kendi session’ına
  ayrılırsa process-group fallback’i her child’ı yakalayamayabilir.
- cgroup v2 yoksa bellek limiti ve tam cgroup izolasyonu uygulanamaz; process
  group temizliği kullanılır.
- Grafik masaüstü, display manager, GPU oturumu, polkit ve tam logind session
  yönetimi varsayılan profilde garanti edilmez.
- Network interface adı ve DHCP programı hedef rootfs/donanıma bağlıdır.
- Servis komutları `/bin/sh -c` üzerinden çalıştığı için servis dosyalarının
  root tarafından yazılabilir olması gerekir.
- Rootfs’teki gerçek binary yolları ve initramfs handoff’u fiziksel kurulumdan
  önce doğrulanmalıdır.
- `--make-default` açıkça verilmeden kurulum mevcut init’i değiştirmez; açıkça
  verildiğinde backup dosyası mutlaka korunmalıdır.

## 11. Kısa sonuç

Yinit 1.2’nin ana kazanımı yalnızca yeni servis alanları değildir. Eski
prototipte sessizce göz ardı edilen hata sonuçları artık servis state’ine ve
loglara yansır; child süreçler daha sistematik temizlenir; PID 1 ana döngüsü
restart beklemeleriyle kilitlenmez; rootfs ve initramfs handoff’u için güvenli
bir rollout yolu vardır; bütün bunlar QEMU’da tekrar edilebilir testlerle
kontrol edilir.

Bu nedenle Yinit 1.2, minimal/özel Linux dağıtımlarında kontrollü konsol boot’u
için kullanılabilir bir temel sürümdür. Her donanım ve grafik masaüstü için
tak-çalıştır systemd alternatifi olarak değerlendirilmemelidir.
