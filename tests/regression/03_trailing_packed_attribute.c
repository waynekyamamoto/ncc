// Regression: trailing __attribute__((packed)) was ignored by struct_layout
// because attributes parsed AFTER the layout pass — is_packed was always
// false at layout time.
//
// Found via Linux btrfs btrfs_super_block static_assert(sizeof == 4096).
// Fixed: 2026-04-27 (commit ad2db17).

// A packed struct whose size is only 4096 bytes when the trailing attribute
// is honored. Without it, alignment padding bumps the size.
struct btrfs_like {
  unsigned long long u64_field;
  unsigned int u32_field;
  // 4 bytes of padding here unless packed.
  unsigned long long another_u64;
  char tail[4076];
} __attribute__((packed));

_Static_assert(sizeof(struct btrfs_like) == 4096,
               "trailing packed attribute must shrink layout");

int main(void) { return 0; }
