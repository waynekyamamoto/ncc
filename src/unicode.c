// unicode.c — UTF-8 encoding/decoding and identifier character checks
#include "cc.h"

// Encode a Unicode code point as UTF-8.
// Returns the number of bytes written.
int encode_utf8(char *buf, uint32_t c) {
  if (c <= 0x7F) {
    buf[0] = c;
    return 1;
  }
  if (c <= 0x7FF) {
    buf[0] = 0xC0 | (c >> 6);
    buf[1] = 0x80 | (c & 0x3F);
    return 2;
  }
  if (c <= 0xFFFF) {
    buf[0] = 0xE0 | (c >> 12);
    buf[1] = 0x80 | ((c >> 6) & 0x3F);
    buf[2] = 0x80 | (c & 0x3F);
    return 3;
  }
  buf[0] = 0xF0 | (c >> 18);
  buf[1] = 0x80 | ((c >> 12) & 0x3F);
  buf[2] = 0x80 | ((c >> 6) & 0x3F);
  buf[3] = 0x80 | (c & 0x3F);
  return 4;
}

// Decode a UTF-8 encoded character.
// Sets *new_pos to point past the decoded character.
uint32_t decode_utf8(char **new_pos, char *p) {
  if ((unsigned char)*p < 128) {
    *new_pos = p + 1;
    return *p;
  }

  char *start = p;
  int len;
  uint32_t c;

  if ((unsigned char)*p >= 0xF0) {
    len = 4;
    c = *p & 0x07;
  } else if ((unsigned char)*p >= 0xE0) {
    len = 3;
    c = *p & 0x0F;
  } else if ((unsigned char)*p >= 0xC0) {
    len = 2;
    c = *p & 0x1F;
  } else {
    error("invalid UTF-8 sequence");
  }

  for (int i = 1; i < len; i++) {
    if ((unsigned char)p[i] >> 6 != 2)
      error("invalid UTF-8 continuation byte");
    c = (c << 6) | (p[i] & 0x3F);
  }

  *new_pos = start + len;
  return c;
}

// Check if a character can be the first character of an identifier.
// C11 allows Unicode letters per Annex D.
bool is_ident1(uint32_t c) {
  // ASCII fast path
  if (c < 128)
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_' || c == '$';

  // Unicode: allow general categories L (letter) and Nl (letter number)
  // Simplified: allow anything >= 0x80 that isn't clearly not a letter
  // This is a pragmatic approximation.
  return (c >= 0x80);
}

// Check if a character can be a subsequent character in an identifier.
bool is_ident2(uint32_t c) {
  if (is_ident1(c))
    return true;
  if (c < 128)
    return ('0' <= c && c <= '9');
  return (c >= 0x80);
}
