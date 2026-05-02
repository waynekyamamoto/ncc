/* chacha_neon stubs (substituted in for chacha_neon.c, which uses NEON
 * intrinsics ncc cannot handle). Empty bodies — kernel runtime detection
 * should not pick the NEON crypto path with our build, but if it does,
 * the cipher output is garbage. We accept that risk for first-boot.
 */

#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"

typedef unsigned char uint8_t;
typedef unsigned int  uint32_t;
typedef unsigned long size_t;

void chacha_core_neon(uint8_t out[64], const uint8_t in[16],
    const uint8_t k[32], const uint8_t c[16], unsigned r) { (void)out; (void)in; (void)k; (void)c; (void)r; }

void hchacha_neon(uint8_t out[32], const uint8_t in[16],
    const uint8_t k[32], const uint8_t c[16], unsigned r) { (void)out; (void)in; (void)k; (void)c; (void)r; }

void chacha_stream_neon(uint8_t *s, size_t n,
    uint32_t blkno, const uint8_t nonce[12], const uint8_t k[32], unsigned r) {
    (void)s; (void)n; (void)blkno; (void)nonce; (void)k; (void)r;
}

void chacha_stream_xor_neon(uint8_t *c, const uint8_t *p, size_t n,
    uint32_t blkno, const uint8_t nonce[12], const uint8_t k[32], unsigned r) {
    (void)c; (void)p; (void)n; (void)blkno; (void)nonce; (void)k; (void)r;
}

void xchacha_stream_neon(uint8_t *s, size_t n,
    uint32_t blkno, const uint8_t nonce[24], const uint8_t k[32], unsigned r) {
    (void)s; (void)n; (void)blkno; (void)nonce; (void)k; (void)r;
}

void xchacha_stream_xor_neon(uint8_t *c, const uint8_t *p, size_t n,
    uint32_t blkno, const uint8_t nonce[24], const uint8_t k[32], unsigned r) {
    (void)c; (void)p; (void)n; (void)blkno; (void)nonce; (void)k; (void)r;
}
