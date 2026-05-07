CC ?= clang
UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -g -O2 \
         -Wno-unused-parameter -Wno-switch -Wno-missing-field-initializers \
         -Wno-sign-compare -Wno-format -Wno-empty-body \
         -Wno-unused-but-set-variable
else
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -O2 \
         -Wno-unused-parameter -Wno-switch -Wno-missing-field-initializers \
         -Wno-sign-compare
endif
LDFLAGS =

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

ncc: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c src/cc.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f ncc src/*.o

test: ncc
	@echo "=== Basic test ==="
	echo 'int main() { return 42; }' > /tmp/test_ncc.c
	./ncc -S -o /tmp/test_ncc.s /tmp/test_ncc.c
	as -o /tmp/test_ncc.o /tmp/test_ncc.s
	ld -o /tmp/test_ncc -lSystem -syslibroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk -arch arm64 -e _main /tmp/test_ncc.o
	/tmp/test_ncc; test $$? -eq 42 && echo "PASS: return 42" || echo "FAIL"
	@echo "=== Hello world test ==="
	echo '#include <stdio.h>\nint main() { printf("Hello, world!\\n"); return 0; }' > /tmp/test_hello.c
	./ncc -o /tmp/test_hello /tmp/test_hello.c
	/tmp/test_hello

# --------------------------------------------------------------------------
# NetBSD/aarch64 kernel build with ncc.
# See tests/netbsd/README.md for a quickstart.
#
# Two-step flow (same on Linux and macOS):
#   make netbsd-tools   # one-time, ~30 min — builds the gcc cross-toolchain
#   make netbsd         # builds the kernel with ncc (most kernel C files)
#
# Override defaults via env:
#   NETBSD_DIR=/path/to/netbsd        (default ~/netbsd)
#   NETBSD_KERNEL=GENERIC64           (default MINIMAL_VIRT64)
# --------------------------------------------------------------------------

NETBSD_DIR    ?= $(HOME)/netbsd
NETBSD_KERNEL ?= MINIMAL_VIRT64

netbsd-tools:
	@NETBSD_DIR=$(NETBSD_DIR) bash tests/netbsd/tools/build-tools.sh

netbsd:
	@NETBSD_DIR=$(NETBSD_DIR) bash tests/netbsd/build.sh $(NETBSD_KERNEL)

netbsd-boot:
	@bash tests/netbsd/tools/boot-test.sh \
	  $(NETBSD_DIR)/obj/sys/arch/evbarm/compile/$(NETBSD_KERNEL)/netbsd.img

netbsd-clean:
	rm -rf $(NETBSD_DIR)/obj/sys/arch/evbarm/compile/$(NETBSD_KERNEL)

.PHONY: clean test netbsd netbsd-tools netbsd-boot netbsd-clean
