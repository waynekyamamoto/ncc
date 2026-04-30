#!/bin/bash
# Build sqlite3.c + test_sqlite.c with ncc2 and run the harness.
# Defaults to ../../ncc2; override with NCC=...
#
# Exits 0 if the test binary returns 0 (all CHECKs passed).

set -e
cd "$(dirname "$0")"

NCC="${NCC:-../../ncc2}"
OUT="test_sqlite_run"

rm -f sqlite3.o test_sqlite.o "$OUT"

# SQLITE_MEMORY_BARRIER=<expr> overrides sqlite3MemoryBarrier()'s body.
# When undefined, sqlite falls back to __sync_synchronize() — a GCC/clang
# atomic-fence builtin that ncc does not implement, which fails at link
# time with `___sync_synchronize` undefined. Setting the macro to nothing
# makes the body `;` — safe for the single-threaded test harness.
# (Multi-threaded sqlite usage would need a real barrier here.)
echo "compiling sqlite3.c with $NCC (~256k lines, takes a moment)"
"$NCC" -c -DSQLITE_MEMORY_BARRIER= -o sqlite3.o sqlite3.c

echo "compiling test_sqlite.c"
"$NCC" -c -o test_sqlite.o test_sqlite.c

echo "linking $OUT"
"$NCC" -o "$OUT" test_sqlite.o sqlite3.o

echo "running $OUT"
./"$OUT"
