# Makefile — dual_ring_fwd (MVP 1)
# Compila contra o DPDK do sistema via pkg-config (mesmo padrão dos exemplos DPDK).
# No CloudLab (DPDK compilado da fonte): export PKG_CONFIG_PATH=~/dpdk/build/meson-private (ou lib/pkgconfig do install)

APP = dual_ring_fwd
SRC = dual_ring_fwd.c

PKGCONF ?= pkg-config

CFLAGS  += -O3 -g -Wall -Wextra $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS += $(shell $(PKGCONF) --libs libdpdk) -lpthread

all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(APP) $(LDFLAGS)

clean:
	rm -f $(APP)

.PHONY: all clean
