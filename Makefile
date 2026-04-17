# SPDX-License-Identifier: BSD-3-Clause

APP_SERVER = server
APP_CLIENT = client

# Default target
all: $(APP_SERVER) $(APP_CLIENT)

PKGCONF ?= pkg-config

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)

$(APP_SERVER): src/server.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_SHARED)

$(APP_CLIENT): src/client.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_SHARED)

clean:
	rm -f $(APP_SERVER) $(APP_CLIENT)

.PHONY: all clean