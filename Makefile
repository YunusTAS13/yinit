CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O2
LDFLAGS = -static
PREFIX = /usr/local
SERVICES = $(wildcard etc/yinit/services/*.service)

all: yinit yinitctl

yinit: src/yinit.c src/yinit.h
	$(CC) $(CFLAGS) -o $@ src/yinit.c $(LDFLAGS)

yinitctl: src/yinitctl.c src/yinit.h
	$(CC) $(CFLAGS) -o $@ src/yinitctl.c $(LDFLAGS)

clean:
	rm -f yinit yinitctl

install: all
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)/etc/yinit/services
	install -d $(DESTDIR)/etc/yinit/targets
	install -d $(DESTDIR)/run/yinit
	install -m 755 yinit $(DESTDIR)$(PREFIX)/sbin/yinit
	install -m 755 yinitctl $(DESTDIR)$(PREFIX)/bin/yinitctl
	@for f in $(SERVICES); do \
		install -m 644 $$f $(DESTDIR)/etc/yinit/services/; \
	done

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/yinit
	rm -f $(DESTDIR)$(PREFIX)/bin/yinitctl
	rm -rf $(DESTDIR)/etc/yinit

.PHONY: all clean install uninstall
