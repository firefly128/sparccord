# SPARCcord Makefile for Solaris 7 / CDE / SPARCstation
#
# Discord client for SPARC. Talks to discord-bridge server via HTTP/1.0.
# Requires: Motif (libXm), X11 (libX11, libXt), Solaris sockets
# These are standard on Solaris 7 with CDE installed.

# Use tgcware GCC if available, fall back to system cc
CC = /usr/tgcware/gcc47/bin/gcc
#CC = cc

CFLAGS = -O2 -I/usr/dt/include -I/usr/openwin/include -Wall
LDFLAGS = -L/usr/dt/lib -L/usr/openwin/lib -R/usr/dt/lib -R/usr/openwin/lib
LIBS = -lXm -lXt -lX11 -lsocket -lnsl -lm

TARGET = sparccord
SRCS = sparccord.c http.c json.c gifload.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

.c.o:
	$(CC) $(CFLAGS) -c $<

sparccord.o: sparccord.c http.h json.h gifload.h
http.o: http.c http.h
json.o: json.c json.h
gifload.o: gifload.c gifload.h

clean:
	rm -f $(TARGET) $(OBJS)

install: $(TARGET)
	@echo "Installing SPARCcord..."
	mkdir -p /opt/sparccord/bin 2>/dev/null || true
	cp $(TARGET) /opt/sparccord/bin/sparccord
	@echo "Done! Configure ~/.sparccordrc and run /opt/sparccord/bin/sparccord"

run: $(TARGET)
	DISPLAY=:0 ./$(TARGET)

.PHONY: all clean install run
