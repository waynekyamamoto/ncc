#!/bin/bash
# Build CPython 3.12.3 with ncc (hybrid: ncc where possible, clang fallback)
set -e

NCC=/Users/yamamoto/new_compiler/ncc
PYDIR=/tmp/Python-3.12.3
OUTDIR=/tmp/cpython_ncc_build
CFLAGS="-DPy_BUILD_CORE -DNDEBUG -I$PYDIR -I$PYDIR/Include -I$PYDIR/Include/internal"

mkdir -p $OUTDIR

# Files that need clang fallback (codegen bug: infinite recursion in inline functions)
CLANG_FALLBACK=(
  Modules/main.c Modules/gcmodule.c Modules/signalmodule.c
  Modules/_io/bufferedio.c Modules/_io/textio.c
  Python/_warnings.c Python/ast_opt.c Python/bltinmodule.c Python/ceval.c
  Python/codecs.c Python/errors.c Python/frame.c Python/import.c
  Python/initconfig.c Python/pylifecycle.c Python/pystate.c
  Python/pythonrun.c Python/specialize.c Python/symtable.c
  Python/sysmodule.c Python/thread.c Python/getopt.c
  Objects/abstract.c Objects/bytearrayobject.c Objects/bytesobject.c
  Objects/call.c Objects/cellobject.c Objects/classobject.c
  Objects/codeobject.c Objects/descrobject.c Objects/enumobject.c
  Objects/exceptions.c Objects/genobject.c Objects/floatobject.c
  Objects/funcobject.c Objects/listobject.c Objects/dictobject.c
  Objects/memoryobject.c Objects/methodobject.c Objects/object.c
  Objects/setobject.c Objects/sliceobject.c Objects/tupleobject.c
  Objects/typeobject.c Objects/unicodeobject.c Objects/weakrefobject.c
  Modules/getpath.c
)

is_fallback() {
  local f=$1
  for fb in "${CLANG_FALLBACK[@]}"; do
    [ "$f" = "$fb" ] && return 0
  done
  return 1
}

# All core library .c files
FILES=(
  Modules/config.c Modules/main.c Modules/gcmodule.c
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
  Parser/token.c Parser/pegen.c Parser/pegen_errors.c
  Parser/action_helpers.c Parser/parser.c Parser/string_parser.c
  Parser/peg_api.c Parser/myreadline.c Parser/tokenizer.c
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
  Python/deepfreeze/deepfreeze.c Modules/getpath.c Python/frozen.c
  Modules/getbuildinfo.c
)

compile_file() {
  local f=$1 obj=$2
  local CC=$NCC
  if is_fallback "$f"; then CC=clang; fi

  case "$f" in
    Modules/getpath.c)
      $CC $CFLAGS \
        '-DPREFIX="/usr/local"' '-DEXEC_PREFIX="/usr/local"' \
        '-DVERSION="3.12"' '-DVPATH="."' '-DPLATLIBDIR="lib"' \
        -c "$PYDIR/$f" -o "$obj" 2>/dev/null ;;
    Python/dynload_shlib.c)
      $CC $CFLAGS '-DSOABI="cpython-312-darwin"' -c "$PYDIR/$f" -o "$obj" 2>/dev/null ;;
    *)
      $CC $CFLAGS -c "$PYDIR/$f" -o "$obj" 2>/dev/null ;;
  esac
}

NCC_COUNT=0
CLANG_COUNT=0
OBJ_FILES=""

echo "=== Compiling CPython 3.12.3 ==="

for f in "${FILES[@]}"; do
  name=$(echo "$f" | sed 's|/|_|g; s|\.c$||')
  obj="$OUTDIR/${name}.o"

  if compile_file "$f" "$obj"; then
    OBJ_FILES="$OBJ_FILES $obj"
    if is_fallback "$f"; then
      CLANG_COUNT=$((CLANG_COUNT+1))
    else
      NCC_COUNT=$((NCC_COUNT+1))
    fi
  else
    echo "FAIL: $f"
    exit 1
  fi
done

echo "  ncc: $NCC_COUNT files, clang fallback: $CLANG_COUNT files"

# Compile Programs/python.c
echo "=== Compiling Programs/python.c ==="
$NCC $CFLAGS -c "$PYDIR/Programs/python.c" -o "$OUTDIR/Programs_python.o"

# Create static library
echo "=== Creating libpython3.12.a ==="
ar rcs "$OUTDIR/libpython3.12.a" $OBJ_FILES

# Link python executable
echo "=== Linking python.exe ==="
clang -o "$OUTDIR/python.exe" "$OUTDIR/Programs_python.o" "$OUTDIR/libpython3.12.a" \
  -ldl -framework CoreFoundation -Wl,-stack_size,1000000 \
  -lz -lreadline

# Copy to build tree for stdlib path detection
cp "$OUTDIR/python.exe" "$PYDIR/python_ncc.exe"

echo ""
echo "=== Testing ==="
"$PYDIR/python_ncc.exe" -c 'print("Hello from ncc-compiled CPython!")'
echo ""
"$PYDIR/python_ncc.exe" -c 'import sys; print(f"Python {sys.version}")'
echo ""
"$PYDIR/python_ncc.exe" -c 'print(2**100)'
echo ""
echo "=== SUCCESS: $NCC_COUNT/$((NCC_COUNT+CLANG_COUNT)) files compiled with ncc ==="
