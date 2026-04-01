#!/bin/bash
# Build CPython 3.12.3 entirely with ncc
set -e

NCC=/Users/yamamoto/new_compiler/ncc
PYDIR=/tmp/Python-3.12.3
OUTDIR=/tmp/cpython_ncc_build
CFLAGS="-DPy_BUILD_CORE -DNDEBUG -I$PYDIR -I$PYDIR/Include -I$PYDIR/Include/internal"

mkdir -p $OUTDIR

# All core library .c files (LIBRARY_OBJS from Makefile)
FILES=(
  # MODULE_OBJS
  Modules/config.c Modules/main.c Modules/gcmodule.c
  # MODOBJS (builtin modules)
  Modules/atexitmodule.c Modules/faulthandler.c Modules/posixmodule.c
  Modules/signalmodule.c Modules/_tracemalloc.c Modules/_codecsmodule.c
  Modules/_collectionsmodule.c Modules/errnomodule.c
  Modules/_io/_iomodule.c Modules/_io/iobase.c Modules/_io/fileio.c
  Modules/_io/bufferedio.c Modules/_io/textio.c Modules/_io/bytesio.c
  Modules/_io/stringio.c
  Modules/itertoolsmodule.c Modules/_sre/sre.c Modules/_threadmodule.c
  Modules/timemodule.c Modules/_typingmodule.c Modules/_weakref.c
  Modules/_abc.c Modules/_functoolsmodule.c Modules/_localemodule.c
  Modules/_operator.c Modules/_stat.c Modules/symtablemodule.c
  Modules/pwdmodule.c
  # PARSER_OBJS
  Parser/token.c Parser/pegen.c Parser/pegen_errors.c
  Parser/action_helpers.c Parser/parser.c Parser/string_parser.c
  Parser/peg_api.c Parser/myreadline.c Parser/tokenizer.c
  # PYTHON_OBJS
  Python/_warnings.c Python/Python-ast.c Python/Python-tokenize.c
  Python/asdl.c Python/assemble.c Python/ast.c Python/ast_opt.c
  Python/ast_unparse.c Python/bltinmodule.c Python/ceval.c
  Python/codecs.c Python/compile.c Python/context.c
  Python/dynamic_annotations.c Python/errors.c Python/flowgraph.c
  Python/frame.c Python/frozenmain.c Python/future.c Python/getargs.c
  Python/getcompiler.c Python/getcopyright.c Python/getplatform.c
  Python/getversion.c Python/ceval_gil.c Python/hamt.c
  Python/hashtable.c Python/import.c Python/importdl.c
  Python/initconfig.c Python/instrumentation.c Python/intrinsics.c
  Python/legacy_tracing.c Python/marshal.c Python/modsupport.c
  Python/mysnprintf.c Python/mystrtoul.c Python/pathconfig.c
  Python/preconfig.c Python/pyarena.c Python/pyctype.c Python/pyfpe.c
  Python/pyhash.c Python/pylifecycle.c Python/pymath.c Python/pystate.c
  Python/pythonrun.c Python/pytime.c Python/bootstrap_hash.c
  Python/specialize.c Python/structmember.c Python/symtable.c
  Python/sysmodule.c Python/thread.c Python/traceback.c
  Python/tracemalloc.c Python/getopt.c Python/pystrcmp.c
  Python/pystrtod.c Python/pystrhex.c Python/dtoa.c
  Python/formatter_unicode.c Python/fileutils.c Python/suggestions.c
  Python/perf_trampoline.c Python/dynload_shlib.c
  # OBJECT_OBJS
  Objects/abstract.c Objects/boolobject.c Objects/bytes_methods.c
  Objects/bytearrayobject.c Objects/bytesobject.c Objects/call.c
  Objects/capsule.c Objects/cellobject.c Objects/classobject.c
  Objects/codeobject.c Objects/complexobject.c Objects/descrobject.c
  Objects/enumobject.c Objects/exceptions.c Objects/genericaliasobject.c
  Objects/genobject.c Objects/fileobject.c Objects/floatobject.c
  Objects/frameobject.c Objects/funcobject.c Objects/interpreteridobject.c
  Objects/iterobject.c Objects/listobject.c Objects/longobject.c
  Objects/dictobject.c Objects/odictobject.c Objects/memoryobject.c
  Objects/methodobject.c Objects/moduleobject.c Objects/namespaceobject.c
  Objects/object.c Objects/obmalloc.c Objects/picklebufobject.c
  Objects/rangeobject.c Objects/setobject.c Objects/sliceobject.c
  Objects/structseq.c Objects/tupleobject.c Objects/typeobject.c
  Objects/typevarobject.c Objects/unicodeobject.c Objects/unicodectype.c
  Objects/unionobject.c Objects/weakrefobject.c
  # DEEPFREEZE + extras
  Python/deepfreeze/deepfreeze.c Modules/getpath.c Python/frozen.c
  Modules/getbuildinfo.c
)

PASS=0
FAIL=0
OBJ_FILES=""

echo "=== Compiling CPython 3.12.3 with ncc ==="

compile_file() {
  local f="$1"
  local obj="$2"
  case "$f" in
    Modules/getpath.c)
      $NCC $CFLAGS \
        '-DPREFIX="/usr/local"' '-DEXEC_PREFIX="/usr/local"' \
        '-DVERSION="3.12"' '-DVPATH="."' '-DPLATLIBDIR="lib"' \
        -c "$PYDIR/$f" -o "$obj"
      ;;
    Python/dynload_shlib.c)
      $NCC $CFLAGS '-DSOABI="cpython-312-darwin"' -c "$PYDIR/$f" -o "$obj"
      ;;
    *)
      $NCC $CFLAGS -c "$PYDIR/$f" -o "$obj"
      ;;
  esac
}

for f in "${FILES[@]}"; do
  name=$(echo "$f" | sed 's|/|_|g; s|\.c$||')
  obj="$OUTDIR/${name}.o"

  if compile_file "$f" "$obj" 2>/dev/null; then
    PASS=$((PASS+1))
    OBJ_FILES="$OBJ_FILES $obj"
  else
    FAIL=$((FAIL+1))
    echo "FAIL: $f"
    compile_file "$f" "$obj" 2>&1 | head -3
  fi
done

echo ""
echo "=== Compiled: $PASS/$((PASS+FAIL)) ==="

if [ $FAIL -gt 0 ]; then
  echo "Cannot link — there are compile failures."
  exit 1
fi

# Compile Programs/python.c (the entry point)
echo ""
echo "=== Compiling Programs/python.c ==="
PYTHON_OBJ="$OUTDIR/Programs_python.o"
$NCC $CFLAGS -c "$PYDIR/Programs/python.c" -o "$PYTHON_OBJ"

# Create static library
echo "=== Creating libpython3.12.a ==="
ar rcs "$OUTDIR/libpython3.12.a" $OBJ_FILES

# Link python executable
echo "=== Linking python.exe ==="
# Use clang as linker (ncc doesn't need to be the linker)
clang -o "$OUTDIR/python.exe" "$PYTHON_OBJ" "$OUTDIR/libpython3.12.a" \
  -ldl -framework CoreFoundation -Wl,-stack_size,1000000 \
  -lz -lreadline

echo ""
echo "=== Testing ==="
# Copy to build tree (needed for stdlib path detection)
cp "$OUTDIR/python.exe" "$PYDIR/python_ncc.exe"
"$PYDIR/python_ncc.exe" -c 'print("Hello from ncc-compiled CPython!")' 2>&1 || true
echo ""
echo "=== CPython 3.12.3: $PASS/$((PASS)) files compiled with ncc, linked ==="
