# ncc Parser — Declarations Sub-Chunk (Phase 4, 04a)

This sub-chunk specifies the **declaration zone** of `src/parse.c`:
declaration specifiers, declarators, struct/union/enum bodies,
array and VLA dimensioning, attributes, and the top-level entry
point that dispatches between declarations and function
definitions.

Cross-cutting decisions (typedef rules, `is_typename`, scope
mechanics, attribute disposition table) live in `04_parse.md`;
this file cites them.

**Coverage in `src/parse.c` (per `04_parse_inventory.md`):**

| Lines | Section |
|---|---|
| 463–614 | Variable creation + variable attributes |
| 616–700 | Type-name detection + typedef handling |
| 702–959 | Declaration specifiers (`declspec`) |
| 960–1072 | Declarator parsing |
| 1073–1254 | Array dimensions (fixed, VLA, incomplete) |
| 1255–1429 | Struct / union declarations + member layout + `__asm__` labels |
| 1430–1691 | `__attribute__((..))` parsing and dispatch |
| 1693–1804 | Enum parsing + misc declaration helpers |
| 4547–4662 | Local declarations inside functions |
| 5549–end | Top-level entry (`parse`) — dispatch into this sub-chunk |

**Status:** skeleton.  Substantive drafting in progress per Q2.C.

---

## A. Declaration grammar overview

[Top-level grammar in BNF-ish form, mapping to `parse.c`'s
recursive-descent functions.  Distinguishes file-scope declarations,
local declarations, parameter declarations, and struct-member
declarations.]

## B. `declspec` — declaration specifiers

[The declaration-specifier combiner.  Parses storage-class +
type-specifier + qualifier + attribute keywords in any order.  Builds
a single `Type *` and `VarAttr` (storage class + alignment +
linkage flags).  Specifies the type-specifier state machine that
combines `signed`, `unsigned`, `int`, `long`, etc. into the right
canonical type per C11 §6.7.2/2.  Calls `is_typename` (§5 of
`04_parse.md`) for typedef-name resolution.]

### B.1 Type-specifier combinations

[The exhaustive table: `int`, `unsigned int`, `signed int`,
`long`, `long long`, `unsigned long long`, etc.  Match `main`
exactly.]

### B.2 Storage class

[`static`, `extern`, `auto`, `register`, `_Thread_local`,
`typedef` — at most one per declspec; `typedef` is special-cased
because it short-circuits the rest of declaration handling.]

### B.3 Qualifiers

[`const`, `volatile`, `restrict`, `_Atomic` — accumulated into the
type's qualifier flags.  Detail of how qualifiers attach to typedef
chains.]

### B.4 Function specifiers

[`inline`, `_Noreturn` — recorded on `VarAttr`; honored by codegen
for `_Noreturn` (suppresses falloff diagnostic), parsed-and-
ignored for `inline`.]

### B.5 Alignment specifiers

[`_Alignas(T)`, `_Alignas(N)` — both forms.  Interaction with
`__attribute__((aligned(N)))`.]

### B.6 GCC `__typeof__` integration

[`__typeof__(expr)` and `__typeof__(type-name)` produce a `Type *`
inline within declspec.]

## C. Declarator

[The recursive declarator grammar: `*` (pointer),
`[N]` (array), `(...)` (function or grouping), identifier
(terminal).  Pointer qualifiers (`const T *`, `T * const`).  Function
prototypes — see §E.  The "spiral" / inside-out type-construction
order.]

### C.1 Abstract declarators

[Type names without identifiers, used in casts, sizeof, function
parameters, etc.  Same grammar; identifier slot is empty.]

### C.2 Declarator vs abstract declarator disambiguation

[Where the parser must look ahead to decide.  Specifically the case
of `int (x)` vs `int (*)`.]

## D. Array dimensions and VLA

[`[N]` (constant), `[*]` (VLA-prototype), `[]` (incomplete),
`[CONST_EXPR]` (constant fold via `eval_node`), `[NON_CONST_EXPR]`
(VLA).  Decision tree, hidden-local emission per §7 of
`04_parse.md`.]

### D.1 VLA size-variable insertion

[Reference §7.3 of `04_parse.md`; describe in detail the insertion
mechanism inside `compound_stmt`.]

### D.2 VLA in function parameters

[`void f(int n, int a[n])` — the `n` parameter must precede `a`'s
declarator; `a` decays to `int *` in the parameter list per C
semantics, but the size expression is still evaluated for
side-effect purposes (parameter VLA: `[*]` form).]

## E. Function prototypes

[Parameter list grammar: `(void)`, `()` (empty K&R-ish), `(T1, T2,
...)`, variadic `...`.  Parameter type adjustment (array → pointer,
function → pointer-to-function).  Storage classes on parameters
(only `register` is permitted).]

### E.1 K&R-style declarations

[Out of scope: ncc does not accept `int f(a, b) int a; int b;
{...}` — modern prototype-only.  Document explicitly to flag
incompatibility with very-old C.]

## F. Struct / union / enum

### F.1 Tag namespace mechanics

[`struct Foo`, `union Foo`, `enum Foo` reference the tag
namespace.  Forward declarations (`struct Foo;`).  Same-tag
redefinition error rules.]

### F.2 Struct body parsing

[Member-declaration-list: declspec + comma-separated declarators
+ optional bit-field width.  Anonymous members (unnamed structs/
unions inside structs).  Flexible array member as last member.]

### F.3 Member layout

[Offset computation: respect alignment, bit-field packing rules
per C11 §6.7.2.1.  Padding insertion.  Total struct size +
alignment.]

### F.4 Bit-fields

[`int x : 5;` syntax.  Width must be a constant expression
folded via `eval_node`.  Type-of-bit-field rules: `int`,
`unsigned int`, `_Bool`, named `char`/`short` widths.  Storage-
unit packing.  Zero-width separator semantics.]

### F.5 Union layout

[Simpler than struct: max-member-size, all offsets 0, alignment
is max-member-alignment.]

### F.6 Enum

[`enum E { A, B = 5, C }` — value-assignment rules, default
underlying type (int unless any value exceeds int range), enum
constants placed in ordinary namespace.]

### F.7 Anonymous struct / union members

[GCC extension: `struct S { struct { int x; }; }` exposes `x` as
if it were a direct member.  Handled by `find_member` walking
embedded-struct chains.]

## G. `__attribute__` parsing

[Dispatch to per-attribute handlers.  Reference `04_parse.md` §11
for the disposition table.  This section specifies the parsing
grammar (`__attribute__((name(args)))`) and the dispatch
mechanism, not the per-attribute semantics (which are in §11 of
`04_parse.md`).]

### G.1 `__asm__("name")` symbol-name override

[`int x __asm__("real_name");` — places `x` in symbol table as
`real_name`.  Honored by codegen; affects linkage but not C-level
identity.]

## H. Local declarations inside functions

[The declaration zone inside a `compound_stmt`.  Differences from
file-scope: storage class defaults to `auto`; `static` produces a
hidden global; init lowering produces ND_ASSIGN sequence (see
`04d_init.md`).  VLA hidden-local emission (per §D.1).]

### H.1 `static` locals

[Promotion to anonymous file-scope global; initialization runs
once at program start; per-call value persistence.]

### H.2 `extern` locals

[Deferred linkage to a name resolved at link time; no storage at
local scope.]

## I. Top-level entry (`parse`)

[The main loop in `src/parse.c` line 5552: walk the token stream,
dispatch each top-level construct.  Three cases:
1. `typedef` — parse declspec + declarator(s), register typedef in
   ordinary namespace, no AST emitted.
2. Function definition — declspec + declarator + `{` body + `}`.
   Emit `Obj`, append to global list.
3. Declaration (variable, function prototype) — declspec +
   declarator(s) + `;`.  Emit `Obj`(s).]

### I.1 Disambiguation: function definition vs declaration

[After parsing declspec + declarator, peek the next token.  `{` →
function definition; `;` or `,` → declaration.  Other token →
error.]

### I.2 `_Static_assert` at file scope

[Parsed as a top-level construct, emits no `Obj`.  Calls
`try_eval_node` (§6 of `04_parse.md`) on the assertion expression.]

## J. Variable creation helpers

[`new_lvar` (local), `new_gvar` (global), `new_anon_gvar`
(compiler-emitted globals like string literals), `new_var_node`
(reference to existing var).  Counter mechanics for anonymous
names.  Per-scope vs per-translation-unit storage chains.]

## K. `Obj` data shape

[Field-by-field reference for `Obj`: name, asm_name, ty, locals,
params, body, init_data, rel, is_function, is_definition,
is_static, is_tentative, is_tls, is_inline, is_live, alignment,
section, va_area, alloca_bottom, stack_size, is_constructor,
is_destructor.  Which fields are populated by this sub-chunk's
parsing paths.]

## L. Worked examples

### L.1 `static const int x = 42;`

[Trace through declspec → declarator → init parse → `Obj`
emission.]

### L.2 `int (*f)(int, int);`

[Function-pointer declaration; declarator nesting.]

### L.3 `struct S { int a; int b : 5; int c : 3; } s;`

[Struct + bit-field layout + variable declaration in one
construct.]

### L.4 `int a[n][m];` where `n`, `m` are runtime

[Two-dimensional VLA; size-variable emission ordering.]

### L.5 `typedef int (*signal_handler_t)(int);`

[Typedef of function-pointer type; chain interaction.]

## M. Phase 5 prerequisites added by 04a

[AST invariants this sub-chunk's parsing produces that codegen
relies on.  Append to `04_parse.md` §15 master list during impl
review.]

## N. Open questions

[Sub-chunk-specific questions raised during drafting; resolved
inline or escalated to a Phase 4 design-Q&A revision.]
