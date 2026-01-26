#include "tls.h"
#include "crypto.h"
#include <ctime>
#include <algorithm>

namespace lightjs {
namespace tls {

// ASN.1 tag types
constexpr uint8_t ASN1_SEQUENCE = 0x30;
constexpr uint8_t ASN1_SET = 0x31;
constexpr uint8_t ASN1_INTEGER = 0x02;
constexpr uint8_t ASN1_BIT_STRING = 0x03;
constexpr uint8_t ASN1_OCTET_STRING = 0x04;
constexpr uint8_t ASN1_NULL = 0x05;
constexpr uint8_t ASN1_OID = 0x06;
constexpr uint8_t ASN1_UTF8_STRING = 0x0C;
constexpr uint8_t ASN1_PRINTABLE_STRING = 0x13;
constexpr uint8_t ASN1_IA5_STRING = 0x16;
constexpr uint8_t ASN1_UTC_TIME = 0x17;
constexpr uint8_t ASN1_GENERALIZED_TIME = 0x18;
constexpr uint8_t ASN1_CONTEXT_0 = 0xA0;
constexpr uint8_t ASN1_CONTEXT_3 = 0xA3;

// Common OIDs
static const std::string OID_RSA_ENCRYPTION = "1.2.840.113549.1.1.1";
static const std::string OID_SHA256_WITH_RSA = "1.2.840.113549.1.1.11";
static const std::string OID_COMMON_NAME = "2.5.4.3";
static const std::string OID_SUBJECT_ALT_NAME = "2.5.29.17";

size_t X509Certificate::parseLength(const uint8_t*& p, const uint8_t* end) {
  if (p >= end) return 0;

  uint8_t first = *p++;
  if (first < 0x80) {
    return first;
  }

  int numBytes = first & 0x7f;
  if (numBytes > 4 || p + numBytes > end) {
    return 0;
  }

  size_t len = 0;
  for (int i = 0; i < numBytes; i++) {
    len = (len << 8) | *p++;
  }
  return len;
}

bool X509Certificate::parseSequence(const uint8_t*& p, const uint8_t* end, size_t& len) {
  if (p >= end || *p != ASN1_SEQUENCE) return false;
  p++;
  len = parseLength(p, end);
  return len > 0 && p + len <= end;
}

bool X509Certificate::parseInteger(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& value) {
  if (p >= end || *p != ASN1_INTEGER) return false;
  p++;
  size_t len = parseLength(p, end);
  if (len == 0 || p + len > end) return false;

  // Skip leading zeros
  while (len > 0 && *p == 0) {
    p++;
    len--;
  }

  value.assign(p, p + len);
  p += len;
  return true;
}

bool X509Certificate::parseOID(const uint8_t*& p, const uint8_t* end, std::string& oid) {
  if (p >= end || *p != ASN1_OID) return false;
  p++;
  size_t len = parseLength(p, end);
  if (len == 0 || p + len > end) return false;

  const uint8_t* oidEnd = p + len;
  oid.clear();

  // First byte encodes first two components
  if (p < oidEnd) {
    oid = std::to_string(*p / 40) + "." + std::to_string(*p % 40);
    p++;
  }

  // Remaining bytes encode subsequent components
  while (p < oidEnd) {
    uint32_t component = 0;
    while (p < oidEnd) {
      uint8_t b = *p++;
      component = (component << 7) | (b & 0x7f);
      if ((b & 0x80) == 0) break;
    }
    oid += "." + std::to_string(component);
  }

  return true;
}

bool X509Certificate::parseString(const uint8_t*& p, const uint8_t* end, std::string& str) {
  if (p >= end) return false;

  uint8_t tag = *p++;
  if (tag != ASN1_UTF8_STRING && tag != ASN1_PRINTABLE_STRING &&
      tag != ASN1_IA5_STRING && tag != ASN1_OCTET_STRING) {
    return false;
  }

  size_t len = parseLength(p, end);
  if (len == 0 || p + len > end) return false;

  str.assign(reinterpret_cast<const char*>(p), len);
  p += len;
  return true;
}

bool X509Certificate::parseBitString(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& bits) {
  if (p >= end || *p != ASN1_BIT_STRING) return false;
  p++;
  size_t len = parseLength(p, end);
  if (len < 2 || p + len > end) return false;

  // First byte is number of unused bits in last byte
  uint8_t unusedBits = *p++;
  len--;

  bits.assign(p, p + len);
  p += len;

  // Remove unused bits from last byte
  if (unusedBits > 0 && !bits.empty()) {
    bits.back() &= (0xff << unusedBits);
  }

  return true;
}

bool X509Certificate::parseTime(const uint8_t*& p, const uint8_t* end, uint64_t& timestamp) {
  if (p >= end) return false;

  uint8_t tag = *p++;
  if (tag != ASN1_UTC_TIME && tag != ASN1_GENERALIZED_TIME) return false;

  size_t len = parseLength(p, end);
  if (len == 0 || p + len > end) return false;

  std::string timeStr(reinterpret_cast<const char*>(p), len);
  p += len;

  struct tm t = {};
  int year, month, day, hour, min, sec;

  if (tag == ASN1_UTC_TIME) {
    // YYMMDDhhmmssZ
    if (sscanf(timeStr.c_str(), "%2d%2d%2d%2d%2d%2d", &year, &month, &day, &hour, &min, &sec) != 6) {
      return false;
    }
    year += (year >= 50) ? 1900 : 2000;
  } else {
    // YYYYMMDDhhmmssZ
    if (sscanf(timeStr.c_str(), "%4d%2d%2d%2d%2d%2d", &year, &month, &day, &hour, &min, &sec) != 6) {
      return false;
    }
  }

  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = min;
  t.tm_sec = sec;

  timestamp = static_cast<uint64_t>(mktime(&t));
  return true;
}

bool X509Certificate::parse(const uint8_t* data, size_t len, Certificate& cert) {
  cert.raw.assign(data, data + len);
  const uint8_t* p = data;
  const uint8_t* end = data + len;

  // Certificate ::= SEQUENCE
  size_t certLen;
  if (!parseSequence(p, end, certLen)) return false;

  const uint8_t* certEnd = p + certLen;

  // TBSCertificate ::= SEQUENCE
  const uint8_t* tbsStart = p;
  size_t tbsLen;
  if (!parseSequence(p, certEnd, tbsLen)) return false;
  const uint8_t* tbsEnd = p + tbsLen;

  // Version (optional, context-specific [0])
  if (p < tbsEnd && *p == ASN1_CONTEXT_0) {
    p++;
    size_t vLen = parseLength(p, tbsEnd);
    p += vLen;  // Skip version
  }

  // Serial number
  std::vector<uint8_t> serialNumber;
  if (!parseInteger(p, tbsEnd, serialNumber)) return false;

  // Signature algorithm
  size_t sigAlgLen;
  if (!parseSequence(p, tbsEnd, sigAlgLen)) return false;
  const uint8_t* sigAlgEnd = p + sigAlgLen;
  std::string sigAlgOID;
  if (!parseOID(p, sigAlgEnd, sigAlgOID)) return false;
  p = sigAlgEnd;

  // Issuer
  size_t issuerLen;
  if (!parseSequence(p, tbsEnd, issuerLen)) return false;
  const uint8_t* issuerEnd = p + issuerLen;

  // Parse issuer RDNs
  while (p < issuerEnd) {
    size_t setLen;
    if (*p == ASN1_SET) {
      p++;
      setLen = parseLength(p, issuerEnd);
      const uint8_t* setEnd = p + setLen;

      while (p < setEnd) {
        size_t seqLen;
        if (!parseSequence(p, setEnd, seqLen)) break;
        const uint8_t* attrEnd = p + seqLen;

        std::string oid;
        if (!parseOID(p, attrEnd, oid)) break;

        std::string value;
        if (!parseString(p, attrEnd, value)) {
          p = attrEnd;
          continue;
        }

        if (oid == OID_COMMON_NAME) {
          cert.issuer = value;
        }
        p = attrEnd;
      }
      p = setEnd;
    } else {
      break;
    }
  }
  p = issuerEnd;

  // Validity
  size_t validityLen;
  if (!parseSequence(p, tbsEnd, validityLen)) return false;
  const uint8_t* validityEnd = p + validityLen;
  if (!parseTime(p, validityEnd, cert.notBefore)) return false;
  if (!parseTime(p, validityEnd, cert.notAfter)) return false;
  p = validityEnd;

  // Subject
  size_t subjectLen;
  if (!parseSequence(p, tbsEnd, subjectLen)) return false;
  const uint8_t* subjectEnd = p + subjectLen;

  while (p < subjectEnd) {
    if (*p == ASN1_SET) {
      p++;
      size_t setLen = parseLength(p, subjectEnd);
      const uint8_t* setEnd = p + setLen;

      while (p < setEnd) {
        size_t seqLen;
        if (!parseSequence(p, setEnd, seqLen)) break;
        const uint8_t* attrEnd = p + seqLen;

        std::string oid;
        if (!parseOID(p, attrEnd, oid)) break;

        std::string value;
        if (!parseString(p, attrEnd, value)) {
          p = attrEnd;
          continue;
        }

        if (oid == OID_COMMON_NAME) {
          cert.commonName = value;
          cert.subject = value;
        }
        p = attrEnd;
      }
      p = setEnd;
    } else {
      break;
    }
  }
  p = subjectEnd;

  // SubjectPublicKeyInfo
  size_t spkiLen;
  if (!parseSequence(p, tbsEnd, spkiLen)) return false;
  const uint8_t* spkiEnd = p + spkiLen;

  // Algorithm
  size_t algLen;
  if (!parseSequence(p, spkiEnd, algLen)) return false;
  const uint8_t* algEnd = p + algLen;
  std::string pubKeyAlg;
  if (!parseOID(p, algEnd, pubKeyAlg)) return false;
  p = algEnd;

  // SubjectPublicKey (BIT STRING containing RSA public key)
  std::vector<uint8_t> pubKeyBits;
  if (!parseBitString(p, spkiEnd, pubKeyBits)) return false;

  // Parse RSA public key from the bit string
  if (pubKeyAlg == OID_RSA_ENCRYPTION && !pubKeyBits.empty()) {
    const uint8_t* keyP = pubKeyBits.data();
    const uint8_t* keyEnd = keyP + pubKeyBits.size();

    size_t rsaSeqLen;
    if (parseSequence(keyP, keyEnd, rsaSeqLen)) {
      // Modulus n
      parseInteger(keyP, keyEnd, cert.publicKey.n);
      // Exponent e
      parseInteger(keyP, keyEnd, cert.publicKey.e);
    }
  }

  p = spkiEnd;

  // Extensions (optional, context-specific [3])
  if (p < tbsEnd && *p == ASN1_CONTEXT_3) {
    p++;
    size_t extOuterLen = parseLength(p, tbsEnd);
    const uint8_t* extOuterEnd = p + extOuterLen;

    size_t extLen;
    if (parseSequence(p, extOuterEnd, extLen)) {
      const uint8_t* extEnd = p + extLen;

      while (p < extEnd) {
        size_t extSeqLen;
        if (!parseSequence(p, extEnd, extSeqLen)) break;
        const uint8_t* extSeqEnd = p + extSeqLen;

        std::string extOID;
        if (!parseOID(p, extSeqEnd, extOID)) {
          p = extSeqEnd;
          continue;
        }

        // Skip critical flag if present
        if (p < extSeqEnd && *p == 0x01) {
          p++;
          size_t boolLen = parseLength(p, extSeqEnd);
          p += boolLen;
        }

        // Extension value (OCTET STRING)
        if (p < extSeqEnd && *p == ASN1_OCTET_STRING) {
          p++;
          size_t octetLen = parseLength(p, extSeqEnd);
          const uint8_t* octetEnd = p + octetLen;

          // Parse Subject Alternative Name
          if (extOID == OID_SUBJECT_ALT_NAME) {
            size_t sanSeqLen;
            if (parseSequence(p, octetEnd, sanSeqLen)) {
              const uint8_t* sanEnd = p + sanSeqLen;
              while (p < sanEnd) {
                uint8_t tag = *p++;
                size_t sanLen = parseLength(p, sanEnd);
                // DNS name has tag [2]
                if ((tag & 0x1f) == 2) {
                  std::string dnsName(reinterpret_cast<const char*>(p), sanLen);
                  cert.subjectAltNames.push_back(dnsName);
                }
                p += sanLen;
              }
            }
          }
          p = octetEnd;
        }

        p = extSeqEnd;
      }
    }
  }

  p = tbsEnd;

  // Signature algorithm (again)
  size_t sigAlg2Len;
  if (!parseSequence(p, certEnd, sigAlg2Len)) return false;
  p += sigAlg2Len;

  // Signature value
  if (!parseBitString(p, certEnd, cert.signature)) return false;

  return true;
}

bool X509Certificate::verifyHostname(const Certificate& cert, const std::string& hostname) {
  // Check Subject Alternative Names first
  for (const auto& san : cert.subjectAltNames) {
    if (san == hostname) return true;

    // Wildcard matching
    if (san.size() > 2 && san[0] == '*' && san[1] == '.') {
      std::string suffix = san.substr(1);  // .example.com
      size_t dotPos = hostname.find('.');
      if (dotPos != std::string::npos) {
        std::string hostSuffix = hostname.substr(dotPos);
        if (hostSuffix == suffix) return true;
      }
    }
  }

  // Fall back to Common Name
  if (cert.commonName == hostname) return true;

  // Wildcard in CN
  if (cert.commonName.size() > 2 && cert.commonName[0] == '*' && cert.commonName[1] == '.') {
    std::string suffix = cert.commonName.substr(1);
    size_t dotPos = hostname.find('.');
    if (dotPos != std::string::npos) {
      std::string hostSuffix = hostname.substr(dotPos);
      if (hostSuffix == suffix) return true;
    }
  }

  return false;
}

bool X509Certificate::verifyValidity(const Certificate& cert) {
  time_t now = time(nullptr);
  return now >= static_cast<time_t>(cert.notBefore) && now <= static_cast<time_t>(cert.notAfter);
}

}  // namespace tls
}  // namespace lightjs
