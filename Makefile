PREFIX      ?= /usr/local
CC          ?= cc

X11CFLAGS   := $(shell pkg-config --cflags x11 xinerama 2>/dev/null)
X11LIBS     := $(shell pkg-config --libs x11 xinerama 2>/dev/null)
X11CFLAGS   ?= -I/usr/include
X11LIBS     ?= -L/usr/lib -lX11 -lXinerama

CFLAGS      += -O2 -Wall -Wextra $(X11CFLAGS)
LDFLAGS     += $(X11LIBS)

SRC         = dswm.c layout.c workspace.c events.c ewmh.c util.c
OBJ         = $(SRC:.c=.o)

all: dswm dswm-session

dswm: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c dswm.h
	$(CC) $(CFLAGS) -c -o $@ $<

dswm-session: dswm-session.c
	$(CC) $(CFLAGS) -o $@ dswm-session.c $(LDFLAGS)

clean:
	rm -f dswm dswm-session $(OBJ)
	rm -rf pkg/ src/
	rm -f dswm-git-*.pkg.tar.zst

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 dswm $(DESTDIR)$(PREFIX)/bin/dswm
	install -m 755 dswm-session $(DESTDIR)$(PREFIX)/bin/dswm-session
	install -d $(DESTDIR)$(PREFIX)/share/xsessions
	install -m 644 dswm.desktop $(DESTDIR)$(PREFIX)/share/xsessions/dswm.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dswm
	rm -f $(DESTDIR)$(PREFIX)/bin/dswm-session
	rm -f $(DESTDIR)$(PREFIX)/share/xsessions/dswm.desktop

.PHONY: all clean install uninstall
