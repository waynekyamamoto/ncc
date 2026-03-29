# ncc

A C compiler targeting ARM64 (Apple Silicon) macOS, written in C.

## Features

- ~9000 lines of C across 9 source files
- Compiles C11 with common GCC extensions
- ARM64 code generation (Apple Silicon / macOS)
- Preprocessor with macro expansion, `#include`, conditionals
- Struct/union, bitfields, enums, typedefs
- Function pointers, variadic functions, compound literals
- GCC builtins: `__builtin_clz`, `__builtin_ctz`, `__builtin_ffs`, `__builtin_popcount`, `__builtin_bswap32/64`, `__builtin_alloca`, and more
- `__attribute__((packed))`, `__attribute__((aligned))`, `__attribute__((section))`

## Building

```
make
```

Requires clang and macOS command line tools for linking.

## Usage

```
# Compile a C file
./ncc -o hello hello.c

# Compile and link multiple files
./ncc -o program file1.c file2.c -lm

# Generate assembly
./ncc -S -o output.s input.c

# Compile to object file
./ncc -c -o output.o input.c
```

## Test Results

| Suite | Pass Rate | Details |
|-------|-----------|---------|
| Compliance | 15/15 (100%) | Basic C feature tests |
| GCC Torture | 852/995 (85.6%) | GCC execute test suite |
| DOOM | Compiles and runs | doomgeneric port with graphics, sound, music |
| SQLite | Compiles, queries work | SELECT queries pass; CREATE TABLE has a known issue |
| Lua | 30/33 files compile | Minor preprocessor gaps remain |
| Self-hosting | Yes | ncc can compile itself |

Run tests:
```
# Compliance tests
cd tests/compliance && bash run.sh

# GCC torture tests
cd tests/torture && bash run.sh --summary
```

## Architecture

| File | Purpose |
|------|---------|
| `src/main.c` | Entry point, argument parsing |
| `src/tokenize.c` | Lexer |
| `src/preprocess.c` | C preprocessor |
| `src/parse.c` | Parser, AST construction, type checking |
| `src/type.c` | Type system, integer promotions |
| `src/codegen_arm64.c` | ARM64 code generation |
| `src/cc.h` | Shared declarations |
| `src/alloc.c` | Memory allocation |
| `src/unicode.c` | Unicode support |
| `src/hashmap.c` | Hash map implementation |

## License

The DOOM test program (`tests/doom/`) is from [doomgeneric](https://github.com/ozkl/doomgeneric) and is licensed under GPLv2.
