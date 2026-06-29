# Makefile — l2fwd_dr (l2fwd com Dual Receive Ring, TCC)
# Compila contra o DPDK do sistema via pkg-config.
# No CloudLab (DPDK compilado da fonte):
#   export PKG_CONFIG_PATH=~/dpdk/build/meson-private

APP = l2fwd_dr
SRC = main.c

PKGCONF ?= pkg-config

CFLAGS  += -O3 -g -Wall -Wextra $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS += $(shell $(PKGCONF) --libs libdpdk)

all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(APP) $(LDFLAGS)

clean:
	rm -f $(APP)

.PHONY: all clean
