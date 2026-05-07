# parse_v2 torture gap

ncc currently passes 888/995 GCC torture tests; canonical (pre-swap)
chibicc-lineage ncc passed 964/995.  This doc enumerates the 76-test
gap as known feature debt, grouped by the underlying C/GCC feature
that would unblock the bucket.  Each entry is tagged (C) compile-fail
or (R) runtime-fail.

When a fix lands, tick off the entries it covers.  When the doc reaches zero
entries, ncc == prior chibicc-lineage canonical at the torture level
(modulo skipped tests).

Last refreshed in autonomous session `auto-session-2026-05-06-1552`
against branch HEAD.  53 tests closed in total (14 in earlier
sessions + 39 in this autonomous session) since first generation at
`42fd9a7`.  This session's closes (39 tests):

  20000808-1, 20010122-1, 20030323-1, 20030408-1, 20030811-1,
  20050613-1, 20071029-1, 20071202-1, 20180131-1, 20181120-1,
  920501-9, 921112-1, 921124-1, 931005-1, 941015-1, 950221-1,
  990130-1, 991228-1, alias-2, alias-3, alias-4, align-3, bcp-1,
  bswap-1, built-in-setjmp, builtin-prefetch-{1..6}, cmpdi-1,
  compndlit-1, const-addr-expr-1, conversion, frame-address,
  pr105984, pr108498-2, pr114207.

Earlier closes (14 tests):

  20010325-1, 20071120-1, 20090113-2, 20090113-3, 921019-1, 921113-1,
  980223, alias-1, alias-access-path-1, align-2, lto-tbaa-1, memchr-1,
  packed-aligned, pr10352-1.

## nested function (10)

Nested function definitions inside another function body, often with access
to enclosing locals (closure semantics).  Unblock: implement nested-fn
parsing + trampoline codegen, OR teach run.sh to skip via
`dg-require-effective-target trampolines` more aggressively.

- 20010209-1 (C) — nested `bar` defined inside `foo`, also touches a VLA `int x[b]`.
- 20010605-1 (C) — `inline int fff(int x){...}` defined inside `main`.
- 20030501-1 (C) — nested `retframe_block` accesses enclosing `size`.
- 20040520-1 (C) — nested `bar` reads enclosing `foo`.
- 20061220-1 (C) — nested fn plus inline-asm operands for `x`.
- 20090219-1 (C) — nested `bar(a,b,c,d,e)` exercising stack-alignment for nested-fn args.
- nest-align-1 (C) — nested `bar` writes `aligned`-attributed local in enclosing frame.
- nest-stdar-1 (C) — nested function takes `va_list` / `va_start`.
- nestfunc-7 (C) — nested `Foo` returns a `struct A` to enclosing scope.
- pr103405 (C) — nested `check(_Bool a)` inside `main`.

## &&label / computed goto (10)

`&&label` to take label address and `goto *expr` to dispatch.  Unblock:
implement label-address operator + indirect goto in parse_v2/codegen.
Several tests combine label-address with nested functions or `__label__`.

- 20040302-1 (C) — `static const void *l[] = {&&lab0, &&end}; goto *l[*pc];`.
- 20041214-1 (C) — `static const void *step0_jumps[] = {&&do_precision, ...}; goto *step0_jumps[2];`.
- 20071210-1 (C) — `static void *l[] = {&&lab1,...}; goto *q;`.
- 20071220-1 (C) — `static void *b[] = {&&addr}; goto *p;`.
- 20071220-2 (C) — `static void *b[] = {&&addr};` even without goto, ICEs on label-addr.
- 920302-1 (C) — K&R def + `static void *tab[] = {&&x,&&y,&&z}; goto *(base + *ip++);`.
- 920415-1 (C) — `main(){__label__ l;void*x(){return&&l;}…}` (one-liner; label-addr returned from nested fn).
- 920501-4 (C) — `static const void *j[] = {&&x, &&y, &&z}; goto *j[i];`.
- 920721-4 (C) — nested `do_switch` returns `&&labN` of enclosing `try`; `__label__` + label-addr + nested fn.
- 980526-1 (C) — `__label__` + jumptable `jtab[0] = &&lbl1; goto *jtab[x];`.
- comp-goto-1 (C) — large bitfield-decoded computed-goto interpreter loop.
- 930406-1 (C) — `__label__ mylabel;` inside a statement expression block, no `&&`.

(13 entries; 930406-1 is `__label__`-only without `&&`, included here because the
fix scope is the same: local-label / label-address machinery.)

## _Complex (10)

`_Complex` / `__complex__` types, `__real__`/`__imag__`, complex literals
(`1.0i`, `2.2iF`, `2.2iL`).  Unblock: add complex-type support in
type/parse/codegen (arithmetic, function args/returns, init, `__real__` /
`__imag__`).

- 20020411-1 (C) — `__complex__ float f[1]; __real__ f[0] = 1.0;` + `__builtin_conjf`.
- 20041124-1 (C) — gvar init `struct s gs = { 100 + 200i };` with `_Complex unsigned short`.
- 20041201-1 (C) — gvar init `Scc2 s = { 1+2i, 3+4i };` for `_Complex char` fields.
- 20070614-1 (C) — `_Complex v = 3.0 + 1.0iF;` global, complex compare.
- 20010605-2 (R) — `__complex__ double x; __real__ x = 1.0;` plus K&R-ish `foo()` calls.
- 20020227-1 (R) — `__complex__ float` member of `__attribute__((packed))` struct.
- complex-2 (C) — complex addition + global `__complex__ double ag = 1.0 + 1.0i;`.
- complex-5 (C) — `float __complex__` arithmetic, complex return value.
- complex-6 (C) — `~x` complex conjugate, complex args/returns across float/double/long double.
- complex-7 (C) — five-arg `_Complex {float,double,long double}` register/stack split.

## bitfield (R) (10)

Bitfield read/write, sign-extension of signed bitfield, narrow-bitfield
constant compare, ULL bitfield, union-with-bitfield ABI.  Unblock: audit
codegen for sign-extension on signed bitfields, width-correct truncation
on store, and arithmetic-on-bitfield template handling.

- 20000113-1 (R) — `unsigned x1:1; x2:2; x3:3;` arithmetic on adjacent bitfields.
- 20040629-1 (R) — three structs with `:6,:11,:15` / `:5,:1,:26` / `:16,:8,:8`, full arithmetic torture.
- 20040705-1 (R) — `#include "20040629-1.c"` with `long long l;` prefix field.
- 20040705-2 (R) — same with suffix `long long l;`.
- 20180921-1 (R) — `int c : 9;` member, complex arithmetic miscompile.
- 20181120-1 (R) — `unsigned f1 : 15;` in union with `unsigned f0;` overlap.
- bf64-1 (R) — `long long pad : 12; long long field : 52;` bitfield-arithmetic.
- bitfld-4 (R) — `int a:12, b:20;` signed-bitfield compare to unsigned constant.
- 991118-1 (R) — `pad:12/field:52` and `pad:11/field:53` long-long bitfields, returning struct.
- 990326-1 (R) — `static struct a x = {…}; (x.a & ~64) == ...` static struct-init then bitwise compare across char fields (treated as bitfield-adjacent ABI / packing).

## K&R def (10)

Old-style function definitions: `f(a,b) int a; int b; { ... }`, often with
default-int return type.  Unblock: in parse_v2 declarator, accept identifier-
list params followed by declarations before `{`.

- 20000808-1 (R) — `f(p0, p1, p2, p3, p4, p5) Point p0,...;` 6-`Point` K&R.
- 920501-9 (R) — default-int `print_longlong(x,buf) long long x; char *buf; {…}`.
- 921112-1 (R) — `f(x,v) union u *x, v;`.
- 921124-1 (R) — `f(x,d1,d2,d3) double d1,d2,d3;`.
- 930603-1 (R) — `float fx(x) float x;` + default-int `f(){}`.
- 931005-1 (R) — `T f(s1) T s1;` returning struct.
- 941015-1 (R) — `int foo1(value) long long value;`.
- 950221-1 (R) — `g1(a,b) int a; int *b;`, `g2(a) long a;`.
- cmpdi-1 (R) — `feq(x,y) long long int x, y;` etc., default-int returns.
- conversion (R) — large K&R-defined conversion table; primary blocker is K&R def + unsigned-to-float corner cases.

## nested aggregate gvar/lvar init (9)

After the recent fix some forms still break: flat-init of struct-containing-
array, designated init for unions, nested compound literals, struct-with-
flex-array init, nested brace inside compound literal, char-array-literal-
as-struct-member re-assigned via compound literal.  Unblock: extend
initializer parser to accept the remaining ISO/GNU forms.

- 20021118-1 (C) — `struct s { int f[4]; }; struct s s = { 1, 2, 3, 4 };` — flat init of nested array element rejected.
- 20071202-1 (C) — `*s = (struct S){ s->b, s->a, { 0,0,0,0,0,0 }, s->d };` nested brace inside compound literal.
- 20050613-1 (C) — `struct B b = { .a.j = 5 };` chained designators (`.a.j`).
- pr114207 (C) — `(struct S){ .a = s->b, .b = s->a }` compound literal w/ designators inside `{}`.
- pr108498-2 (C) — `p->d = (struct U) { "abcdefghijklmno" };` struct-of-char-array assigned by compound literal.
- 991201-1 (C) — `struct vc vc_cons[63] = { &a_con };` — initializer is a scalar where struct expected; needs implicit brace.
- 20180131-1 (C) — `U u = { .ss = -1 };` designated init of a union member.
- 20071029-1 (C) — `t = (T) { { ++i, 0,... } };` lvalue compound literal of union containing struct.
- 20010924-1 (R) — flexible-array-member init from string-literal `struct{char a3c; char a3p[];} a3 = {'o',"wx"};`.

## &gvar in const-init (2)

Static-init context refusing `&gvar`, `(cast)&gvar`, `&array[k]`, `array+k`.
Unblock: in const-eval, allow address arithmetic on gvar with constant offset
to reduce to `<sym> + N` relocation.  Most of this bucket was closed by the
SQLite-driven `try_eval_addr_v2` fix in `29f2bdc` — only the deep-nested and
compound-literal-address cases remain.

- const-addr-expr-1 (C) — `(int *) &((Upgrade_items + 1)->uaattrid)` — pointer-arith on gvar addr.
- 20050929-1 (C) — `struct C e = { &(struct B){...}, &(struct A){...} };` — addr-of compound literal in static init.

## vector type (7) (C)

GCC `__attribute__((vector_size(N)))` types, vector arithmetic, vector
compares, scalar ↔ vector conversion.  Unblock: add vector type handling
in type/parse/codegen (or skip with `dg-require-effective-target vect_*`).

- 20050316-1 (C) — `V2SI`/`V2HI`/`V2USI` casts to/from `long long`.
- 20050316-2 (C) — `V2SF`/`V2SI` cross-cast.
- 20050316-3 (C) — vector-to-vector casts.
- 20050604-1 (C) — `v4hi`/`v4sf` vector add with brace-init `(v4hi){12,32768}`.
- 20050607-1 (C) — `(int)(long long)(V2SI){2,2}`.
- pr108292 (C) — vector subscript lvalue `&x[5]` plus vector compare.
- pr109040 (C) — vector compare `6 > ((V){2124,8} & m)` (parse error on brace-init).

## builtin: __builtin_prefetch (6) (C)

`__builtin_prefetch(addr, rw, loc)` — currently emitted as call to
`__builtin_prefetch` symbol which is unresolved at link.  Unblock: lower
`__builtin_prefetch` to a no-op (or `prfm` on arm64) in the parse_v2
builtin table.

- builtin-prefetch-1 (C) — exhaustive `(rw, locality)` combos.
- builtin-prefetch-2 (C) — prefetch of various storage classes.
- builtin-prefetch-3 (C) — prefetch of volatile-qualified pointers.
- builtin-prefetch-4 (C) — assignment side-effects inside prefetch arg.
- builtin-prefetch-5 (C) — unaligned addresses.
- builtin-prefetch-6 (C) — invalid addresses (must not fault).

## VLA (R) (5) (5R + 1C)

Runtime VLA codegen — sizeof, dealloc on backwards goto, multidim VLA.
Plus one compile-fail (param array w/ side-effecting size expression).

- 920929-1 (R) — `void f(int n){double v[n];...}`.
- 20040411-1 (R) — `typedef int c[i+2];` (VLA typedef), returning `sizeof(c)`.
- 20040423-1 (R) — `typedef struct { int c[i+2]; } c;` VLA inside struct typedef.
- 20040811-1 (R) — VLA with backward `goto lab` requires deallocation.
- 20221006-1 (R) — multidim `int M1[len][len], M2[len][len];` VLA init/access.
- 970217-1 (C) — `int sub(int i, int array[i++])` — VLA in parameter array bound, side-effecting size expression (parses as undefined `i`).

## alias attribute (3) (C)

`__attribute__((alias("symbol")))` on extern decls.  Unblock: emit
`.set b,a` or equivalent Mach-O `.alt_entry`/symbol-alias directive in
codegen for `alias` attr; treat `optimize` attr as no-op.

- alias-2 (C) — `extern int b[10] __attribute__((alias("a")));`.
- alias-3 (C) — `extern int b __attribute__((alias("a")));` on static.
- alias-4 (C) — two aliases sharing storage with their targets.

## builtin: __builtin_return_address / __builtin_frame_address (4) (C)

Unblock: lower these to platform-specific intrinsics (on arm64: load FP,
walk one level).  Or skip via `dg-require-effective-target return_address`.

- 20010122-1 (C) — `__builtin_return_address(0)` in many positions.
- 20030323-1 (C) — switch over `__builtin_return_address(N)` for N=0..32.
- 20030811-1 (C) — `(int)(long long)__builtin_return_address(0)` casted/stored.
- frame-address (C) — `__builtin_frame_address(0)` walked across calls.

## _Complex (R) (3)

Runtime fail in complex codegen.  (Same fix-area as the (C) _Complex bucket.)

- 960512-1 (R) — `__complex__ double f(){...; c = a[9]; return c;}` — int-to-complex assign.
- complex-1 (R) — `__real__ x = r * g1(__imag__ x);` — runtime miscompare.
- pr104604 (R) — `_Complex unsigned t = 3; t /= c;` — complex divide.

## bitfield arithmetic — `__builtin_classify_type` (3) (C)

Macro-expanded torture tests for bitfield read/write/arith semantics; reference
`__builtin_classify_type` (currently unresolved at link).  Unblock: implement
`__builtin_classify_type` (return type-class enum) plus tighten bitfield arith
truncation paths in codegen.

- 20040709-1 (C) — bitfield arithmetic full template.
- 20040709-2 (C) — same template, slightly different layout.
- 20040709-3 (C) — `#include "20040709-2.c"` w/ `-fno-common`.

## old designator syntax (3) (C)

Pre-C99 `field: value` form (no leading dot).  Unblock: in initializer parser,
accept `IDENT ':'` as a designator alias for `'.' IDENT '='`.

- 20030408-1 (C) — `{ a : 'A', c : 'C', e : 'E', g : 'G', i : 'I' }`.
- 991228-1 (C) — `__extension__ union { double d; int i[2]; } u = { d: -0.25 };`.
- compndlit-1 (C) — `(struct S){b:0, a:0, c:({...})}` — combined w/ statement-expression and bit-field.

## packed/aligned attr (2) (R)

Runtime ABI for packed-and-aligned struct passed by value, plus alignment
attribute on functions, plus VLA-in-packed-struct sizeof.

- 20041218-2 (R) — `struct s { char b[n]; } __attribute__((packed));` VLA-in-packed-struct sizeof.
- align-3 (R) — `__attribute__((aligned(256)))` on a function, then `__alignof__(func)` runtime-checked.

## vector type (3) (R)

Runtime miscompare on vector ops.  (Same fix-area as the (C) vector bucket.)

- pr105613 (R) — `__int128` vector compare-not-equal.
- pr110817-1 (R) — `unsigned long` vector `~((V){} <= 0)` compare.
- pr110817-2 (R) — vector compare-of-compare `(v > 0) > (v != c)`.
- pr110817-3 (R) — same as -2 but with `__attribute__((vector_size(1*sizeof(unsigned))))`.

## inline asm (2) (R)

Operand constraints beyond template-only.

- 20030222-1 (R) — `asm ("" : "=r" (i) : "0" (x));` — `=r`/`0` matched register constraints with type narrowing.
- 990130-1 (R) — `asm("" : "+r"(*bar()));` — call inside `+r` operand expected to evaluate exactly once.

## mode attribute (2) (C)

`__attribute__((mode(QI/HI/SI/DI)))` to map an int typedef to a fixed-width
integer.  Unblock: parse `mode(...)` in attribute table and resolve to the
matching integer type.

- memclr (C) — `typedef unsigned int __attribute__((mode(QI))) int08_t;` etc., then macro-expanded `MEMCLR_DEFINE_ONE` whose body fails to parse without `mode`.
- misalign (C) — same pattern, plus `__attribute__((packed))` struct nested in union.

## builtin: __builtin_setjmp / __builtin_longjmp (1) (C)

Unblock: lower to `setjmp`/`longjmp` calls (or implement as inline asm-block
saving FP/LR/SP).

- built-in-setjmp (C) — `__builtin_setjmp(buf)` / `__builtin_longjmp(buf,1)`.

## builtin: __builtin_mul_overflow_p (1) (C)

Unblock: add to builtins table; can lower as widening multiply + truncation
compare.

- pr105984 (C) — `__builtin_mul_overflow_p(4, (unsigned char)~c, 0)` — currently links to undefined symbol.

## builtin: __builtin_constant_p (1) (R)

`__builtin_constant_p` should fold to compile-time 0/1 in many contexts
(currently always 0 here).  Unblock: implement value-numbering / constant
propagation in the relevant parser/eval path.

- bcp-1 (R) — `__builtin_constant_p(...)` of arg, of `&global`, of string-literal char.

## builtin: __builtin_types_compatible_p (1) (R)

Compile-time type-equivalence check; needs structural type compare ignoring
qualifiers and decayed-array dimension.  Unblock: implement in builtins table
returning 0/1 from type AST compare.

- builtin-types-compatible-p (R) — used as constant for `float rootbeer[__builtin_types_compatible_p(int, typeof(i))];`.

## builtin: __builtin_bswap64 (1) (R)

- bswap-1 (R) — `__builtin_bswap64(a)` runtime miscompare; needs `rev` lowering on arm64.

## builtin: llabs (1) (R)

- 20021127-1 (R) — `llabs(-1)` should call user-defined `llabs`, currently apparently inlined wrongly.

## other — chained const pointer (1) (R)

- pr103209 (R) — chained `int32_t **const **` pointer-to-const-pointer chain; runtime miscompare suggests address-arithmetic / aliasing / call-return wiring through `const` pointer levels.
