#include "tls.h"
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lightjs {
namespace tls {

// Secure random number generation
void secureRandom(uint8_t* buffer, size_t len) {
#ifdef _WIN32
  HCRYPTPROV hProv;
  if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
    CryptGenRandom(hProv, static_cast<DWORD>(len), buffer);
    CryptReleaseContext(hProv, 0);
  } else {
    // Fallback to less secure random
    srand(static_cast<unsigned>(time(nullptr)));
    for (size_t i = 0; i < len; i++) {
      buffer[i] = static_cast<uint8_t>(rand() & 0xFF);
    }
  }
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    ssize_t bytesRead = read(fd, buffer, len);
    close(fd);
    if (bytesRead == static_cast<ssize_t>(len)) {
      return;
    }
  }
  // Fallback
  srand(static_cast<unsigned>(time(nullptr)));
  for (size_t i = 0; i < len; i++) {
    buffer[i] = static_cast<uint8_t>(rand() & 0xFF);
  }
#endif
}

// X25519 implementation (Curve25519 Diffie-Hellman)
// Based on the RFC 7748 specification

void X25519::feZero(fe out) {
  for (int i = 0; i < 16; i++) out[i] = 0;
}

void X25519::feOne(fe out) {
  out[0] = 1;
  for (int i = 1; i < 16; i++) out[i] = 0;
}

void X25519::feCopy(fe out, const fe in) {
  for (int i = 0; i < 16; i++) out[i] = in[i];
}

void X25519::feAdd(fe out, const fe a, const fe b) {
  for (int i = 0; i < 16; i++) out[i] = a[i] + b[i];
}

void X25519::feSub(fe out, const fe a, const fe b) {
  for (int i = 0; i < 16; i++) out[i] = a[i] - b[i];
}

void X25519::feReduce(fe h) {
  int64_t carry;
  for (int i = 0; i < 16; i++) {
    carry = h[i] >> 16;
    h[i] -= carry << 16;
    if (i < 15) {
      h[i + 1] += carry;
    } else {
      h[0] += 38 * carry;
    }
  }
}

void X25519::feMul(fe out, const fe a, const fe b) {
  int64_t t[31] = {0};

  for (int i = 0; i < 16; i++) {
    for (int j = 0; j < 16; j++) {
      t[i + j] += a[i] * b[j];
    }
  }

  // Reduce mod 2^255 - 19
  for (int i = 16; i < 31; i++) {
    t[i - 16] += 38 * t[i];
  }

  for (int i = 0; i < 16; i++) {
    out[i] = t[i];
  }

  feReduce(out);
  feReduce(out);
}

void X25519::feSq(fe out, const fe a) {
  feMul(out, a, a);
}

void X25519::feCswap(fe f, fe g, int b) {
  int64_t mask = -b;
  for (int i = 0; i < 16; i++) {
    int64_t t = mask & (f[i] ^ g[i]);
    f[i] ^= t;
    g[i] ^= t;
  }
}

void X25519::feInvert(fe out, const fe z) {
  fe t0, t1, t2, t3;

  feSq(t0, z);
  feSq(t1, t0);
  feSq(t1, t1);
  feMul(t1, z, t1);
  feMul(t0, t0, t1);
  feSq(t2, t0);
  feMul(t1, t1, t2);
  feSq(t2, t1);
  for (int i = 0; i < 4; i++) feSq(t2, t2);
  feMul(t1, t2, t1);
  feSq(t2, t1);
  for (int i = 0; i < 9; i++) feSq(t2, t2);
  feMul(t2, t2, t1);
  feSq(t3, t2);
  for (int i = 0; i < 19; i++) feSq(t3, t3);
  feMul(t2, t3, t2);
  feSq(t2, t2);
  for (int i = 0; i < 9; i++) feSq(t2, t2);
  feMul(t1, t2, t1);
  feSq(t2, t1);
  for (int i = 0; i < 49; i++) feSq(t2, t2);
  feMul(t2, t2, t1);
  feSq(t3, t2);
  for (int i = 0; i < 99; i++) feSq(t3, t3);
  feMul(t2, t3, t2);
  feSq(t2, t2);
  for (int i = 0; i < 49; i++) feSq(t2, t2);
  feMul(t1, t2, t1);
  feSq(t1, t1);
  for (int i = 0; i < 4; i++) feSq(t1, t1);
  feMul(out, t1, t0);
}

void X25519::feFromBytes(fe out, const uint8_t* s) {
  for (int i = 0; i < 16; i++) {
    out[i] = static_cast<int64_t>(s[2*i]) | (static_cast<int64_t>(s[2*i + 1]) << 8);
  }
  out[15] &= 0x7fff;  // Clear top bit
}

void X25519::feToBytes(uint8_t* s, const fe h) {
  fe t;
  feCopy(t, h);

  // Reduce to canonical form
  for (int j = 0; j < 3; j++) {
    int64_t carry = 0;
    for (int i = 0; i < 16; i++) {
      t[i] += carry;
      carry = t[i] >> 16;
      t[i] &= 0xffff;
    }
    t[0] += 38 * carry;
  }

  // Final reduction
  int64_t carry = 0;
  for (int i = 0; i < 16; i++) {
    t[i] += carry;
    carry = t[i] >> 16;
    t[i] &= 0xffff;
  }
  t[0] += 38 * carry;

  // Check if >= 2^255 - 19 and reduce
  int64_t m = t[15] - 0x7fff;
  for (int i = 14; i >= 0; i--) {
    m |= t[i] - 0xffff;
  }
  m = (m >> 63) + 1;  // 0 if >= p, 1 if < p

  // Conditionally subtract p
  t[0] -= 19 * (1 - m);
  int64_t c = 0;
  for (int i = 0; i < 16; i++) {
    t[i] += c;
    c = t[i] >> 16;
    t[i] &= 0xffff;
  }

  for (int i = 0; i < 16; i++) {
    s[2*i] = static_cast<uint8_t>(t[i] & 0xff);
    s[2*i + 1] = static_cast<uint8_t>((t[i] >> 8) & 0xff);
  }
}

void X25519::scalarmult(uint8_t* q, const uint8_t* n, const uint8_t* p) {
  uint8_t e[32];
  std::memcpy(e, n, 32);

  // Clamp scalar per RFC 7748
  e[0] &= 248;
  e[31] &= 127;
  e[31] |= 64;

  fe x1, x2, z2, x3, z3, tmp0, tmp1;

  feFromBytes(x1, p);
  feOne(x2);
  feZero(z2);
  feCopy(x3, x1);
  feOne(z3);

  int swap = 0;

  // Montgomery ladder
  for (int pos = 254; pos >= 0; pos--) {
    int b = (e[pos / 8] >> (pos & 7)) & 1;
    swap ^= b;
    feCswap(x2, x3, swap);
    feCswap(z2, z3, swap);
    swap = b;

    feSub(tmp0, x3, z3);
    feSub(tmp1, x2, z2);
    feAdd(x2, x2, z2);
    feAdd(z2, x3, z3);
    feMul(z3, tmp0, x2);
    feMul(z2, z2, tmp1);
    feSq(tmp0, tmp1);
    feSq(tmp1, x2);
    feAdd(x3, z3, z2);
    feSub(z2, z3, z2);
    feMul(x2, tmp1, tmp0);
    feSub(tmp1, tmp1, tmp0);
    feSq(z2, z2);

    // tmp0 = 121666 * tmp1
    fe c121666;
    feZero(c121666);
    c121666[0] = 0xDB42;
    c121666[1] = 1;
    feMul(z3, tmp1, c121666);

    feSq(x3, x3);
    feAdd(tmp0, tmp0, z3);
    feMul(z3, x1, z2);
    feMul(z2, tmp1, tmp0);
  }

  feCswap(x2, x3, swap);
  feCswap(z2, z3, swap);

  feInvert(z2, z2);
  feMul(x2, x2, z2);
  feToBytes(q, x2);
}

void X25519::generatePrivateKey(uint8_t* privateKey) {
  secureRandom(privateKey, KEY_SIZE);
  // Clamp as per RFC 7748
  privateKey[0] &= 248;
  privateKey[31] &= 127;
  privateKey[31] |= 64;
}

void X25519::derivePublicKey(const uint8_t* privateKey, uint8_t* publicKey) {
  // Base point for Curve25519: 9
  static const uint8_t basepoint[32] = {9};
  scalarmult(publicKey, privateKey, basepoint);
}

bool X25519::computeSharedSecret(
  const uint8_t* privateKey,
  const uint8_t* peerPublicKey,
  uint8_t* sharedSecret
) {
  scalarmult(sharedSecret, privateKey, peerPublicKey);

  // Check for all-zero output (low-order point attack)
  uint8_t zero = 0;
  for (int i = 0; i < 32; i++) {
    zero |= sharedSecret[i];
  }
  return zero != 0;
}

}  // namespace tls
}  // namespace lightjs
