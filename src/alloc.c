// alloc.c — Memory allocation helpers
#include "cc.h"

void *calloc_checked(int count, int size) {
  void *p = calloc(count, size);
  if (!p)
    error("out of memory");
  return p;
}

char *strndup_checked(const char *s, int n) {
  char *p = strndup(s, n);
  if (!p)
    error("out of memory");
  return p;
}

char *format(const char *fmt, ...) {
  char *buf;
  size_t buflen;
  FILE *fp = open_memstream(&buf, &buflen);

  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fclose(fp);
  return buf;
}

void strarray_push(StringArray *arr, char *s) {
  if (arr->len >= arr->capacity) {
    arr->capacity = arr->capacity ? arr->capacity * 2 : 8;
    arr->data = realloc(arr->data, sizeof(char *) * arr->capacity);
  }
  arr->data[arr->len++] = s;
}
