# ncc Tokenizer — English Specification (Phase 1)

This is the source of truth for ncc's tokenizer. The implementation in
`src/tokenize.c` should be re-derivable from this document without
referring to chibicc or the existing source. When the implementation
and this document disagree, the document is authoritative; bugs in the
implementation are fixed against the document, the document is updated
only when the behavior is intentionally changed.

## 1. Scope

The tokenizer takes a single source file's contents (a NUL-terminated
byte string with a guaranteed trailing newline) and produces a singly-
linked list of `Token` records, terminated by a `TK_EOF` token. It does
not handle preprocessing; it produces a stream of preprocessing tokens
plus a single conversion pass (`convert_pp_tokens`) that the
preprocessor invokes after macro expansion to lower preprocessing-number
tokens to typed numeric tokens.

Inputs the tokenizer reads:

- `File *file` — owns the byte buffer and provides display name and
  file number.

Outputs:

- A `Token *` list. The `next` field of the last real token points at
  a `TK_EOF` token; `TK_EOF`'s `next` is `NULL`.

The tokenizer is single-pass over the byte buffer except for a final
linear pass that fills `line_no` on each token (`add_line_numbers`)
and a final linear pass that re-classifies identifiers that match a
keyword as `TK_KEYWORD` (`convert_keywords`).

## 2. Token kinds

`enum TokenKind` distinguishes:

- `TK_IDENT`     — identifier
- `TK_PUNCT`     — punctuator / operator
- `TK_KEYWORD`   — keyword (a `TK_IDENT` re-classified after tokenize)
- `TK_STR`       — string literal
- `TK_NUM`       — numeric literal (after pp-num conversion: integer or
                   float, with `ty` set; or a character literal)
- `TK_PP_NUM`    — preprocessing number (during tokenize, before the
                   pp-num conversion runs)
- `TK_EOF`       — end of file marker

The preprocessor sees `TK_PP_NUM` for numeric literals; only after macro
expansion completes does `convert_pp_tokens` walk the stream and lower
`TK_PP_NUM` to `TK_NUM` with a real value, type, and (for floats) `fval`.

## 3. Token record (relevant fields)

| Field          | Meaning |
|----------------|---------|
| `kind`         | enum above |
| `next`         | next token in the stream |
| `loc`          | pointer into the source buffer at the first byte of the token |
| `len`          | byte length of the token's source spelling |
| `val`          | integer value (`TK_NUM` integer, `TK_NUM` from char literal) |
| `fval`         | `long double` floating-point value (`TK_NUM` float) |
| `ty`           | `Type *` for `TK_NUM` and `TK_STR` |
| `str`          | decoded string contents for `TK_STR` (escapes resolved, NUL-terminated, length is `ty->array_len`) |
| `file`         | source file pointer |
| `line_no`      | 1-based line number filled by `add_line_numbers` |
| `at_bol`       | true if the token begins a line (no non-newline non-comment tokens before it on this line) |
| `has_space`    | true if any whitespace, comment, or line-continuation immediately preceded this token on the same line |
| `hideset`, `origin` | preprocessor metadata; tokenizer initializes both to NULL |

`at_bol` and `has_space` exist solely for the preprocessor: it uses
`at_bol` to recognize directive lines and `has_space` to distinguish
object-like macros (`#define X foo`) from function-like macros
(`#define X(y) ...`). The tokenizer must set them precisely.

## 4. The main loop

Walk a pointer `p` over `file->contents` from start until `*p == '\0'`.
Maintain `at_bol = true` and `has_space = false` initially. After
emitting a token, set `at_bol = false` and `has_space = false`. After
consuming a piece of whitespace/comment, set `has_space = true`. After
consuming a literal newline `\n`, set `at_bol = true` and
`has_space = false`.

At each iteration, in this order, attempt to match:

1. **Line continuation** — `\<newline>`: advance 2 bytes; treat as
   whitespace (`has_space = true`). Do not set `at_bol`. The
   continuation is invisible to the rest of the source for token-
   composition purposes, but `\n` after the `\` does NOT reset
   `at_bol` because the logical line continues.
2. **Line comment** — `//`: advance to the next `\n` (do not consume
   the `\n` itself); `has_space = true`.
3. **Block comment** — `/*` ... `*/`: advance past the closing `*/`;
   `has_space = true`. Unclosed block comment is `error_at(start,
   "unclosed block comment")`.
4. **Newline** — `\n`: advance 1; `at_bol = true; has_space = false`.
5. **Other whitespace** — `isspace(*p)` (space, tab, vertical tab,
   form feed, carriage return): advance 1; `has_space = true`.
6. **Numeric literal** — a digit, OR `.` followed by a digit: read as
   a preprocessing number (see §6). Emit `TK_PP_NUM`.
7. **String literal — plain** — `"`: read string literal (see §7).
   Emit `TK_STR` with `ty = char[N]` and decoded contents in `str`.
8. **String literal — UTF-8** — `u8"`: same as plain; type is still
   `char[N]`. (ncc treats UTF-8 strings as byte strings.)
9. **String literal — wide / UTF-16 / UTF-32** — `L"`, `u"`, `U"`:
   read as plain string literal with the prefix character skipped.
   Type is `char[N]` (ncc does not currently differentiate wide string
   types in the array-element type — known divergence; see §13).
10. **Character literal — plain** — `'`: read char literal (see §8).
    Emit `TK_NUM` with `ty = int` and `val` set.
11. **Character literal — wide / UTF-16 / UTF-32** — `L'`, `u'`, `U'`:
    same shape as plain; type is `int` for `L'`, `unsigned short` for
    `u'`, `unsigned int` for `U'`.
12. **Identifier or keyword** — see §5. Emit `TK_IDENT`.
13. **Punctuator** — see §9. Emit `TK_PUNCT`.
14. None of the above — `error_at(p, "invalid token")`.

When the loop exits, append a `TK_EOF` token whose `loc` is `p` (the
NUL byte) and whose `at_bol`/`has_space` reflect the trailing state.
Then run `add_line_numbers` and `convert_keywords`.

## 5. Identifiers

An identifier starts with an "ident-start" character and continues
with zero or more "ident-cont" characters. Both classes accept ASCII
letters, ASCII digits (cont only), `_`, `$`, and a curated set of
Unicode codepoints (the `is_ident1` / `is_ident2` predicates in
`unicode.c` define the exact set).

Reading proceeds via UTF-8: `decode_utf8` consumes 1–4 bytes and
returns a codepoint. The byte length is the consumed prefix.

After `read_ident` returns the byte length, emit a `TK_IDENT` whose
`loc` points to the first byte and whose `len` is that length.

The `convert_keywords` pass at end-of-tokenize re-checks each `TK_IDENT`
against the keyword table (§10) and changes `kind` to `TK_KEYWORD` when
matched.

## 6. Numeric literals

The tokenizer first reads a "preprocessing number" — a maximal run of
characters drawn from a permissive grammar — and emits `TK_PP_NUM`.
The preprocessor may concatenate such tokens (`##`); the result is
parsed for value/type only when `convert_pp_tokens` runs.

### 6.1 Preprocessing-number boundary

Starting from a digit (or `.<digit>`), advance while the next byte is
in `[0-9A-Za-z_.]`, plus the special case that `+` or `-` is included
when the previous byte was one of `e E p P` (this is the `e+1` /
`p-3` exponent form).

This intentionally accepts illegal-looking sequences like `0x1.fp+10z`
or `3foo`; pp-numbers are syntactic, not semantic. `convert_pp_tokens`
re-tokenizes them properly later.

### 6.2 Integer parsing (`convert_pp_tokens` -> `read_number` integer branch)

A preprocessing number is **floating-point** iff any of:

- It begins with `0x` or `0X` and contains `.`, `p`, or `P`.
- It contains `.`, `e`, or `E` (decimal float).
- It is followed (in the source) by an `f`/`F` suffix character that
  is not part of a wider identifier.

Otherwise it is an integer. Determine base by prefix:

- `0x` / `0X`        → base 16, parse remainder
- `0b` / `0B`        → base 2, parse remainder (after `0b`)
- leading `0`        → base 8 (a lone `0` is base 10 with value 0)
- otherwise          → base 10

Parse the digits with `strtoull` (or equivalent). After the digits,
parse a suffix consisting of any combination of `u`/`U` and one or
two `l`/`L` (`int_suffix_type`):

| L count | with U   | without U |
|---------|----------|-----------|
| 0       | unsigned int | int |
| 1       | unsigned long | long |
| ≥2      | unsigned long long | long long |

If the parsed value exceeds `INT_MAX` and the suffix is `int`, promote
to `long`. If it exceeds `LONG_MAX` and the suffix is `long`, promote
to `long long`. (Promotion rules for unsigned mirror these.)

After the integer suffix, an optional `i`/`I` suffix marks the
literal as imaginary; the resulting type is `_Complex T` where `T`
is the integer type. `tok->fval` becomes `(long double)val`.

Set `tok->val = val`, `tok->ty = T`, `tok->len = (p - start)`.

### 6.3 Floating-point parsing

Decode the value with `strtold(start, NULL)`. After the digits, parse
suffixes in any order (up to two passes) accepting any of `f`, `F`,
`l`, `L`, `i`, `I`. Set the base type by which letter appeared:

- `f`/`F` → `float`
- `l`/`L` → `long double`
- (none) → `double`

If `i`/`I` appeared, wrap the base type with `_Complex`. Set
`tok->fval`, `tok->ty`, `tok->len`.

### 6.4 Hex floats

Recognized when the literal begins with `0x`/`0X` and contains `p`
or `P` (the binary exponent). Lexical scan consumes `[0-9a-fA-F.]*`
then `[pP][+-]?[0-9]+`. `strtold` parses the value.

## 7. String literals

A plain string literal `"..."` is parsed by:

1. Find the closing `"`. While walking, treat `\<any>` as a 2-byte
   pair (the escape and the next byte), so `\"` does not terminate.
   Newlines and NUL terminate prematurely with
   `error_at(start, "unclosed string literal")`.
2. Allocate a buffer of size `(end - quote)` (a generous over-
   estimate; escapes shrink the result).
3. Walk the contents character by character. When a `\` is seen,
   call `read_escaped_char` (see §7.1); otherwise copy the byte.
4. The buffer is the decoded contents; its length `len` is what was
   actually written. NUL-terminate is implicit (calloc'd buffer).
5. Token's `ty = char[len + 1]`, `str = buf`, `loc = start`,
   `len = (end + 1) - start` (source span includes both quotes).

UTF-8 string literals (`u8"..."`) are tokenized identically; the type
remains `char[N]`.

Wide / UTF-16 / UTF-32 prefixes (`L"..."`, `u"..."`, `U"..."`) are
tokenized identically with the prefix byte skipped. Type is currently
`char[N]` for all (known divergence — see §13).

### 7.1 Escape sequences

Inside `read_escaped_char`, the byte after `\` selects:

- Octal: 1–3 octal digits → numeric value
- `x` then 1+ hex digits → numeric value (continues until non-hex)
- `a` → 7, `b` → 8, `t` → 9, `n` → 10, `v` → 11, `f` → 12, `r` → 13
- `e` → 27 (GNU extension)
- Any other byte → that byte verbatim (covers `\\`, `\"`, `\'`, `\?`)

Hex escape with no hex digits is `error_at(p, "invalid hex escape
sequence")`.

## 8. Character literals

A char literal `'<body>'` reads exactly one character into `c`:

- `\<x>`: call `read_escaped_char` (§7.1).
- Otherwise: `decode_utf8` consumes 1–4 bytes; `c` is the codepoint.

After reading, `strchr(p, '\'')` finds the closing quote. If absent,
`error_at(p, "unclosed char literal")`.

Emit `TK_NUM` with:

- `loc = start`, `len = (end + 1) - start`
- `val = c`
- `ty` is the type passed in: `int` for plain `'X'` and `L'X'`,
  `unsigned short` for `u'X'`, `unsigned int` for `U'X'`.

ncc currently reads a single character; multi-character literals
(`'AB'`) are not handled (known minor divergence; not used by any
real program in scope).

## 9. Punctuators

The maximal-munch rule applies. Try in order:

- 3-byte: `<<=`, `>>=`, `...`
- 2-byte: `==`, `!=`, `<=`, `>=`, `->`, `+=`, `-=`, `*=`, `/=`,
          `%=`, `&=`, `|=`, `^=`, `&&`, `||`, `<<`, `>>`, `++`,
          `--`, `##`
- 1-byte: any C `ispunct(*p)` byte

Emit `TK_PUNCT` with the matched length.

Note: `##` is preprocessor-only; the tokenizer emits it as a regular
punctuator and the preprocessor recognizes it.

## 10. Keywords

After tokenize, `convert_keywords` walks the stream and re-classifies
any `TK_IDENT` whose spelling exactly matches one of:

```
return if else for while do sizeof
struct union enum typedef static extern
inline _Noreturn void char short int
long float double signed unsigned _Bool
const volatile restrict _Atomic _Alignof
_Alignas auto register switch case default
break continue goto __attribute__ _Thread_local
__thread typeof __typeof __typeof__ asm __asm__
_Static_assert static_assert _Generic
_Complex __complex__ __real__ __imag__
```

The match is exact byte-equality of `tok->loc[0..len)` against the
keyword. (Whether `__alignof` is accepted is handled by an exception
in the parser, not here — see parse.c.)

## 11. Position tracking

After the main loop, `add_line_numbers` walks the source buffer once
and for each token whose `loc` matches the current pointer, sets
`tok->line_no` to the running line number (1-based, incremented at
each `\n` byte). The `loc` pointers must be in source order for this
to work, which is enforced by the tokenizer's append-only semantics.

Column information is not stored explicitly; `verror_at` recomputes
it on demand from `loc` and the file contents.

## 12. Error reporting

The tokenizer reports errors via three functions:

- `error(fmt, ...)` — abort, print message; no source context.
- `error_at(loc, fmt, ...)` — abort, print the source line containing
  `loc` with a `^` indicator pointing at `loc` and the message.
- `error_tok(tok, fmt, ...)` — abort, but first walk `tok->origin`
  back to the **macro use site** (the deepest non-origin token). This
  ensures errors in macro-expanded code point at the user-visible
  invocation, not at a token that came out of a system header.
- `warn_tok(tok, fmt, ...)` — same as `error_tok` but does not exit.

`error_at`'s line discovery: scan backward from `loc` until either
the start of `current_file->contents` or `\n`; that is the line
start. Scan forward from `loc` until `\n` or `\0`; that is the line
end. Print `<filename>:<line_no>: <line text>\n<padding>^ <fmt
expansion>\n`.

## 13. Known divergences from ISO C / GCC

These are intentional simplifications, recorded so that re-implementing
from this spec does not "fix" them by accident:

- Wide / UTF-16 / UTF-32 string and character literal element types
  collapse to `int`-family scalars; the array element type for wide
  strings is `char` rather than `wchar_t` / `char16_t` / `char32_t`.
  No real program in scope (sqlite, doom, cpython) uses wide strings
  in a way that depends on the element-type distinction.
- Multi-character character literals (`'AB'`) are not handled; the
  tokenizer reads the first character only. Not used in scope.
- Trigraphs (`??=`, `??/`, etc.) are not recognized. Removed from C23;
  not used in scope.
- Universal character names in identifiers (`\uXXXX`) are not
  recognized; UTF-8 source is supported but escape-style names are
  not. Not used in scope.
- The `e`/`E` exponent in pp-numbers triggers the `+`/`-` continuation
  rule even for hex literals (where `e`/`E` is a digit, not the
  exponent marker). Hex floats use `p`/`P` and that case is handled
  correctly; the looseness of the pp-num grammar accepts a few
  syntactically-malformed hex literals that `convert_pp_tokens` would
  then reject. The kernel does not generate such literals.

## 14. Validation criteria for the swap-out implementation

A re-implementation of the tokenizer derived from this spec is
considered correct when:

1. **Token-stream equivalence** on a validation corpus: tokenize each
   file with both the existing and the new tokenizer, diff the
   resulting token streams (kind + loc + len + val/fval/ty for
   `TK_NUM` + str+len for `TK_STR` + at_bol/has_space). The diff is
   empty.

   Validation corpus: `src/*.c` (ncc itself), `tests/sqlite/sqlite3.c`,
   `tests/cpython/Python-3.12.3/Python/ceval.c` (largest ncc-buildable
   real-world C file), all torture inputs.

2. **Torture pass rate unchanged**: `tests/torture/run.sh --summary`
   reports the same `PASS=` count as the Day 0 baseline (964 of 995).

3. **Bootstrap fixed-point**: `scripts/bootstrap_validate.sh` exits 0
   (stage1 == stage2 by md5).

4. **Real-program builds unchanged**: `tests/sqlite/build.sh` (with
   the `-DSQLITE_MEMORY_BARRIER=` workaround), `build_doom_ncc2.sh`,
   and `tests/cpython/build.sh` all succeed, matching the Day 0
   results.

A failure on any of these reverts the swap-out commit.

## 15. Out of scope

- Preprocessor (Phase 2). Macro expansion, conditional inclusion,
  `#include` resolution, `##`/`#`, predefined macros, `#line`.
- Type system (Phase 3). The tokenizer attaches `Type *` to numeric
  and string tokens, but the type lookup itself (`ty_int`, `ty_char`,
  `complex_type`, `array_of`) is in `type.c` and not re-derived here.
- UTF-8 codepoint decoding. `decode_utf8` and the `is_ident1` /
  `is_ident2` predicates live in `unicode.c` and are used as
  black-box helpers. They are themselves candidates for a separate
  spec if their provenance is questionable.
