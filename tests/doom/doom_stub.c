#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// DG_ScreenBuffer is defined in doomgeneric.c

void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SleepMs(uint32_t ms) {}
uint32_t DG_GetTicksMs(void) {
  struct timespec ts;
  clock_gettime(0, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int DG_GetKey(int *pressed, unsigned char *key) { return 0; }
void DG_SetWindowTitle(const char *title) {
  printf("DOOM: %s\n", title);
}

// Entry point
extern void doomgeneric_Create(int argc, char **argv);
extern void doomgeneric_Tick(void);

int main(int argc, char **argv) {
  printf("Starting DOOM...\n");
  doomgeneric_Create(argc, argv);
  for (int i = 0; i < 10; i++) {
    doomgeneric_Tick();
  }
  printf("DOOM ran 10 ticks!\n");
  return 0;
}
