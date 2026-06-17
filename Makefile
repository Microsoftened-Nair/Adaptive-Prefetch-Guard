# Makefile for Adaptive Prefetch Guard (apg)
#
# Build:  make            # optimized release build
#         make debug      # -g -O0 -fsanitize=address,undefined
#         make clean

CC       ?= cc
CFLAGS   ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
LDFLAGS  ?=
LDLIBS   ?=

# Optional: link against libm for fabs() etc. if the platform needs it.
LDLIBS   += -lm

OBJS     = apg.o
BIN      = apg

.PHONY: all clean debug install uninstall

all: $(BIN)

$(BIN): apg.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

debug: CFLAGS = -std=gnu11 -g -O0 -Wall -Wextra -Wpedantic -D_GNU_SOURCE \
                 -fsanitize=address,undefined -fno-omit-frame-pointer
debug: LDFLAGS = -fsanitize=address,undefined
debug: $(BIN)
	@echo "Built debug binary: ./$(BIN)"

# Static-analysis sanity check (does not produce a binary)
analyze:
	$(CC) $(CFLAGS) -c apg.c -o /dev/null
	@echo "Static analysis passed."

install: $(BIN)
	install -D -m 0755 $(BIN)       $(DESTDIR)/usr/sbin/apg
	install -D -m 0644 apg.service  $(DESTDIR)/etc/systemd/system/apg.service
	@echo "Installed. Enable with: systemctl enable --now apg"

uninstall:
	rm -f $(DESTDIR)/usr/sbin/apg
	rm -f $(DESTDIR)/etc/systemd/system/apg.service
	@echo "Uninstalled."

clean:
	rm -f $(BIN) $(OBJS)
