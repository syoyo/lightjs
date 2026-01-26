#include "tls.h"

namespace lightjs {
namespace tls {

// GF(2^128) multiplication for GHASH
void GCM::gfMult(const uint8_t* X, const uint8_t* Y, uint8_t* Z) {
  uint8_t V[16];
  std::memcpy(V, Y, 16);
  std::memset(Z, 0, 16);

  for (int i = 0; i < 16; i++) {
    for (int j = 7; j >= 0; j--) {
      if ((X[i] >> j) & 1) {
        for (int k = 0; k < 16; k++) {
          Z[k] ^= V[k];
        }
      }

      // Check if LSB of V is set (need reduction)
      bool lsb = V[15] & 1;

      // Right shift V by 1
      for (int k = 15; k > 0; k--) {
        V[k] = (V[k] >> 1) | ((V[k-1] & 1) << 7);
      }
      V[0] >>= 1;

      // XOR with R if LSB was set (R = 0xE1 || 0^120)
      if (lsb) {
        V[0] ^= 0xE1;
      }
    }
  }
}

// GHASH function
void GCM::ghash(const uint8_t* H, const uint8_t* data, size_t len, uint8_t* out) {
  uint8_t Y[16] = {0};
  uint8_t temp[16];

  size_t blocks = len / 16;
  for (size_t i = 0; i < blocks; i++) {
    for (int j = 0; j < 16; j++) {
      Y[j] ^= data[i * 16 + j];
    }
    gfMult(Y, H, temp);
    std::memcpy(Y, temp, 16);
  }

  // Handle partial block
  size_t remaining = len % 16;
  if (remaining > 0) {
    for (size_t j = 0; j < remaining; j++) {
      Y[j] ^= data[blocks * 16 + j];
    }
    gfMult(Y, H, temp);
    std::memcpy(Y, temp, 16);
  }

  std::memcpy(out, Y, 16);
}

// Increment counter
void GCM::incr(uint8_t* counter) {
  for (int i = 15; i >= 12; i--) {
    if (++counter[i] != 0) break;
  }
}

bool GCM::encrypt128(
  const uint8_t* key,
  const uint8_t* nonce,
  const uint8_t* plaintext, size_t plaintextLen,
  const uint8_t* aad, size_t aadLen,
  uint8_t* ciphertext,
  uint8_t* tag
) {
  AES128 aes(key);

  // Generate H = AES(K, 0^128)
  uint8_t H[16] = {0};
  aes.encryptBlock(H, H);

  // Initialize counter: nonce || 0^31 || 1
  uint8_t counter[16];
  std::memcpy(counter, nonce, 12);
  counter[12] = 0;
  counter[13] = 0;
  counter[14] = 0;
  counter[15] = 1;

  // Encrypt counter for final XOR with tag
  uint8_t E_K_Y0[16];
  aes.encryptBlock(counter, E_K_Y0);

  // Encrypt plaintext with CTR mode
  incr(counter);
  size_t blocks = (plaintextLen + 15) / 16;
  for (size_t i = 0; i < blocks; i++) {
    uint8_t keystream[16];
    aes.encryptBlock(counter, keystream);
    incr(counter);

    size_t blockLen = (i == blocks - 1 && plaintextLen % 16 != 0)
                      ? plaintextLen % 16 : 16;
    for (size_t j = 0; j < blockLen; j++) {
      ciphertext[i * 16 + j] = plaintext[i * 16 + j] ^ keystream[j];
    }
  }

  // Build data for GHASH: AAD || pad || C || pad || len(AAD) || len(C)
  size_t aadPadLen = (16 - (aadLen % 16)) % 16;
  size_t cPadLen = (16 - (plaintextLen % 16)) % 16;
  size_t ghashLen = aadLen + aadPadLen + plaintextLen + cPadLen + 16;

  std::vector<uint8_t> ghashData(ghashLen, 0);
  std::memcpy(ghashData.data(), aad, aadLen);
  std::memcpy(ghashData.data() + aadLen + aadPadLen, ciphertext, plaintextLen);

  // Length block (bit lengths as 64-bit big-endian)
  uint64_t aadBitLen = aadLen * 8;
  uint64_t cBitLen = plaintextLen * 8;
  size_t lenOffset = aadLen + aadPadLen + plaintextLen + cPadLen;
  for (int i = 0; i < 8; i++) {
    ghashData[lenOffset + i] = (aadBitLen >> (56 - i * 8)) & 0xff;
    ghashData[lenOffset + 8 + i] = (cBitLen >> (56 - i * 8)) & 0xff;
  }

  // Compute GHASH
  uint8_t S[16];
  ghash(H, ghashData.data(), ghashLen, S);

  // Tag = GHASH ^ E(K, Y0)
  for (int i = 0; i < 16; i++) {
    tag[i] = S[i] ^ E_K_Y0[i];
  }

  return true;
}

bool GCM::decrypt128(
  const uint8_t* key,
  const uint8_t* nonce,
  const uint8_t* ciphertext, size_t ciphertextLen,
  const uint8_t* aad, size_t aadLen,
  const uint8_t* tag,
  uint8_t* plaintext
) {
  AES128 aes(key);

  // Generate H
  uint8_t H[16] = {0};
  aes.encryptBlock(H, H);

  // Initialize counter
  uint8_t counter[16];
  std::memcpy(counter, nonce, 12);
  counter[12] = 0;
  counter[13] = 0;
  counter[14] = 0;
  counter[15] = 1;

  uint8_t E_K_Y0[16];
  aes.encryptBlock(counter, E_K_Y0);

  // Verify tag first
  size_t aadPadLen = (16 - (aadLen % 16)) % 16;
  size_t cPadLen = (16 - (ciphertextLen % 16)) % 16;
  size_t ghashLen = aadLen + aadPadLen + ciphertextLen + cPadLen + 16;

  std::vector<uint8_t> ghashData(ghashLen, 0);
  std::memcpy(ghashData.data(), aad, aadLen);
  std::memcpy(ghashData.data() + aadLen + aadPadLen, ciphertext, ciphertextLen);

  uint64_t aadBitLen = aadLen * 8;
  uint64_t cBitLen = ciphertextLen * 8;
  size_t lenOffset = aadLen + aadPadLen + ciphertextLen + cPadLen;
  for (int i = 0; i < 8; i++) {
    ghashData[lenOffset + i] = (aadBitLen >> (56 - i * 8)) & 0xff;
    ghashData[lenOffset + 8 + i] = (cBitLen >> (56 - i * 8)) & 0xff;
  }

  uint8_t S[16];
  ghash(H, ghashData.data(), ghashLen, S);

  uint8_t computedTag[16];
  for (int i = 0; i < 16; i++) {
    computedTag[i] = S[i] ^ E_K_Y0[i];
  }

  // Constant-time comparison
  uint8_t diff = 0;
  for (int i = 0; i < 16; i++) {
    diff |= computedTag[i] ^ tag[i];
  }
  if (diff != 0) {
    return false;  // Authentication failed
  }

  // Decrypt
  incr(counter);
  size_t blocks = (ciphertextLen + 15) / 16;
  for (size_t i = 0; i < blocks; i++) {
    uint8_t keystream[16];
    aes.encryptBlock(counter, keystream);
    incr(counter);

    size_t blockLen = (i == blocks - 1 && ciphertextLen % 16 != 0)
                      ? ciphertextLen % 16 : 16;
    for (size_t j = 0; j < blockLen; j++) {
      plaintext[i * 16 + j] = ciphertext[i * 16 + j] ^ keystream[j];
    }
  }

  return true;
}

bool GCM::encrypt256(
  const uint8_t* key,
  const uint8_t* nonce,
  const uint8_t* plaintext, size_t plaintextLen,
  const uint8_t* aad, size_t aadLen,
  uint8_t* ciphertext,
  uint8_t* tag
) {
  AES256 aes(key);

  uint8_t H[16] = {0};
  aes.encryptBlock(H, H);

  uint8_t counter[16];
  std::memcpy(counter, nonce, 12);
  counter[12] = 0;
  counter[13] = 0;
  counter[14] = 0;
  counter[15] = 1;

  uint8_t E_K_Y0[16];
  aes.encryptBlock(counter, E_K_Y0);

  incr(counter);
  size_t blocks = (plaintextLen + 15) / 16;
  for (size_t i = 0; i < blocks; i++) {
    uint8_t keystream[16];
    aes.encryptBlock(counter, keystream);
    incr(counter);

    size_t blockLen = (i == blocks - 1 && plaintextLen % 16 != 0)
                      ? plaintextLen % 16 : 16;
    for (size_t j = 0; j < blockLen; j++) {
      ciphertext[i * 16 + j] = plaintext[i * 16 + j] ^ keystream[j];
    }
  }

  size_t aadPadLen = (16 - (aadLen % 16)) % 16;
  size_t cPadLen = (16 - (plaintextLen % 16)) % 16;
  size_t ghashLen = aadLen + aadPadLen + plaintextLen + cPadLen + 16;

  std::vector<uint8_t> ghashData(ghashLen, 0);
  std::memcpy(ghashData.data(), aad, aadLen);
  std::memcpy(ghashData.data() + aadLen + aadPadLen, ciphertext, plaintextLen);

  uint64_t aadBitLen = aadLen * 8;
  uint64_t cBitLen = plaintextLen * 8;
  size_t lenOffset = aadLen + aadPadLen + plaintextLen + cPadLen;
  for (int i = 0; i < 8; i++) {
    ghashData[lenOffset + i] = (aadBitLen >> (56 - i * 8)) & 0xff;
    ghashData[lenOffset + 8 + i] = (cBitLen >> (56 - i * 8)) & 0xff;
  }

  uint8_t S[16];
  ghash(H, ghashData.data(), ghashLen, S);

  for (int i = 0; i < 16; i++) {
    tag[i] = S[i] ^ E_K_Y0[i];
  }

  return true;
}

bool GCM::decrypt256(
  const uint8_t* key,
  const uint8_t* nonce,
  const uint8_t* ciphertext, size_t ciphertextLen,
  const uint8_t* aad, size_t aadLen,
  const uint8_t* tag,
  uint8_t* plaintext
) {
  AES256 aes(key);

  uint8_t H[16] = {0};
  aes.encryptBlock(H, H);

  uint8_t counter[16];
  std::memcpy(counter, nonce, 12);
  counter[12] = 0;
  counter[13] = 0;
  counter[14] = 0;
  counter[15] = 1;

  uint8_t E_K_Y0[16];
  aes.encryptBlock(counter, E_K_Y0);

  // Verify tag
  size_t aadPadLen = (16 - (aadLen % 16)) % 16;
  size_t cPadLen = (16 - (ciphertextLen % 16)) % 16;
  size_t ghashLen = aadLen + aadPadLen + ciphertextLen + cPadLen + 16;

  std::vector<uint8_t> ghashData(ghashLen, 0);
  std::memcpy(ghashData.data(), aad, aadLen);
  std::memcpy(ghashData.data() + aadLen + aadPadLen, ciphertext, ciphertextLen);

  uint64_t aadBitLen = aadLen * 8;
  uint64_t cBitLen = ciphertextLen * 8;
  size_t lenOffset = aadLen + aadPadLen + ciphertextLen + cPadLen;
  for (int i = 0; i < 8; i++) {
    ghashData[lenOffset + i] = (aadBitLen >> (56 - i * 8)) & 0xff;
    ghashData[lenOffset + 8 + i] = (cBitLen >> (56 - i * 8)) & 0xff;
  }

  uint8_t S[16];
  ghash(H, ghashData.data(), ghashLen, S);

  uint8_t computedTag[16];
  for (int i = 0; i < 16; i++) {
    computedTag[i] = S[i] ^ E_K_Y0[i];
  }

  uint8_t diff = 0;
  for (int i = 0; i < 16; i++) {
    diff |= computedTag[i] ^ tag[i];
  }
  if (diff != 0) {
    return false;
  }

  incr(counter);
  size_t blocks = (ciphertextLen + 15) / 16;
  for (size_t i = 0; i < blocks; i++) {
    uint8_t keystream[16];
    aes.encryptBlock(counter, keystream);
    incr(counter);

    size_t blockLen = (i == blocks - 1 && ciphertextLen % 16 != 0)
                      ? ciphertextLen % 16 : 16;
    for (size_t j = 0; j < blockLen; j++) {
      plaintext[i * 16 + j] = ciphertext[i * 16 + j] ^ keystream[j];
    }
  }

  return true;
}

}  // namespace tls
}  // namespace lightjs
