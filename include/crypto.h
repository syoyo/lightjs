#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include <vector>
#include <string>

namespace tinyjs {
namespace crypto {

constexpr uint32_t rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

constexpr uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}

constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

constexpr uint32_t sigma0(uint32_t x) {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

constexpr uint32_t sigma1(uint32_t x) {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

constexpr uint32_t gamma0(uint32_t x) {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

constexpr uint32_t gamma1(uint32_t x) {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

class SHA256 {
public:
  static constexpr size_t HASH_SIZE = 32;
  static constexpr size_t BLOCK_SIZE = 64;

  SHA256();
  void update(const uint8_t* data, size_t len);
  void final(uint8_t* hash);
  void reset();

  static std::array<uint8_t, HASH_SIZE> hash(const uint8_t* data, size_t len);
  static std::string hashHex(const uint8_t* data, size_t len);

private:
  void processBlock();

  uint32_t state_[8];
  uint8_t buffer_[BLOCK_SIZE];
  size_t bufferLen_;
  uint64_t totalLen_;

  static constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  };
};

class HMAC {
public:
  static constexpr size_t HASH_SIZE = SHA256::HASH_SIZE;

  static std::array<uint8_t, HASH_SIZE> compute(
    const uint8_t* key, size_t keyLen,
    const uint8_t* message, size_t messageLen
  );

  static std::string computeHex(
    const uint8_t* key, size_t keyLen,
    const uint8_t* message, size_t messageLen
  );
};

inline uint32_t getBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

inline void putBE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

inline void putBE64(uint8_t* p, uint64_t v) {
  putBE32(p, static_cast<uint32_t>(v >> 32));
  putBE32(p + 4, static_cast<uint32_t>(v));
}

std::string toHex(const uint8_t* data, size_t len);
std::vector<uint8_t> fromHex(const std::string& hex);

}
}