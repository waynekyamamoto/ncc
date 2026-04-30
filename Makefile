CC = clang
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -O2 -Wno-unused-parameter -Wno-switch -Wno-missing-field-initializers -Wno-sign-compare
LDFLAGS =

# Default ncc uses src/tokenize.c. The Phase-1 swap-out candidate is
# src/tokenize_v2.c — it is excluded from the default build so the two
# do not collide on duplicate symbols. `make ncc-v2` builds an alt
# binary with tokenize_v2.c instead.
SRCS_DEFAULT = $(filter-out src/tokenize_v2.c, $(wildcard src/*.c))
SRCS_V2      = $(filter-out src/tokenize.c,    $(wildcard src/*.c))

OBJS_DEFAULT = $(SRCS_DEFAULT:.c=.o)
OBJS_V2      = $(SRCS_V2:.c=.o)

ncc: $(OBJS_DEFAULT)
	$(CC) $(LDFLAGS) -o $@ $^

ncc-v2: $(OBJS_V2)
	$(CC) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c src/cc.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f ncc ncc-v2 src/*.o

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

.PHONY: clean test
