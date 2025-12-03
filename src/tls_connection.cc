#include "tls.h"
#include "crypto.h"

namespace lightjs {
namespace tls {

// TLS Record serialization
std::vector<uint8_t> TLSRecord::serialize() const {
  std::vector<uint8_t> data;
  data.push_back(static_cast<uint8_t>(type));
  data.push_back(static_cast<uint8_t>((static_cast<uint16_t>(version) >> 8) & 0xff));
  data.push_back(static_cast<uint8_t>(static_cast<uint16_t>(version) & 0xff));
  data.push_back(static_cast<uint8_t>((fragment.size() >> 8) & 0xff));
  data.push_back(static_cast<uint8_t>(fragment.size() & 0xff));
  data.insert(data.end(), fragment.begin(), fragment.end());
  return data;
}

bool TLSRecord::parse(const uint8_t* data, size_t len, TLSRecord& record, size_t& consumed) {
  if (len < 5) return false;

  record.type = static_cast<ContentType>(data[0]);
  record.version = static_cast<TLSVersion>((static_cast<uint16_t>(data[1]) << 8) | data[2]);
  size_t fragLen = (static_cast<size_t>(data[3]) << 8) | data[4];

  if (len < 5 + fragLen) return false;

  record.fragment.assign(data + 5, data + 5 + fragLen);
  consumed = 5 + fragLen;
  return true;
}

TLSConnection::TLSConnection(SendCallback send, RecvCallback recv)
  : sendCallback_(send), recvCallback_(recv),
    version_(TLSVersion::TLS_1_2),
    cipherSuite_(CipherSuite::TLS_AES_128_GCM_SHA256),
    handshakeComplete_(false), isEncrypted_(false),
    clientSeqNum_(0), serverSeqNum_(0) {
  clientRandom_.fill(0);
  serverRandom_.fill(0);
  privateKey_.fill(0);
  publicKey_.fill(0);
  sharedSecret_.fill(0);
}

TLSConnection::~TLSConnection() {
  // Clear sensitive data
  std::fill(privateKey_.begin(), privateKey_.end(), 0);
  std::fill(sharedSecret_.begin(), sharedSecret_.end(), 0);
  std::fill(clientKey_.begin(), clientKey_.end(), 0);
  std::fill(serverKey_.begin(), serverKey_.end(), 0);
}

void TLSConnection::generateRandom(uint8_t* buffer, size_t len) {
  secureRandom(buffer, len);
}

void TLSConnection::updateTranscript(const uint8_t* data, size_t len) {
  transcriptData_.insert(transcriptData_.end(), data, data + len);
}

std::vector<uint8_t> TLSConnection::getTranscriptHash() {
  auto hash = crypto::SHA256::hash(transcriptData_.data(), transcriptData_.size());
  return std::vector<uint8_t>(hash.begin(), hash.end());
}

bool TLSConnection::sendRecord(ContentType type, const std::vector<uint8_t>& data) {
  TLSRecord record;
  record.type = type;
  record.version = TLSVersion::TLS_1_2;  // Always use 1.2 for record layer
  record.fragment = data;

  auto serialized = record.serialize();
  return sendCallback_(serialized.data(), serialized.size());
}

bool TLSConnection::receiveRecord(TLSRecord& record) {
  // Read header first
  uint8_t header[5];
  size_t headerRead = 0;

  while (headerRead < 5) {
    // Try from buffer first
    while (headerRead < 5 && !recvBuffer_.empty()) {
      header[headerRead++] = recvBuffer_.front();
      recvBuffer_.erase(recvBuffer_.begin());
    }

    if (headerRead < 5) {
      uint8_t temp[256];
      int n = recvCallback_(temp, sizeof(temp));
      if (n <= 0) return false;
      recvBuffer_.insert(recvBuffer_.end(), temp, temp + n);
    }
  }

  size_t fragLen = (static_cast<size_t>(header[3]) << 8) | header[4];
  if (fragLen > 16384 + 256) {  // Max record size with expansion
    lastError_ = "Record too large";
    return false;
  }

  record.type = static_cast<ContentType>(header[0]);
  record.version = static_cast<TLSVersion>((static_cast<uint16_t>(header[1]) << 8) | header[2]);
  record.fragment.resize(fragLen);

  size_t fragRead = 0;
  while (fragRead < fragLen) {
    while (fragRead < fragLen && !recvBuffer_.empty()) {
      record.fragment[fragRead++] = recvBuffer_.front();
      recvBuffer_.erase(recvBuffer_.begin());
    }

    if (fragRead < fragLen) {
      uint8_t temp[4096];
      int n = recvCallback_(temp, sizeof(temp));
      if (n <= 0) return false;
      recvBuffer_.insert(recvBuffer_.end(), temp, temp + n);
    }
  }

  return true;
}

bool TLSConnection::sendEncryptedRecord(ContentType type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> ciphertext;
  if (!encrypt(data, ciphertext, type)) {
    return false;
  }

  ContentType recordType = (version_ == TLSVersion::TLS_1_3)
    ? ContentType::APPLICATION_DATA
    : type;

  return sendRecord(recordType, ciphertext);
}

bool TLSConnection::receiveEncryptedRecord(TLSRecord& record) {
  TLSRecord encryptedRecord;
  if (!receiveRecord(encryptedRecord)) {
    return false;
  }

  std::vector<uint8_t> plaintext;
  if (!decrypt(encryptedRecord.fragment, plaintext, encryptedRecord.type)) {
    lastError_ = "Decryption failed";
    return false;
  }

  if (version_ == TLSVersion::TLS_1_3) {
    // In TLS 1.3, actual content type is at the end
    if (plaintext.empty()) {
      lastError_ = "Empty plaintext";
      return false;
    }

    // Remove padding zeros
    while (!plaintext.empty() && plaintext.back() == 0) {
      plaintext.pop_back();
    }

    if (plaintext.empty()) {
      lastError_ = "No content type";
      return false;
    }

    record.type = static_cast<ContentType>(plaintext.back());
    plaintext.pop_back();
  } else {
    record.type = encryptedRecord.type;
  }

  record.version = encryptedRecord.version;
  record.fragment = plaintext;
  return true;
}

bool TLSConnection::encrypt(const std::vector<uint8_t>& plaintext,
                            std::vector<uint8_t>& ciphertext,
                            ContentType type) {
  // Build nonce: IV XOR sequence number
  std::vector<uint8_t> nonce = clientIV_;
  for (int i = 0; i < 8; i++) {
    nonce[nonce.size() - 1 - i] ^= (clientSeqNum_ >> (i * 8)) & 0xff;
  }

  std::vector<uint8_t> input = plaintext;

  // TLS 1.3: append content type
  if (version_ == TLSVersion::TLS_1_3) {
    input.push_back(static_cast<uint8_t>(type));
  }

  ciphertext.resize(input.size() + GCM::TAG_SIZE);

  // Build AAD
  std::vector<uint8_t> aad;
  if (version_ == TLSVersion::TLS_1_3) {
    // TLS 1.3 AAD: record header
    aad.push_back(static_cast<uint8_t>(ContentType::APPLICATION_DATA));
    aad.push_back(0x03);
    aad.push_back(0x03);
    uint16_t len = static_cast<uint16_t>(input.size() + GCM::TAG_SIZE);
    aad.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    aad.push_back(static_cast<uint8_t>(len & 0xff));
  } else {
    // TLS 1.2 AAD: seq_num || type || version || length
    for (int i = 7; i >= 0; i--) {
      aad.push_back((clientSeqNum_ >> (i * 8)) & 0xff);
    }
    aad.push_back(static_cast<uint8_t>(type));
    aad.push_back(0x03);
    aad.push_back(0x03);
    uint16_t len = static_cast<uint16_t>(plaintext.size());
    aad.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    aad.push_back(static_cast<uint8_t>(len & 0xff));
  }

  bool ok;
  if (clientKey_.size() == 16) {
    ok = GCM::encrypt128(
      clientKey_.data(), nonce.data(),
      input.data(), input.size(),
      aad.data(), aad.size(),
      ciphertext.data(),
      ciphertext.data() + input.size()
    );
  } else {
    ok = GCM::encrypt256(
      clientKey_.data(), nonce.data(),
      input.data(), input.size(),
      aad.data(), aad.size(),
      ciphertext.data(),
      ciphertext.data() + input.size()
    );
  }

  if (ok) clientSeqNum_++;
  return ok;
}

bool TLSConnection::decrypt(const std::vector<uint8_t>& ciphertext,
                            std::vector<uint8_t>& plaintext,
                            ContentType type) {
  if (ciphertext.size() < GCM::TAG_SIZE) {
    return false;
  }

  // Build nonce
  std::vector<uint8_t> nonce = serverIV_;
  for (int i = 0; i < 8; i++) {
    nonce[nonce.size() - 1 - i] ^= (serverSeqNum_ >> (i * 8)) & 0xff;
  }

  size_t plaintextLen = ciphertext.size() - GCM::TAG_SIZE;
  plaintext.resize(plaintextLen);

  // Build AAD
  std::vector<uint8_t> aad;
  if (version_ == TLSVersion::TLS_1_3) {
    aad.push_back(static_cast<uint8_t>(ContentType::APPLICATION_DATA));
    aad.push_back(0x03);
    aad.push_back(0x03);
    uint16_t len = static_cast<uint16_t>(ciphertext.size());
    aad.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    aad.push_back(static_cast<uint8_t>(len & 0xff));
  } else {
    for (int i = 7; i >= 0; i--) {
      aad.push_back((serverSeqNum_ >> (i * 8)) & 0xff);
    }
    aad.push_back(static_cast<uint8_t>(type));
    aad.push_back(0x03);
    aad.push_back(0x03);
    uint16_t len = static_cast<uint16_t>(plaintextLen);
    aad.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    aad.push_back(static_cast<uint8_t>(len & 0xff));
  }

  bool ok;
  if (serverKey_.size() == 16) {
    ok = GCM::decrypt128(
      serverKey_.data(), nonce.data(),
      ciphertext.data(), plaintextLen,
      aad.data(), aad.size(),
      ciphertext.data() + plaintextLen,
      plaintext.data()
    );
  } else {
    ok = GCM::decrypt256(
      serverKey_.data(), nonce.data(),
      ciphertext.data(), plaintextLen,
      aad.data(), aad.size(),
      ciphertext.data() + plaintextLen,
      plaintext.data()
    );
  }

  if (ok) serverSeqNum_++;
  return ok;
}

void TLSConnection::deriveTrafficKeys(const std::vector<uint8_t>& secret,
                                       std::vector<uint8_t>& key,
                                       std::vector<uint8_t>& iv) {
  size_t keyLen = (cipherSuite_ == CipherSuite::TLS_AES_256_GCM_SHA384 ||
                   cipherSuite_ == CipherSuite::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384)
    ? 32 : 16;

  key = HKDF::expandLabel(secret.data(), secret.size(), "key", nullptr, 0, keyLen);
  iv = HKDF::expandLabel(secret.data(), secret.size(), "iv", nullptr, 0, 12);
}

void TLSConnection::deriveKeysTLS13() {
  // Early Secret
  std::vector<uint8_t> zeros(32, 0);
  auto earlySecret = HKDF::extract(nullptr, 0, zeros.data(), zeros.size());

  // Derive Handshake Secret
  auto derivedSecret = HKDF::deriveSecret(earlySecret.data(), earlySecret.size(),
                                          "derived", nullptr, 0);
  auto handshakeSecret = HKDF::extract(derivedSecret.data(), derivedSecret.size(),
                                       sharedSecret_.data(), sharedSecret_.size());

  // Client/Server Handshake Traffic Secrets
  auto transcriptHash = getTranscriptHash();
  clientHandshakeSecret_ = HKDF::deriveSecret(handshakeSecret.data(), handshakeSecret.size(),
                                               "c hs traffic",
                                               transcriptHash.data(), transcriptHash.size());
  serverHandshakeSecret_ = HKDF::deriveSecret(handshakeSecret.data(), handshakeSecret.size(),
                                               "s hs traffic",
                                               transcriptHash.data(), transcriptHash.size());

  // Derive handshake keys
  deriveTrafficKeys(clientHandshakeSecret_, clientKey_, clientIV_);
  deriveTrafficKeys(serverHandshakeSecret_, serverKey_, serverIV_);

  // Derive Master Secret for later
  auto derivedSecret2 = HKDF::deriveSecret(handshakeSecret.data(), handshakeSecret.size(),
                                           "derived", nullptr, 0);
  masterSecret_ = HKDF::extract(derivedSecret2.data(), derivedSecret2.size(),
                                zeros.data(), zeros.size());
}

void TLSConnection::deriveKeysTLS12() {
  // Combine randoms
  std::vector<uint8_t> seed;
  seed.insert(seed.end(), clientRandom_.begin(), clientRandom_.end());
  seed.insert(seed.end(), serverRandom_.begin(), serverRandom_.end());

  // Master secret = PRF(pre_master_secret, "master secret", ClientHello.random + ServerHello.random)
  masterSecret_ = PRF::compute(sharedSecret_.data(), sharedSecret_.size(),
                               "master secret", seed.data(), seed.size(), 48);

  // Key expansion
  std::vector<uint8_t> keySeed;
  keySeed.insert(keySeed.end(), serverRandom_.begin(), serverRandom_.end());
  keySeed.insert(keySeed.end(), clientRandom_.begin(), clientRandom_.end());

  size_t keyLen = (cipherSuite_ == CipherSuite::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 ||
                   cipherSuite_ == CipherSuite::TLS_RSA_WITH_AES_256_GCM_SHA384)
    ? 32 : 16;

  // client_write_key || server_write_key || client_write_IV || server_write_IV
  size_t keyBlockLen = keyLen * 2 + 8;  // 4-byte implicit IVs for GCM
  auto keyBlock = PRF::compute(masterSecret_.data(), masterSecret_.size(),
                               "key expansion", keySeed.data(), keySeed.size(), keyBlockLen);

  size_t offset = 0;
  clientKey_.assign(keyBlock.begin() + offset, keyBlock.begin() + offset + keyLen);
  offset += keyLen;
  serverKey_.assign(keyBlock.begin() + offset, keyBlock.begin() + offset + keyLen);
  offset += keyLen;

  // For GCM, we need 12-byte nonces; TLS 1.2 uses implicit + explicit
  clientIV_.assign(keyBlock.begin() + offset, keyBlock.begin() + offset + 4);
  clientIV_.resize(12, 0);  // Will be filled with explicit nonce
  offset += 4;
  serverIV_.assign(keyBlock.begin() + offset, keyBlock.begin() + offset + 4);
  serverIV_.resize(12, 0);
}

bool TLSConnection::sendClientHello() {
  generateRandom(clientRandom_.data(), clientRandom_.size());

  // Generate X25519 key pair
  X25519::generatePrivateKey(privateKey_.data());
  X25519::derivePublicKey(privateKey_.data(), publicKey_.data());

  std::vector<uint8_t> hello;

  // Handshake type
  hello.push_back(static_cast<uint8_t>(HandshakeType::CLIENT_HELLO));

  // Length placeholder (3 bytes)
  size_t lenPos = hello.size();
  hello.push_back(0); hello.push_back(0); hello.push_back(0);

  // Client version (TLS 1.2 for compatibility)
  hello.push_back(0x03); hello.push_back(0x03);

  // Random
  hello.insert(hello.end(), clientRandom_.begin(), clientRandom_.end());

  // Session ID (empty)
  hello.push_back(0);

  // Cipher suites
  std::vector<uint16_t> suites = {
    static_cast<uint16_t>(CipherSuite::TLS_AES_128_GCM_SHA256),
    static_cast<uint16_t>(CipherSuite::TLS_AES_256_GCM_SHA384),
    static_cast<uint16_t>(CipherSuite::TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256),
    static_cast<uint16_t>(CipherSuite::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384)
  };
  hello.push_back(static_cast<uint8_t>((suites.size() * 2) >> 8));
  hello.push_back(static_cast<uint8_t>((suites.size() * 2) & 0xff));
  for (auto suite : suites) {
    hello.push_back(static_cast<uint8_t>((suite >> 8) & 0xff));
    hello.push_back(static_cast<uint8_t>(suite & 0xff));
  }

  // Compression methods (null only)
  hello.push_back(1);
  hello.push_back(0);

  // Extensions
  std::vector<uint8_t> extensions;

  // SNI extension
  if (!hostname_.empty()) {
    std::vector<uint8_t> sni;
    sni.push_back(0);  // Host name type
    sni.push_back(static_cast<uint8_t>((hostname_.size() >> 8) & 0xff));
    sni.push_back(static_cast<uint8_t>(hostname_.size() & 0xff));
    sni.insert(sni.end(), hostname_.begin(), hostname_.end());

    extensions.push_back(0x00); extensions.push_back(0x00);  // SNI type
    uint16_t sniListLen = static_cast<uint16_t>(sni.size());
    uint16_t sniExtLen = sniListLen + 2;
    extensions.push_back(static_cast<uint8_t>((sniExtLen >> 8) & 0xff));
    extensions.push_back(static_cast<uint8_t>(sniExtLen & 0xff));
    extensions.push_back(static_cast<uint8_t>((sniListLen >> 8) & 0xff));
    extensions.push_back(static_cast<uint8_t>(sniListLen & 0xff));
    extensions.insert(extensions.end(), sni.begin(), sni.end());
  }

  // Supported versions extension (TLS 1.3)
  extensions.push_back(0x00); extensions.push_back(0x2b);  // Type
  extensions.push_back(0x00); extensions.push_back(0x03);  // Length
  extensions.push_back(0x02);  // Versions length
  extensions.push_back(0x03); extensions.push_back(0x04);  // TLS 1.3

  // Supported groups extension
  extensions.push_back(0x00); extensions.push_back(0x0a);  // Type
  extensions.push_back(0x00); extensions.push_back(0x04);  // Length
  extensions.push_back(0x00); extensions.push_back(0x02);  // Groups length
  extensions.push_back(0x00); extensions.push_back(0x1d);  // X25519

  // Signature algorithms extension
  extensions.push_back(0x00); extensions.push_back(0x0d);  // Type
  extensions.push_back(0x00); extensions.push_back(0x08);  // Length
  extensions.push_back(0x00); extensions.push_back(0x06);  // Algos length
  extensions.push_back(0x04); extensions.push_back(0x01);  // RSA_PKCS1_SHA256
  extensions.push_back(0x04); extensions.push_back(0x03);  // ECDSA_SECP256R1_SHA256
  extensions.push_back(0x08); extensions.push_back(0x04);  // RSA_PSS_RSAE_SHA256

  // Key share extension (X25519)
  extensions.push_back(0x00); extensions.push_back(0x33);  // Type
  uint16_t keyShareLen = 2 + 2 + 32;  // group + key_len + key
  extensions.push_back(static_cast<uint8_t>((keyShareLen + 2) >> 8));
  extensions.push_back(static_cast<uint8_t>((keyShareLen + 2) & 0xff));
  extensions.push_back(static_cast<uint8_t>((keyShareLen) >> 8));
  extensions.push_back(static_cast<uint8_t>((keyShareLen) & 0xff));
  extensions.push_back(0x00); extensions.push_back(0x1d);  // X25519
  extensions.push_back(0x00); extensions.push_back(0x20);  // 32 bytes
  extensions.insert(extensions.end(), publicKey_.begin(), publicKey_.end());

  // Add extensions to hello
  hello.push_back(static_cast<uint8_t>((extensions.size() >> 8) & 0xff));
  hello.push_back(static_cast<uint8_t>(extensions.size() & 0xff));
  hello.insert(hello.end(), extensions.begin(), extensions.end());

  // Fix length
  size_t helloLen = hello.size() - 4;
  hello[lenPos] = static_cast<uint8_t>((helloLen >> 16) & 0xff);
  hello[lenPos + 1] = static_cast<uint8_t>((helloLen >> 8) & 0xff);
  hello[lenPos + 2] = static_cast<uint8_t>(helloLen & 0xff);

  updateTranscript(hello.data(), hello.size());
  return sendRecord(ContentType::HANDSHAKE, hello);
}

bool TLSConnection::receiveServerHello() {
  TLSRecord record;
  if (!receiveRecord(record)) {
    lastError_ = "Failed to receive ServerHello";
    return false;
  }

  if (record.type != ContentType::HANDSHAKE || record.fragment.empty()) {
    lastError_ = "Unexpected record type";
    return false;
  }

  const uint8_t* p = record.fragment.data();
  const uint8_t* end = p + record.fragment.size();

  if (*p != static_cast<uint8_t>(HandshakeType::SERVER_HELLO)) {
    lastError_ = "Expected ServerHello";
    return false;
  }
  p++;

  // Length
  if (p + 3 > end) return false;
  size_t len = (static_cast<size_t>(p[0]) << 16) | (static_cast<size_t>(p[1]) << 8) | p[2];
  p += 3;

  if (p + len > end) return false;

  // Version
  if (p + 2 > end) return false;
  uint16_t legacyVersion = (static_cast<uint16_t>(p[0]) << 8) | p[1];
  p += 2;
  (void)legacyVersion;

  // Random
  if (p + 32 > end) return false;
  std::memcpy(serverRandom_.data(), p, 32);
  p += 32;

  // Check for HelloRetryRequest (special random value)
  static const uint8_t hrrRandom[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
    0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
    0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C
  };
  if (std::memcmp(serverRandom_.data(), hrrRandom, 32) == 0) {
    lastError_ = "HelloRetryRequest not supported";
    return false;
  }

  // Session ID
  if (p >= end) return false;
  uint8_t sessionIdLen = *p++;
  if (p + sessionIdLen > end) return false;
  p += sessionIdLen;

  // Cipher suite
  if (p + 2 > end) return false;
  cipherSuite_ = static_cast<CipherSuite>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
  p += 2;

  // Compression method
  if (p >= end) return false;
  p++;

  // Extensions
  version_ = TLSVersion::TLS_1_2;  // Default
  std::array<uint8_t, 32> serverPublicKey = {0};

  if (p + 2 <= end) {
    size_t extLen = (static_cast<size_t>(p[0]) << 8) | p[1];
    p += 2;
    const uint8_t* extEnd = p + extLen;

    while (p + 4 <= extEnd) {
      uint16_t extType = (static_cast<uint16_t>(p[0]) << 8) | p[1];
      size_t extDataLen = (static_cast<size_t>(p[2]) << 8) | p[3];
      p += 4;

      if (p + extDataLen > extEnd) break;

      if (extType == 0x002b) {  // Supported versions
        if (extDataLen >= 2) {
          uint16_t ver = (static_cast<uint16_t>(p[0]) << 8) | p[1];
          if (ver == 0x0304) {
            version_ = TLSVersion::TLS_1_3;
          }
        }
      } else if (extType == 0x0033) {  // Key share
        if (extDataLen >= 36) {
          // group (2) + key_len (2) + key (32)
          std::memcpy(serverPublicKey.data(), p + 4, 32);
        }
      }

      p += extDataLen;
    }
  }

  // Compute shared secret
  if (!X25519::computeSharedSecret(privateKey_.data(), serverPublicKey.data(), sharedSecret_.data())) {
    lastError_ = "Key exchange failed";
    return false;
  }

  updateTranscript(record.fragment.data(), record.fragment.size());

  if (version_ == TLSVersion::TLS_1_3) {
    deriveKeysTLS13();
    isEncrypted_ = true;
  }

  return true;
}

bool TLSConnection::receiveCertificate() {
  TLSRecord record;

  if (version_ == TLSVersion::TLS_1_3) {
    if (!receiveEncryptedRecord(record)) {
      lastError_ = "Failed to receive Certificate";
      return false;
    }
  } else {
    if (!receiveRecord(record)) {
      lastError_ = "Failed to receive Certificate";
      return false;
    }
  }

  if (record.type != ContentType::HANDSHAKE) {
    lastError_ = "Expected Handshake record";
    return false;
  }

  const uint8_t* p = record.fragment.data();
  const uint8_t* end = p + record.fragment.size();

  // TLS 1.3: might receive EncryptedExtensions first
  if (version_ == TLSVersion::TLS_1_3 &&
      *p == static_cast<uint8_t>(HandshakeType::ENCRYPTED_EXTENSIONS)) {
    // Skip EncryptedExtensions
    p++;
    if (p + 3 > end) return false;
    size_t len = (static_cast<size_t>(p[0]) << 16) | (static_cast<size_t>(p[1]) << 8) | p[2];
    p += 3 + len;

    updateTranscript(record.fragment.data(), p - record.fragment.data());

    // Get next record for Certificate
    if (!receiveEncryptedRecord(record)) {
      lastError_ = "Failed to receive Certificate after EncryptedExtensions";
      return false;
    }
    p = record.fragment.data();
    end = p + record.fragment.size();
  }

  if (*p != static_cast<uint8_t>(HandshakeType::CERTIFICATE)) {
    lastError_ = "Expected Certificate";
    return false;
  }
  p++;

  if (p + 3 > end) return false;
  size_t certMsgLen = (static_cast<size_t>(p[0]) << 16) | (static_cast<size_t>(p[1]) << 8) | p[2];
  p += 3;
  (void)certMsgLen;

  // TLS 1.3: certificate request context
  if (version_ == TLSVersion::TLS_1_3) {
    if (p >= end) return false;
    uint8_t contextLen = *p++;
    p += contextLen;
  }

  // Certificate list length
  if (p + 3 > end) return false;
  size_t certListLen = (static_cast<size_t>(p[0]) << 16) | (static_cast<size_t>(p[1]) << 8) | p[2];
  p += 3;

  const uint8_t* certListEnd = p + certListLen;
  if (certListEnd > end) return false;

  // Parse certificates
  while (p + 3 <= certListEnd) {
    size_t certLen = (static_cast<size_t>(p[0]) << 16) | (static_cast<size_t>(p[1]) << 8) | p[2];
    p += 3;

    if (p + certLen > certListEnd) break;

    X509Certificate::Certificate cert;
    if (X509Certificate::parse(p, certLen, cert)) {
      certificates_.push_back(cert);
    }
    p += certLen;

    // TLS 1.3: skip extensions after each certificate
    if (version_ == TLSVersion::TLS_1_3 && p + 2 <= certListEnd) {
      size_t extLen = (static_cast<size_t>(p[0]) << 8) | p[1];
      p += 2 + extLen;
    }
  }

  if (certificates_.empty()) {
    lastError_ = "No valid certificates";
    return false;
  }

  // Verify hostname
  if (!hostname_.empty() && !X509Certificate::verifyHostname(certificates_[0], hostname_)) {
    lastError_ = "Certificate hostname mismatch";
    return false;
  }

  // Verify validity period
  if (!X509Certificate::verifyValidity(certificates_[0])) {
    lastError_ = "Certificate expired or not yet valid";
    return false;
  }

  updateTranscript(record.fragment.data(), record.fragment.size());
  return true;
}

bool TLSConnection::handshake(const std::string& hostname) {
  hostname_ = hostname;

  if (!sendClientHello()) {
    return false;
  }

  if (!receiveServerHello()) {
    return false;
  }

  if (!receiveCertificate()) {
    return false;
  }

  if (version_ == TLSVersion::TLS_1_3) {
    // Receive CertificateVerify
    TLSRecord record;
    if (!receiveEncryptedRecord(record)) {
      lastError_ = "Failed to receive CertificateVerify";
      return false;
    }
    updateTranscript(record.fragment.data(), record.fragment.size());

    // Receive Finished
    if (!receiveEncryptedRecord(record)) {
      lastError_ = "Failed to receive Finished";
      return false;
    }
    updateTranscript(record.fragment.data(), record.fragment.size());

    // Derive application keys
    auto transcriptHash = getTranscriptHash();
    clientAppSecret_ = HKDF::deriveSecret(masterSecret_.data(), masterSecret_.size(),
                                           "c ap traffic",
                                           transcriptHash.data(), transcriptHash.size());
    serverAppSecret_ = HKDF::deriveSecret(masterSecret_.data(), masterSecret_.size(),
                                           "s ap traffic",
                                           transcriptHash.data(), transcriptHash.size());

    // Send client Finished
    auto finishedKey = HKDF::expandLabel(clientHandshakeSecret_.data(), clientHandshakeSecret_.size(),
                                          "finished", nullptr, 0, 32);
    auto verifyData = crypto::HMAC::compute(finishedKey.data(), finishedKey.size(),
                                             transcriptHash.data(), transcriptHash.size());

    std::vector<uint8_t> finished;
    finished.push_back(static_cast<uint8_t>(HandshakeType::FINISHED));
    finished.push_back(0); finished.push_back(0); finished.push_back(32);
    finished.insert(finished.end(), verifyData.begin(), verifyData.end());

    if (!sendEncryptedRecord(ContentType::HANDSHAKE, finished)) {
      lastError_ = "Failed to send Finished";
      return false;
    }

    // Switch to application keys
    deriveTrafficKeys(clientAppSecret_, clientKey_, clientIV_);
    deriveTrafficKeys(serverAppSecret_, serverKey_, serverIV_);
    clientSeqNum_ = 0;
    serverSeqNum_ = 0;

  } else {
    // TLS 1.2 flow
    // Receive ServerKeyExchange
    TLSRecord record;
    if (!receiveRecord(record)) {
      lastError_ = "Failed to receive ServerKeyExchange";
      return false;
    }
    updateTranscript(record.fragment.data(), record.fragment.size());

    // Receive ServerHelloDone
    if (!receiveRecord(record)) {
      lastError_ = "Failed to receive ServerHelloDone";
      return false;
    }
    updateTranscript(record.fragment.data(), record.fragment.size());

    deriveKeysTLS12();

    // Send ClientKeyExchange
    std::vector<uint8_t> cke;
    cke.push_back(static_cast<uint8_t>(HandshakeType::CLIENT_KEY_EXCHANGE));
    cke.push_back(0); cke.push_back(0); cke.push_back(33);
    cke.push_back(32);
    cke.insert(cke.end(), publicKey_.begin(), publicKey_.end());

    if (!sendRecord(ContentType::HANDSHAKE, cke)) {
      lastError_ = "Failed to send ClientKeyExchange";
      return false;
    }
    updateTranscript(cke.data(), cke.size());

    // Send ChangeCipherSpec
    std::vector<uint8_t> ccs = {1};
    if (!sendRecord(ContentType::CHANGE_CIPHER_SPEC, ccs)) {
      lastError_ = "Failed to send ChangeCipherSpec";
      return false;
    }
    isEncrypted_ = true;

    // Send Finished
    auto transcriptHash = getTranscriptHash();
    auto verifyData = PRF::compute(masterSecret_.data(), masterSecret_.size(),
                                    "client finished",
                                    transcriptHash.data(), transcriptHash.size(), 12);

    std::vector<uint8_t> finished;
    finished.push_back(static_cast<uint8_t>(HandshakeType::FINISHED));
    finished.push_back(0); finished.push_back(0); finished.push_back(12);
    finished.insert(finished.end(), verifyData.begin(), verifyData.end());

    if (!sendEncryptedRecord(ContentType::HANDSHAKE, finished)) {
      lastError_ = "Failed to send Finished";
      return false;
    }

    // Receive ChangeCipherSpec
    if (!receiveRecord(record)) {
      lastError_ = "Failed to receive ChangeCipherSpec";
      return false;
    }

    // Receive Finished
    if (!receiveEncryptedRecord(record)) {
      lastError_ = "Failed to receive server Finished";
      return false;
    }
  }

  handshakeComplete_ = true;
  return true;
}

bool TLSConnection::send(const uint8_t* data, size_t len) {
  if (!handshakeComplete_) {
    lastError_ = "Handshake not complete";
    return false;
  }

  // Fragment into max record size
  const size_t maxFragment = 16384;
  size_t offset = 0;

  while (offset < len) {
    size_t fragLen = std::min(maxFragment, len - offset);
    std::vector<uint8_t> fragment(data + offset, data + offset + fragLen);

    if (!sendEncryptedRecord(ContentType::APPLICATION_DATA, fragment)) {
      return false;
    }
    offset += fragLen;
  }

  return true;
}

int TLSConnection::recv(uint8_t* buffer, size_t maxLen) {
  if (!handshakeComplete_) {
    lastError_ = "Handshake not complete";
    return -1;
  }

  TLSRecord record;
  if (!receiveEncryptedRecord(record)) {
    return -1;
  }

  if (record.type == ContentType::ALERT) {
    if (record.fragment.size() >= 2 && record.fragment[1] == 0) {
      // close_notify
      return 0;
    }
    lastError_ = "TLS alert received";
    return -1;
  }

  if (record.type != ContentType::APPLICATION_DATA) {
    lastError_ = "Unexpected record type";
    return -1;
  }

  size_t copyLen = std::min(maxLen, record.fragment.size());
  std::memcpy(buffer, record.fragment.data(), copyLen);
  return static_cast<int>(copyLen);
}

void TLSConnection::close() {
  if (handshakeComplete_ && isEncrypted_) {
    // Send close_notify alert
    std::vector<uint8_t> alert = {
      static_cast<uint8_t>(AlertLevel::WARNING),
      static_cast<uint8_t>(AlertDescription::CLOSE_NOTIFY)
    };
    sendEncryptedRecord(ContentType::ALERT, alert);
  }
}

}  // namespace tls
}  // namespace lightjs
