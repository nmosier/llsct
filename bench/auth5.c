#include "bench.h"

#include <assert.h>
#include <stdlib.h>

#include "sodium.h"

static unsigned char key[32];
static unsigned char a[32];

// `c` is output-only
void auth5_bench(unsigned char *c, size_t clen) {
  crypto_auth_keygen(key);
  randombytes_buf(c, clen);
  crypto_auth(a, c, clen, key);
  assert(crypto_auth_verify(a, c, clen, key) == 0);
  if (clen > 0) {
    c[rand() % clen] += 1 + (rand() % 255);
    assert(crypto_auth_verify(a, c, clen, key) != 0);
    a[rand() % sizeof a] += 1 + (rand() % 255);
    assert(crypto_auth_verify(a, c, clen, key) != 0);
  }

  crypto_auth_keygen(key);
}
