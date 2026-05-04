// Q11.B / spec §9.4 — Bitfield promotion follows the C standard's
// "value preserving" rule:
//   unsigned bitfield, width < 32  -> int (signed!)
//   unsigned bitfield, width == 32 -> unsigned int
//   signed bitfield,   width <= 32 -> int
//
// Verified via _Generic, which selects a branch at compile time
// based on the controlling expression's type.  If a future
// implementation regresses (e.g., leaves an unsigned-width-<32
// bitfield typed as unsigned), the _Generic branch changes and
// _Static_assert fires.
//
// Spec ref: docs/specs/03_type.md §9.4 (Q7 / Q11.B).

struct S {
    unsigned int u20 : 20;
    unsigned int u32 : 32;
    signed int   s20 : 20;
    signed int   s32 : 32;
};

struct S g_s;

_Static_assert(_Generic(g_s.u20, int: 1, default: 0) == 1,
    "unsigned bitfield width 20 must promote to int (signed)");
_Static_assert(_Generic(g_s.u32, unsigned int: 1, default: 0) == 1,
    "unsigned bitfield width 32 stays unsigned int");
_Static_assert(_Generic(g_s.s20, int: 1, default: 0) == 1,
    "signed bitfield width 20 promotes to int");
_Static_assert(_Generic(g_s.s32, int: 1, default: 0) == 1,
    "signed bitfield width 32 stays int");

int main(void) { return 0; }
