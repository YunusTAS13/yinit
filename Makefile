CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=gnu11 -O2
# The default build is for host-side development.  Use `make musl` for the
# static artifact that will live in a minimal rootfs or initramfs.
LDFLAGS ?=
PREFIX ?= /usr/local
SBINDIR ?= /sbin
SYSCONFDIR ?= /etc
MUSL_CC ?= musl-gcc
SERVICES = $(wildcard etc/yinit/services/*.service)

all: yinit yinitctl

yinit: src/yinit.c src/yinit.h
	$(CC) $(CFLAGS) -o $@ src/yinit.c $(LDFLAGS)

yinitctl: src/yinitctl.c src/yinit.h
	$(CC) $(CFLAGS) -o $@ src/yinitctl.c $(LDFLAGS)

clean:
	rm -f yinit yinitctl

check: all
	./yinit --check ./etc/yinit/services

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(SYSCONFDIR)/yinit/services
	install -d $(DESTDIR)$(SYSCONFDIR)/yinit/targets
	install -d $(DESTDIR)/run/yinit
	install -m 755 yinit $(DESTDIR)$(SBINDIR)/yinit
	install -m 755 yinitctl $(DESTDIR)$(PREFIX)/bin/yinitctl
	@for f in $(SERVICES); do \
		install -m 644 $$f $(DESTDIR)$(SYSCONFDIR)/yinit/services/; \
	done

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/yinit
	rm -f $(DESTDIR)$(PREFIX)/bin/yinitctl
	rm -rf $(DESTDIR)$(SYSCONFDIR)/yinit

.PHONY: all clean check install uninstall

.PHONY: musl
musl:
	$(MAKE) clean
	$(MAKE) CC="$(MUSL_CC)" CFLAGS="$(CFLAGS)" LDFLAGS="-static" all

.PHONY: install-musl
install-musl:
	$(MAKE) musl
	$(MAKE) CC="$(MUSL_CC)" CFLAGS="$(CFLAGS)" LDFLAGS="-static" install
