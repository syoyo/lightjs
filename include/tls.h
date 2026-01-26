#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include <vector>
#include <string>
#include <memory>
#include <functional>

namespace lightjs {
namespace tls {

// Forward declarations
class TLSConnection;

// TLS versions
enum class TLSVersion : uint16_t {
  TLS_1_2 = 0x0303,
  TLS_1_3 = 0x0304
};

// TLS content types
enum class ContentType : uint8_t {
  CHANGE_CIPHER_SPEC = 20,
  ALERT = 21,
  HANDSHAKE = 22,
  APPLICATION_DATA = 23
};

// TLS handshake types
enum class HandshakeType : uint8_t {
  CLIENT_HELLO = 1,
  SERVER_HELLO = 2,
  NEW_SESSION_TICKET = 4,
  END_OF_EARLY_DATA = 5,
  ENCRYPTED_EXTENSIONS = 8,
  CERTIFICATE = 11,
  SERVER_KEY_EXCHANGE = 12,
  CERTIFICATE_REQUEST = 13,
  SERVER_HELLO_DONE = 14,
  CERTIFICATE_VERIFY = 15,
  CLIENT_KEY_EXCHANGE = 16,
  FINISHED = 20,
  KEY_UPDATE = 24,
  MESSAGE_HASH = 254
};

// TLS cipher suites
enum class CipherSuite : uint16_t {
  // TLS 1.3
  TLS_AES_128_GCM_SHA256 = 0x1301,
  TLS_AES_256_GCM_SHA384 = 0x1302,
  TLS_CHACHA20_POLY1305_SHA256 = 0x1303,

  // TLS 1.2
  TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 = 0xC02F,
  TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 = 0xC030,
  TLS_RSA_WITH_AES_128_GCM_SHA256 = 0x009C,
  TLS_RSA_WITH_AES_256_GCM_SHA384 = 0x009D
};

// Named groups for key exchange
enum class NamedGroup : uint16_t {
  SECP256R1 = 0x0017,
  SECP384R1 = 0x0018,
  SECP521R1 = 0x0019,
  X25519 = 0x001D,
  X448 = 0x001E
};

// Signature algorithms
enum class SignatureScheme : uint16_t {
  RSA_PKCS1_SHA256 = 0x0401,
  RSA_PKCS1_SHA384 = 0x0501,
  RSA_PKCS1_SHA512 = 0x0601,
  ECDSA_SECP256R1_SHA256 = 0x0403,
  ECDSA_SECP384R1_SHA384 = 0x0503,
  RSA_PSS_RSAE_SHA256 = 0x0804,
  RSA_PSS_RSAE_SHA384 = 0x0805,
  RSA_PSS_RSAE_SHA512 = 0x0806
};

// Alert levels and descriptions
enum class AlertLevel : uint8_t {
  WARNING = 1,
  FATAL = 2
};

enum class AlertDescription : uint8_t {
  CLOSE_NOTIFY = 0,
  UNEXPECTED_MESSAGE = 10,
  BAD_RECORD_MAC = 20,
  RECORD_OVERFLOW = 22,
  HANDSHAKE_FAILURE = 40,
  BAD_CERTIFICATE = 42,
  UNSUPPORTED_CERTIFICATE = 43,
  CERTIFICATE_REVOKED = 44,
  CERTIFICATE_EXPIRED = 45,
  CERTIFICATE_UNKNOWN = 46,
  ILLEGAL_PARAMETER = 47,
  UNKNOWN_CA = 48,
  ACCESS_DENIED = 49,
  DECODE_ERROR = 50,
  DECRYPT_ERROR = 51,
  PROTOCOL_VERSION = 70,
  INSUFFICIENT_SECURITY = 71,
  INTERNAL_ERROR = 80,
  INAPPROPRIATE_FALLBACK = 86,
  USER_CANCELED = 90,
  MISSING_EXTENSION = 109,
  UNSUPPORTED_EXTENSION = 110,
  UNRECOGNIZED_NAME = 112,
  BAD_CERTIFICATE_STATUS_RESPONSE = 113,
  UNKNOWN_PSK_IDENTITY = 115,
  CERTIFICATE_REQUIRED = 116,
  NO_APPLICATION_PROTOCOL = 120
};

// AES-128 implementation
class AES128 {
public:
  static constexpr size_t BLOCK_SIZE = 16;
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t NUM_ROUNDS = 10;

  AES128();
  explicit AES128(const uint8_t* key);

  void setKey(const uint8_t* key);
  void encryptBlock(const uint8_t* in, uint8_t* out) const;
  void decryptBlock(const uint8_t* in, uint8_t* out) const;

  // Shared lookup tables (also used by AES256)
  static const uint8_t SBOX[256];
  static const uint8_t INV_SBOX[256];
  static const uint8_t RCON[11];

private:
  void keyExpansion(const uint8_t* key);

  uint32_t roundKeys_[44];  // 4 * (NUM_ROUNDS + 1)
};

// AES-256 implementation
class AES256 {
public:
  static constexpr size_t BLOCK_SIZE = 16;
  static constexpr size_t KEY_SIZE = 32;
  static constexpr size_t NUM_ROUNDS = 14;

  AES256();
  explicit AES256(const uint8_t* key);

  void setKey(const uint8_t* key);
  void encryptBlock(const uint8_t* in, uint8_t* out) const;
  void decryptBlock(const uint8_t* in, uint8_t* out) const;

private:
  void keyExpansion(const uint8_t* key);

  uint32_t roundKeys_[60];  // 4 * (NUM_ROUNDS + 1)
};

// GCM mode for authenticated encryption
class GCM {
public:
  static constexpr size_t TAG_SIZE = 16;
  static constexpr size_t NONCE_SIZE = 12;

  // Encrypt with AES-128-GCM
  static bool encrypt128(
    const uint8_t* key,
    const uint8_t* nonce,
    const uint8_t* plaintext, size_t plaintextLen,
    const uint8_t* aad, size_t aadLen,
    uint8_t* ciphertext,
    uint8_t* tag
  );

  // Decrypt with AES-128-GCM
  static bool decrypt128(
    const uint8_t* key,
    const uint8_t* nonce,
    const uint8_t* ciphertext, size_t ciphertextLen,
    const uint8_t* aad, size_t aadLen,
    const uint8_t* tag,
    uint8_t* plaintext
  );

  // Encrypt with AES-256-GCM
  static bool encrypt256(
    const uint8_t* key,
    const uint8_t* nonce,
    const uint8_t* plaintext, size_t plaintextLen,
    const uint8_t* aad, size_t aadLen,
    uint8_t* ciphertext,
    uint8_t* tag
  );

  // Decrypt with AES-256-GCM
  static bool decrypt256(
    const uint8_t* key,
    const uint8_t* nonce,
    const uint8_t* ciphertext, size_t ciphertextLen,
    const uint8_t* aad, size_t aadLen,
    const uint8_t* tag,
    uint8_t* plaintext
  );

private:
  static void ghash(const uint8_t* H, const uint8_t* data, size_t len, uint8_t* out);
  static void gfMult(const uint8_t* X, const uint8_t* Y, uint8_t* Z);
  static void incr(uint8_t* counter);
};

// X25519 Elliptic Curve Diffie-Hellman
class X25519 {
public:
  static constexpr size_t KEY_SIZE = 32;

  // Generate a random private key
  static void generatePrivateKey(uint8_t* privateKey);

  // Derive public key from private key
  static void derivePublicKey(const uint8_t* privateKey, uint8_t* publicKey);

  // Compute shared secret
  static bool computeSharedSecret(
    const uint8_t* privateKey,
    const uint8_t* peerPublicKey,
    uint8_t* sharedSecret
  );

private:
  // Field arithmetic in GF(2^255 - 19)
  using fe = int64_t[16];  // Field element

  static void feZero(fe out);
  static void feOne(fe out);
  static void feCopy(fe out, const fe in);
  static void feAdd(fe out, const fe a, const fe b);
  static void feSub(fe out, const fe a, const fe b);
  static void feMul(fe out, const fe a, const fe b);
  static void feSq(fe out, const fe a);
  static void feInvert(fe out, const fe z);
  static void fePow2523(fe out, const fe z);
  static void feFromBytes(fe out, const uint8_t* s);
  static void feToBytes(uint8_t* s, const fe h);
  static void feReduce(fe h);
  static void feCswap(fe f, fe g, int b);

  static void scalarmult(uint8_t* q, const uint8_t* n, const uint8_t* p);
};

// HKDF (HMAC-based Key Derivation Function) for TLS 1.3
class HKDF {
public:
  // HKDF-Extract
  static std::vector<uint8_t> extract(
    const uint8_t* salt, size_t saltLen,
    const uint8_t* ikm, size_t ikmLen
  );

  // HKDF-Expand
  static std::vector<uint8_t> expand(
    const uint8_t* prk, size_t prkLen,
    const uint8_t* info, size_t infoLen,
    size_t length
  );

  // HKDF-Expand-Label for TLS 1.3
  static std::vector<uint8_t> expandLabel(
    const uint8_t* secret, size_t secretLen,
    const std::string& label,
    const uint8_t* context, size_t contextLen,
    size_t length
  );

  // Derive-Secret for TLS 1.3
  static std::vector<uint8_t> deriveSecret(
    const uint8_t* secret, size_t secretLen,
    const std::string& label,
    const uint8_t* messages, size_t messagesLen
  );
};

// PRF for TLS 1.2
class PRF {
public:
  static std::vector<uint8_t> compute(
    const uint8_t* secret, size_t secretLen,
    const std::string& label,
    const uint8_t* seed, size_t seedLen,
    size_t length
  );
};

// Simple RSA implementation for certificate verification
class RSA {
public:
  struct PublicKey {
    std::vector<uint8_t> n;  // Modulus
    std::vector<uint8_t> e;  // Exponent
  };

  // Verify PKCS#1 v1.5 signature
  static bool verifyPKCS1(
    const PublicKey& key,
    const uint8_t* message, size_t messageLen,
    const uint8_t* signature, size_t signatureLen
  );

  // Encrypt with public key (for key exchange)
  static std::vector<uint8_t> encrypt(
    const PublicKey& key,
    const uint8_t* data, size_t dataLen
  );

private:
  // Big integer operations
  static std::vector<uint8_t> modPow(
    const std::vector<uint8_t>& base,
    const std::vector<uint8_t>& exp,
    const std::vector<uint8_t>& mod
  );
};

// X.509 Certificate parsing
class X509Certificate {
public:
  struct Certificate {
    std::vector<uint8_t> raw;
    std::string subject;
    std::string issuer;
    std::string commonName;
    std::vector<std::string> subjectAltNames;
    uint64_t notBefore;
    uint64_t notAfter;
    RSA::PublicKey publicKey;
    std::vector<uint8_t> signature;
    uint16_t signatureAlgorithm;
  };

  static bool parse(const uint8_t* data, size_t len, Certificate& cert);
  static bool verifyHostname(const Certificate& cert, const std::string& hostname);
  static bool verifyValidity(const Certificate& cert);

private:
  // ASN.1 DER parsing helpers
  static bool parseSequence(const uint8_t*& p, const uint8_t* end, size_t& len);
  static bool parseInteger(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& value);
  static bool parseOID(const uint8_t*& p, const uint8_t* end, std::string& oid);
  static bool parseString(const uint8_t*& p, const uint8_t* end, std::string& str);
  static bool parseBitString(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& bits);
  static bool parseTime(const uint8_t*& p, const uint8_t* end, uint64_t& timestamp);
  static size_t parseLength(const uint8_t*& p, const uint8_t* end);
};

// TLS Record Layer
class TLSRecord {
public:
  ContentType type;
  TLSVersion version;
  std::vector<uint8_t> fragment;

  std::vector<uint8_t> serialize() const;
  static bool parse(const uint8_t* data, size_t len, TLSRecord& record, size_t& consumed);
};

// TLS Connection state
class TLSConnection {
public:
  using SendCallback = std::function<bool(const uint8_t*, size_t)>;
  using RecvCallback = std::function<int(uint8_t*, size_t)>;

  TLSConnection(SendCallback send, RecvCallback recv);
  ~TLSConnection();

  // Perform TLS handshake as client
  bool handshake(const std::string& hostname);

  // Send application data
  bool send(const uint8_t* data, size_t len);

  // Receive application data
  int recv(uint8_t* buffer, size_t maxLen);

  // Close connection
  void close();

  // Get last error
  const std::string& getLastError() const { return lastError_; }

  // Get negotiated version
  TLSVersion getVersion() const { return version_; }

private:
  // Handshake helpers
  bool sendClientHello();
  bool receiveServerHello();
  bool receiveCertificate();
  bool receiveServerKeyExchange();  // TLS 1.2
  bool receiveServerHelloDone();    // TLS 1.2
  bool sendClientKeyExchange();     // TLS 1.2
  bool sendChangeCipherSpec();      // TLS 1.2
  bool sendFinished();
  bool receiveFinished();

  // TLS 1.3 specific
  bool receiveEncryptedExtensions();
  bool receiveCertificateVerify();

  // Record layer
  bool sendRecord(ContentType type, const std::vector<uint8_t>& data);
  bool receiveRecord(TLSRecord& record);
  bool sendEncryptedRecord(ContentType type, const std::vector<uint8_t>& data);
  bool receiveEncryptedRecord(TLSRecord& record);

  // Key derivation
  void deriveKeysTLS12();
  void deriveKeysTLS13();
  void deriveTrafficKeys(const std::vector<uint8_t>& secret,
                         std::vector<uint8_t>& key,
                         std::vector<uint8_t>& iv);

  // Encryption/Decryption
  bool encrypt(const std::vector<uint8_t>& plaintext,
               std::vector<uint8_t>& ciphertext,
               ContentType type);
  bool decrypt(const std::vector<uint8_t>& ciphertext,
               std::vector<uint8_t>& plaintext,
               ContentType type);

  // Update transcript hash
  void updateTranscript(const uint8_t* data, size_t len);
  std::vector<uint8_t> getTranscriptHash();

  // Generate random bytes
  void generateRandom(uint8_t* buffer, size_t len);

  // Callbacks
  SendCallback sendCallback_;
  RecvCallback recvCallback_;

  // Connection state
  TLSVersion version_;
  CipherSuite cipherSuite_;
  std::string hostname_;
  std::string lastError_;
  bool handshakeComplete_;
  bool isEncrypted_;

  // Random values
  std::array<uint8_t, 32> clientRandom_;
  std::array<uint8_t, 32> serverRandom_;

  // Key exchange
  std::array<uint8_t, 32> privateKey_;
  std::array<uint8_t, 32> publicKey_;
  std::array<uint8_t, 32> sharedSecret_;

  // Master secret (TLS 1.2)
  std::vector<uint8_t> masterSecret_;

  // Traffic secrets (TLS 1.3)
  std::vector<uint8_t> clientHandshakeSecret_;
  std::vector<uint8_t> serverHandshakeSecret_;
  std::vector<uint8_t> clientAppSecret_;
  std::vector<uint8_t> serverAppSecret_;

  // Encryption keys and IVs
  std::vector<uint8_t> clientKey_;
  std::vector<uint8_t> serverKey_;
  std::vector<uint8_t> clientIV_;
  std::vector<uint8_t> serverIV_;

  // Sequence numbers for nonce
  uint64_t clientSeqNum_;
  uint64_t serverSeqNum_;

  // Transcript hash
  std::vector<uint8_t> transcriptData_;

  // Certificate chain
  std::vector<X509Certificate::Certificate> certificates_;

  // Buffer for partial records
  std::vector<uint8_t> recvBuffer_;
};

// Secure random number generation
void secureRandom(uint8_t* buffer, size_t len);

}  // namespace tls
}  // namespace lightjs
