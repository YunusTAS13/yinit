# Yinit 1.2 — fiziksel PC PID 1 hardening release

Yinit 1.2, küçük bir Linux rootfs’inde yalnızca “bir process başlatan init”
olmaktan çıkıp, fiziksel PC boot sürecinde kontrollü biçimde kullanılabilecek
bir PID 1 ve servis yöneticisi haline getirilen sürümdür.

Bu sürüm systemd uyumluluğu iddiasında değildir. Hedef; YunusLinux gibi küçük
bir dağıtımda boot, servis yaşam döngüsü, konsol login’i, kapanış ve hata
durumlarını denetlenebilir tutmaktır.

## Önceki durum ve giderilen hatalar

1.2’den önceki kodda aşağıdaki sorunlar vardı:

- Birden fazla `ExecStart` satırından yalnızca ilk komut çalışıyordu.
- `User=` parse ediliyor fakat servis gerçekten o kullanıcıya geçirilmeden
  root olarak çalışıyordu.
- Supplementary group, çalışma dizini, environment ve nice ayarları eksik veya
  etkisizdi.
- Servis adları `.service` uzantısıyla ve uzantısız farklı tutulduğu için
  `After`/`Requires` bağımlılıkları eşleşmeyebiliyordu.
- Bağımlılık sırası deterministik değildi; döngüler güvenilir biçimde raporlanmıyordu.
- cgroup kurulumu yapılmıyor veya cgroup hataları sessizce yutuluyordu.
- Servis durdurulurken yalnızca ana PID’e sinyal gönderiliyor, child process’ler
  geride kalabiliyordu.
- Restart beklemesi PID 1 ana döngüsünü `sleep()` ile bloke edebiliyordu.
- `Type=oneshot`, `Type=forking`, `Type=notify` ve timeout davranışları eksikti.
- `ExecStartPre`, `ExecStartPost`, `ExecStop` ve oneshot dönüş kodları yeterince
  denetlenmiyordu.
- Control socket herkese açık izinlerle oluşturulabiliyordu.
- `yinitctl`, başarısız socket işlemlerinde yanlış başarı kodu döndürebiliyordu.
- Getty gibi programların kendi `setsid()` davranışı process-group yönetimiyle
  çakışabiliyordu.
- Init mount noktaları, device node’ları ve `/run` hazırlığı rootfs’e göre
  güvenilir değildi.
- Proje için `--check` doğrulaması ve gerçek PID 1 QEMU testi yoktu.
- Host glibc ile üretilen statik binary, farklı CPU özelliklerine sahip QEMU veya
  PC’lerde taşınabilir olmayabiliyordu.
- Fiziksel rootfs’e kurulum, initramfs handoff’u ve geri dönüş prosedürü
  belgelenmemişti.

## 1.2’de yapılan düzeltmeler

### PID 1 ve boot güvenliği

- Yinit, PID 1 dışında çalıştırılırsa kendisini durdurur.
- `--check` normal kullanıcı olarak servis dosyalarını mount yapmadan doğrular.
- `/proc`, `/sys`, `/dev`, `/run`, `/tmp` ve `/dev/pts` gerekli olduğunda
  hazırlanır; initramfs’in zaten mount ettiği noktalar tekrar zorlanmaz.
- `/dev/null`, `/dev/zero`, `/dev/tty`, `/dev/console`, `/dev/random`,
  `/dev/urandom` ve `/dev/ptmx` için gerekli fallback node’ları oluşturulur.
- `/dev/console` üzerinden PID 1’in standart giriş/çıkışı hazırlanır.
- `PR_SET_CHILD_SUBREAPER` ve `SIGCHLD` reaping ile orphan süreçler mümkün
  olduğunca Yinit’e geri bağlanır.

### Servis tanımı ve bağımlılıklar

- `ExecStart` birden fazla kez yazılabilir.
- `Type=oneshot` bütün komutları sırayla çalıştırır ve dönüş kodlarını kontrol
  eder.
- Uzun çalışan servislerde ilk `ExecStart` ana süreç olarak izlenir.
- `Type=simple`, `forking`, `oneshot`, `notify` ve `idle` tanınır.
- `notify` şu an gerçek `READY=1` protokolü yerine güvenli simple-supervision
  fallback’i kullanır; bu bilinçli bir sınırlamadır.
- `After`, `Requires` ve `Wants` servis adlarını `.service` uzantısıyla veya
  uzantısız yazabilmeyi destekler.
- Servisler deterministik sıraya sokulur; eksik bağımlılıklar ve döngüler loglanır.
- `Restart=no`, `always`, `on-failure`, `on-abort`, `RestartSec` ve
  `StartLimitBurst` işlenir.
- Restart beklemesi ana PID 1 döngüsünü bloke etmez; yeniden başlatma zamanı
  planlanır ve ana döngü servisleri zamanı gelince başlatır.
- `TimeoutStartSec` timeout’larında process group/cgroup güvenli biçimde
  sonlandırılır.

### Kullanıcı, kaynak ve süreç izolasyonu

- `User=` kullanıcı adı veya UID ile uygulanır.
- Primary ve supplementary gruplar `/etc/passwd` ve `/etc/group` üzerinden
  hazırlanır.
- `WorkingDirectory`, `Environment`, `Nice` ve `MemoryLimit` desteklenir.
- Servisler ayrı process group içinde başlatılır.
- cgroup v2 varsa servis başına cgroup oluşturulur, process’ler cgroup’a alınır
  ve bellek limiti uygulanabilir.
- cgroup v2 yoksa process-group temizliğiyle çalışan fallback kullanılır.
- cgroup servis isimleri path traversal’a karşı doğrulanır.
- Stop ve shutdown sırasında süreçler ters sırada kapatılır; önce graceful sinyal,
  timeout sonrası kill uygulanır.

### TTY, login ve temel servis profili

- `TTY=yes`, getty’nin kendi session/process-group kurulumunu bozmaz.
- TTY1 ve seri konsol için getty servisleri eklendi.
- udev, dbus, logind, loopback ve DHCP servisleri hedef rootfs’te binary varsa
  çalışır; yoksa küçük rootfs boot’unu gereksiz yere durdurmaz.
- YunusLinux `yunus-network` ve `udhcpc` script’i varsa kullanılır.
- Network interface yoksa DHCP servisi boot’u restart döngüsüne sokmadan çıkar.

### Kontrol socket’i ve kapanış

- `/run/yinit/control.sock` yalnızca root erişimine açık `0600` izinle oluşturulur.
- `status`, `start`, `stop`, `restart`, `reload`, `reboot`, `poweroff` ve
  `version` komutları bulunur.
- Servis isimlerinde numaralı profile (`07-network`) ek olarak kısa isim
  (`network`) kullanılabilir.
- Eksik servis adı, bilinmeyen servis ve boş komutlar hata cevabı verir.
- `yinitctl` socket hatasında sıfır dışı dönüş kodu verir.
- Shutdown sırasında servisler durdurulur, kalan PID 1 child’ları temizlenir,
  `sync()` çağrılır ve ardından reboot/poweroff istenir.

### Taşınabilir binary ve kurulum

- Host geliştirme derlemesi ile minimal rootfs derlemesi ayrıldı.
- `make MUSL_CC=/path/to/musl-gcc musl` statik ve taşınabilir binary üretir.
- `install-musl` portable binary’yi staging rootfs’e kurabilir.
- `tools/install-rootfs.sh`, Yinit, `yinitctl` ve servis profilini rootfs’e taşır.
- `--make-default` açıkça verilirse eski `/sbin/init`,
  `/sbin/init.yinit-backup` olarak korunur ve `/sbin/init -> yinit` oluşturulur.
- Varsayılan kurulum host `/sbin/init` symlink’ini kendiliğinden değiştirmez.

### Test ve dokümantasyon

- `make check` servis yapılandırmasını doğrular.
- QEMU testi Yinit’i gerçek PID 1 olarak boot eder.
- Smoke testi mount, console, oneshot servis ve PID 1 yaşamını doğrular.
- Default profil testi getty, servis sırası ve DHCP akışını doğrular.
- `handoff` testi rootfs `/sbin/init` yolunu, initramfs sonrası beklenen giriş
  biçimiyle doğrular.
- Fiziksel PC rollout, GRUB fallback ve geri dönüş prosedürü
  [`PHYSICAL_BOOT.md`](PHYSICAL_BOOT.md) içinde anlatılır.

## Doğrulama sonucu

Bu sürümün çalışma ağacında ve izole QEMU’da aşağıdaki kontroller geçmiştir:

```sh
make check
make MUSL_CC=/home/yunustas/yunuslinux/build/musl/bin/musl-gcc musl
./yinit --version
./tests/qemu/smoke.sh ROOTFS KERNEL ./yinit
./tests/qemu/smoke.sh ROOTFS KERNEL ./yinit default
./tests/qemu/smoke.sh ROOTFS KERNEL ./yinit default handoff
```

Beklenen sürüm çıktısı `Yinit 1.2`, smoke çıktısı ise
`Yinit QEMU PID 1 smoke test: PASS` ve `YINIT_QEMU_SMOKE_OK` satırlarıdır.

## Bilinçli olarak devam eden sınırlamalar

- Gerçek fiziksel donanım üzerinde bu çalışma kapsamında boot testi yapılmadı;
  ilk deneme ayrı USB/SSD veya geri dönüşlü GRUB girdisiyle yapılmalıdır.
- Yinit tam bir systemd alternatifi değildir; systemd unit uyumluluğu yoktur.
- `Type=notify` gerçek readiness socket/protokolü uygulamaz.
- `Type=forking` servislerde cgroup v2, daemon’ın tüm child süreçlerini takip
  etmek için tercih edilir; cgroup olmayan eski kernel’lerde daemon’ın kendi
  session’ına ayrılması mümkün olabilir.
- Yinit root diskini bulup `switch_root` yapan initramfs değildir; bu işi mevcut
  initramfs yapar ve `/sbin/init` handoff’u sonrasında Yinit çalışır.
- Varsayılan profil konsol/TTY boot içindir. KDE veya başka grafik masaüstü için
  display manager, session bus, GPU sürücüsü ve ilgili servisler ayrıca
  tanımlanmalıdır.
- Servis dosyalarındaki binary yolları hedef rootfs’e göre gözden geçirilmelidir.

## Sürüm özeti

Yinit 1.2’nin amacı “her Linux dağıtımında hiçbir ayar gerektirmeden systemd’nin
yerine geçmek” değil; küçük bir Linux sisteminde PID 1 davranışını görünür,
test edilebilir ve geri dönüşü mümkün hale getirmektir.
