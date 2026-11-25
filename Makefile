# SPDX-License-Identifier: MIT
ifneq ($(KERNELRELEASE),)
obj-m := khttpd.o
khttpd-y := main.o http_server.o http_protocol.o http_connection.o
else
.DEFAULT_GOAL := all
KDIR ?= /lib/modules/$(shell uname -r)/build
CC ?= cc
USER_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -pthread
CORE := http_protocol.c http_connection.c
HEADERS := http_protocol.h http_connection.h
.PHONY: all module userspace test check sanitize integration clean
all: module userspace
module:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" W=1 modules
build:
	mkdir -p $@
build/reference_server: tools/reference_server.c $(CORE) $(HEADERS) | build
	$(CC) $(USER_CFLAGS) $(CFLAGS) -o $@ tools/reference_server.c $(CORE) $(LDFLAGS)
build/unit: tests/unit.c $(CORE) $(HEADERS) | build
	$(CC) $(USER_CFLAGS) $(CFLAGS) -o $@ tests/unit.c $(CORE) $(LDFLAGS)
build/integration: tests/integration.c | build
	$(CC) $(USER_CFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)
build/legacy_startup: tests/legacy_startup.c | build
	$(CC) $(USER_CFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)
userspace: build/reference_server build/unit build/integration build/legacy_startup
test check: userspace
	./build/unit
	./scripts/test-userspace.sh
	python3 -m unittest discover -s tests -p 'test_*.py' -v
sanitize: | build
	$(CC) $(USER_CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o build/unit-sanitize tests/unit.c $(CORE)
	ASAN_OPTIONS=detect_leaks=1 ./build/unit-sanitize
integration: build/integration
	./build/integration 8081
clean:
	$(RM) -r build
	$(RM) *.o *.ko *.mod *.mod.c Module.symvers modules.order .*.cmd .*.d
endif
