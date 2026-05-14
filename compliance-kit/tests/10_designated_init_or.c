/* C-standard: file-scope designated initializer with constant
   expressions on the RHS. */

#define FLAG_A 0x01
#define FLAG_B 0x02
#define FLAG_C 0x04
#define FLAG_D 0x08

struct s {
    int flags;
    int x;
    int y;
};

static struct s o = {
    .flags = FLAG_A | FLAG_B | FLAG_C | FLAG_D,
    .x = 42,
    .y = 7 + 5 * 3,
};

int main(void) {
    if (o.flags != 0xF) return 1;
    if (o.x != 42) return 2;
    if (o.y != 22) return 3;
    return 0;
}
