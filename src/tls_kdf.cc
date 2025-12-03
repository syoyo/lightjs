#include "tls.h"
#include "crypto.h"

namespace lightjs {
namespace tls {

// HKDF-Extract: PRK = HMAC-Hash(salt, IKM)
std::vector<uint8_t> HKDF::extract(
  const uint8_t* salt, size_t saltLen,
  const uint8_t* ikm, size_t ikmLen
) {
  // If salt is not provided, use zeros
  std::vector<uint8_t> actualSalt;
  if (saltLen == 0 || salt == nullptr) {
    actualSalt.resize(32, 0);
    salt = actualSalt.data();
    saltLen = 32;
  }

  auto result = crypto::HMAC::compute(salt, saltLen, ikm, ikmLen);
  return std::vector<uint8_t>(result.begin(), result.end());
}

// HKDF-Expand: OKM = T(1) || T(2) || ... where T(i) = HMAC-Hash(PRK, T(i-1) || info || i)
std::vector<uint8_t> HKDF::expand(
  const uint8_t* prk, size_t prkLen,
  const uint8_t* info, size_t infoLen,
  size_t length
) {
  std::vector<uint8_t> okm;
  okm.reserve(length);

  size_t hashLen = 32;  // SHA-256
  size_t n = (length + hashLen - 1) / hashLen;

  std::vector<uint8_t> t;
  std::vector<uint8_t> input;

  for (size_t i = 1; i <= n; i++) {
    input.clear();
    input.insert(input.end(), t.begin(), t.end());
    if (info && infoLen > 0) {
      input.insert(input.end(), info, info + infoLen);
    }
    input.push_back(static_cast<uint8_t>(i));

    auto hash = crypto::HMAC::compute(prk, prkLen, input.data(), input.size());
    t.assign(hash.begin(), hash.end());
    okm.insert(okm.end(), t.begin(), t.end());
  }

  okm.resize(length);
  return okm;
}

// HKDF-Expand-Label for TLS 1.3
// HkdfLabel = struct {
//   uint16 length;
//   opaque label<7..255> = "tls13 " + Label;
//   opaque context<0..255>;
// }
std::vector<uint8_t> HKDF::expandLabel(
  const uint8_t* secret, size_t secretLen,
  const std::string& label,
  const uint8_t* context, size_t contextLen,
  size_t length
) {
  std::string fullLabel = "tls13 " + label;

  std::vector<uint8_t> hkdfLabel;
  hkdfLabel.reserve(2 + 1 + fullLabel.size() + 1 + contextLen);

  // Length (2 bytes, big-endian)
  hkdfLabel.push_back(static_cast<uint8_t>((length >> 8) & 0xff));
  hkdfLabel.push_back(static_cast<uint8_t>(length & 0xff));

  // Label length + label
  hkdfLabel.push_back(static_cast<uint8_t>(fullLabel.size()));
  hkdfLabel.insert(hkdfLabel.end(), fullLabel.begin(), fullLabel.end());

  // Context length + context
  hkdfLabel.push_back(static_cast<uint8_t>(contextLen));
  if (context && contextLen > 0) {
    hkdfLabel.insert(hkdfLabel.end(), context, context + contextLen);
  }

  return expand(secret, secretLen, hkdfLabel.data(), hkdfLabel.size(), length);
}

// Derive-Secret for TLS 1.3
// Derive-Secret(Secret, Label, Messages) = HKDF-Expand-Label(Secret, Label, Transcript-Hash(Messages), Hash.length)
std::vector<uint8_t> HKDF::deriveSecret(
  const uint8_t* secret, size_t secretLen,
  const std::string& label,
  const uint8_t* messages, size_t messagesLen
) {
  // Compute transcript hash
  auto transcriptHash = crypto::SHA256::hash(messages, messagesLen);
  return expandLabel(secret, secretLen, label, transcriptHash.data(), transcriptHash.size(), 32);
}

// PRF for TLS 1.2 (P_SHA256)
// PRF(secret, label, seed) = P_SHA256(secret, label + seed)
std::vector<uint8_t> PRF::compute(
  const uint8_t* secret, size_t secretLen,
  const std::string& label,
  const uint8_t* seed, size_t seedLen,
  size_t length
) {
  // Concatenate label and seed
  std::vector<uint8_t> labelSeed;
  labelSeed.insert(labelSeed.end(), label.begin(), label.end());
  labelSeed.insert(labelSeed.end(), seed, seed + seedLen);

  std::vector<uint8_t> result;
  result.reserve(length);

  // A(0) = seed
  // A(i) = HMAC_hash(secret, A(i-1))
  // P_hash(secret, seed) = HMAC_hash(secret, A(1) + seed) +
  //                        HMAC_hash(secret, A(2) + seed) + ...

  std::vector<uint8_t> a = labelSeed;  // A(0)

  while (result.size() < length) {
    // A(i) = HMAC(secret, A(i-1))
    auto aNext = crypto::HMAC::compute(secret, secretLen, a.data(), a.size());
    a.assign(aNext.begin(), aNext.end());

    // P_hash block = HMAC(secret, A(i) + seed)
    std::vector<uint8_t> input;
    input.insert(input.end(), a.begin(), a.end());
    input.insert(input.end(), labelSeed.begin(), labelSeed.end());

    auto block = crypto::HMAC::compute(secret, secretLen, input.data(), input.size());
    result.insert(result.end(), block.begin(), block.end());
  }

  result.resize(length);
  return result;
}

}  // namespace tls
}  // namespace lightjs
