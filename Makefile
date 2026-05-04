CC = clang
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -O2 -Wno-unused-parameter -Wno-switch -Wno-missing-field-initializers -Wno-sign-compare
LDFLAGS =

# Common sources: everything except the preprocessor implementations.
# Phase 2 dual-build: ncc links src/preprocess.c (chibicc lineage),
# ncc-v2 links src/preprocess_v2.c (spec-derived per docs/specs/02_preprocessor.md).
# Both share all other src/*.c and cc.h.  When validate_preprocessor.sh
# ncc-v2 ncc passes PASS=N/N, the swap-in step makes preprocess_v2.c
# canonical and drops the dual-build.

COMMON_SRCS = $(filter-out src/preprocess.c src/preprocess_v2.c, $(wildcard src/*.c))
COMMON_OBJS = $(COMMON_SRCS:.c=.o)

ncc: $(COMMON_OBJS) src/preprocess.o
	$(CC) $(LDFLAGS) -o $@ $^

ncc-v2: $(COMMON_OBJS) src/preprocess_v2.o
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
