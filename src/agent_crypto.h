// Kloudputer — the three primitives the agent protocol needs: HMAC-SHA256, AES-256-GCM and random bytes.
// mbedTLS on the Cardputer, OpenSSL in the host tests, so the same protocol code runs in both.
#pragma once
#include <cstddef>
#include <cstdint>
#ifdef ARDUINO
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace agentcrypto {
#ifdef ARDUINO
inline bool hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t out[32]) {
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, data, len, out) == 0;
}

inline bool gcmSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* in,
                    size_t len, uint8_t* out, uint8_t tag[16]) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                  mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, 12, aad, aadLen, in, out, 16, tag) == 0;
  mbedtls_gcm_free(&gcm);
  return ok;
}

inline bool gcmOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* in,
                    size_t len, const uint8_t tag[16], uint8_t* out) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                  mbedtls_gcm_auth_decrypt(&gcm, len, nonce, 12, aad, aadLen, tag, 16, in, out) == 0;
  mbedtls_gcm_free(&gcm);
  return ok;
}

inline void randomBytes(uint8_t* out, size_t n) { esp_fill_random(out, n); }
#else
inline bool hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t out[32]) {
  size_t outLen = 0;
  return EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key, keyLen, data, len, out, 32, &outLen) && outLen == 32;
}

inline bool gcmSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* in,
                    size_t len, uint8_t* out, uint8_t tag[16]) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int n = 0;
  const bool ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
                  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) &&
                  EVP_EncryptInit_ex(c, nullptr, nullptr, key, nonce) &&
                  EVP_EncryptUpdate(c, nullptr, &n, aad, aadLen) && EVP_EncryptUpdate(c, out, &n, in, len) &&
                  EVP_EncryptFinal_ex(c, out + n, &n) && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag);
  EVP_CIPHER_CTX_free(c);
  return ok;
}

inline bool gcmOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* in,
                    size_t len, const uint8_t tag[16], uint8_t* out) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int n = 0;
  const bool ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
                  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) &&
                  EVP_DecryptInit_ex(c, nullptr, nullptr, key, nonce) &&
                  EVP_DecryptUpdate(c, nullptr, &n, aad, aadLen) && EVP_DecryptUpdate(c, out, &n, in, len) &&
                  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) &&
                  EVP_DecryptFinal_ex(c, out + n, &n) > 0;
  EVP_CIPHER_CTX_free(c);
  return ok;
}

inline void randomBytes(uint8_t* out, size_t n) { RAND_bytes(out, n); }
#endif
}  // namespace agentcrypto
