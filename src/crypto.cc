#include "crypto.h"
#include <sstream>
#include <iomanip>

namespace tinyjs {
namespace crypto {

SHA256::SHA256() {
  reset();
}

void SHA256::reset() {
  state_[0] = 0x6a09e667;
  state_[1] = 0xbb67ae85;
  state_[2] = 0x3c6ef372;
  state_[3] = 0xa54ff53a;
  state_[4] = 0x510e527f;
  state_[5] = 0x9b05688c;
  state_[6] = 0x1f83d9ab;
  state_[7] = 0x5be0cd19;
  bufferLen_ = 0;
  totalLen_ = 0;
}

void SHA256::processBlock() {
  uint32_t w[64];

  for (int i = 0; i < 16; i++) {
    w[i] = getBE32(buffer_ + i * 4);
  }

  for (int i = 16; i < 64; i++) {
    w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
  }

  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];
  uint32_t f = state_[5];
  uint32_t g = state_[6];
  uint32_t h = state_[7];

  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + w[i];
    uint32_t t2 = sigma0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void SHA256::update(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    buffer_[bufferLen_++] = data[i];
    if (bufferLen_ == BLOCK_SIZE) {
      processBlock();
      totalLen_ += BLOCK_SIZE * 8;
      bufferLen_ = 0;
    }
  }
}

void SHA256::final(uint8_t* hash) {
  uint64_t totalBits = totalLen_ + bufferLen_ * 8;

  buffer_[bufferLen_++] = 0x80;

  if (bufferLen_ > 56) {
    while (bufferLen_ < BLOCK_SIZE) {
      buffer_[bufferLen_++] = 0;
    }
    processBlock();
    bufferLen_ = 0;
  }

  while (bufferLen_ < 56) {
    buffer_[bufferLen_++] = 0;
  }

  putBE64(buffer_ + 56, totalBits);
  bufferLen_ = BLOCK_SIZE;
  processBlock();

  for (int i = 0; i < 8; i++) {
    putBE32(hash + i * 4, state_[i]);
  }
}

std::array<uint8_t, SHA256::HASH_SIZE> SHA256::hash(const uint8_t* data, size_t len) {
  SHA256 sha;
  sha.update(data, len);
  std::array<uint8_t, HASH_SIZE> result;
  sha.final(result.data());
  return result;
}

std::string SHA256::hashHex(const uint8_t* data, size_t len) {
  auto h = hash(data, len);
  return toHex(h.data(), h.size());
}

std::array<uint8_t, HMAC::HASH_SIZE> HMAC::compute(
  const uint8_t* key, size_t keyLen,
  const uint8_t* message, size_t messageLen
) {
  constexpr size_t BLOCK_SIZE = 64;
  uint8_t keyPadded[BLOCK_SIZE] = {0};

  if (keyLen > BLOCK_SIZE) {
    auto hashed = SHA256::hash(key, keyLen);
    std::memcpy(keyPadded, hashed.data(), SHA256::HASH_SIZE);
  } else {
    std::memcpy(keyPadded, key, keyLen);
  }

  uint8_t ipad[BLOCK_SIZE];
  uint8_t opad[BLOCK_SIZE];
  for (size_t i = 0; i < BLOCK_SIZE; i++) {
    ipad[i] = keyPadded[i] ^ 0x36;
    opad[i] = keyPadded[i] ^ 0x5c;
  }

  SHA256 sha1;
  sha1.update(ipad, BLOCK_SIZE);
  sha1.update(message, messageLen);
  std::array<uint8_t, SHA256::HASH_SIZE> innerHash;
  sha1.final(innerHash.data());

  SHA256 sha2;
  sha2.update(opad, BLOCK_SIZE);
  sha2.update(innerHash.data(), innerHash.size());
  std::array<uint8_t, HASH_SIZE> result;
  sha2.final(result.data());

  return result;
}

std::string HMAC::computeHex(
  const uint8_t* key, size_t keyLen,
  const uint8_t* message, size_t messageLen
) {
  auto h = compute(key, keyLen, message, messageLen);
  return toHex(h.data(), h.size());
}

std::string toHex(const uint8_t* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; i++) {
    oss << std::setw(2) << static_cast<int>(data[i]);
  }
  return oss.str();
}

std::vector<uint8_t> fromHex(const std::string& hex) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < hex.length(); i += 2) {
    std::string byteString = hex.substr(i, 2);
    uint8_t byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
    result.push_back(byte);
  }
  return result;
}

}
}