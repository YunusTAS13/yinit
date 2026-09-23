# Fiziksel PC rollout

Bu akış mevcut host makinenin `/sbin/init` dosyasını veya bootloader ayarını
değiştirmeden Yinit’i bir YunusLinux rootfs’ine yerleştirir.

## 1. Artefact üret

```sh
cd /home/yunustas/yinit
make MUSL_CC=/home/yunustas/yunuslinux/build/musl/bin/musl-gcc musl
./yinit --check ./etc/yinit/services
```

## 2. Rootfs’i stage et

Önce YunusLinux rootfs’ini oluştur. Ardından rootfs’i açıkça seçerek Yinit’i
varsayılan init yap:

```sh
./tools/install-rootfs.sh \
  /home/yunustas/yunuslinux/build/rootfs \
  --make-default
```

Script eski init’i şu dosyada bırakır:
`/home/yunustas/yunuslinux/build/rootfs/sbin/init.yinit-backup`.

## 3. Önce sanal makinede aç

```sh
./tests/qemu/smoke.sh \
  /home/yunustas/yunuslinux/build/rootfs \
  /home/yunustas/yunuslinux/build/iso/boot/vmlinuz \
  ./yinit default
```

Konsolda `YINIT_QEMU_SMOKE_OK` ve `yinit login:` görülmeli. Bu test yalnızca
geçerse fiziksel cihaza geç.

## 4. Fiziksel boot

İlk denemede ayrı bir USB/SSD veya geri dönüşü hazır bir GRUB girdisi kullan.
Kernel komut satırında mevcut initramfs korunur; Yinit rootfs’e geçildikten
sonra `/sbin/init` olarak çalışır. GRUB girdisi mevcut girdinin kopyası olup
yalnızca ayrı bir adla seçilebilir durumda kalmalıdır.

İlk açılışta beklenen sonuç grafik masaüstü değil, seri/TTY login ekranıdır.
`yinitctl status` ile servisleri kontrol et. Ağ, dbus, logind ve grafik oturumu
rootfs’teki gerçek binary ve donanım sürücülerine bağlıdır.

## Geri dönüş

Yinit açılmazsa eski boot girdisini seç. Rootfs üzerinde çalışıyorsan:

```sh
rm /path/to/rootfs/sbin/init
mv /path/to/rootfs/sbin/init.yinit-backup /path/to/rootfs/sbin/init
```

Bu geri dönüş yalnızca açıkça `--make-default` ile oluşturulmuş backup varsa
yapılmalıdır.
