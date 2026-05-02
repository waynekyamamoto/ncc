// Towers of Hanoi for xv6.
//
// Usage:
//   hanoi [N]   - move N disks from peg A to peg C via peg B
//                 (default N=4 if no argument)
//
// Demo program for xv6 on ncc-built kernel: small recursive workload
// that exercises the user/kernel boundary (write syscalls via printf)
// and stack growth from deep recursion.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static int moves;

static void
hanoi(int n, char from, char via, char to)
{
  if (n <= 0)
    return;
  hanoi(n - 1, from, to, via);
  moves++;
  printf("  %d: move disk %d from %c to %c\n", moves, n, from, to);
  hanoi(n - 1, via, from, to);
}

int
main(int argc, char *argv[])
{
  int n = 4;
  if (argc >= 2) {
    n = atoi(argv[1]);
  }
  if (n <= 0 || n > 16) {
    printf("hanoi: N must be between 1 and 16\n");
    exit(1);
  }
  printf("Towers of Hanoi: %d disks (expect %d moves)\n", n, (1 << n) - 1);
  moves = 0;
  hanoi(n, 'A', 'B', 'C');
  printf("done. %d moves total.\n", moves);
  exit(0);
}
