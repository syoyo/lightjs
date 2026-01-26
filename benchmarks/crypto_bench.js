// Crypto operations benchmark

function benchSHA256() {
  let data = "Hello World! ".repeat(100);
  for (let i = 0; i < 100; i++) {
    crypto.sha256(data);
  }
}

function benchHMAC() {
  let key = "secret_key_123456789";
  let data = "Message to authenticate ".repeat(50);
  for (let i = 0; i < 100; i++) {
    crypto.hmac(key, data);
  }
}

function benchBase64() {
  let data = "0123456789abcdef".repeat(100);
  let encoded;
  for (let i = 0; i < 100; i++) {
    encoded = crypto.toHex(data);
  }
}

benchSHA256();
benchHMAC();
benchBase64();

console.log("Crypto benchmark completed");
