// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_crypto.h"

#include <cstring>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace BbRoom {
namespace {

std::array<std::uint8_t, 32> Hmac(const std::uint8_t* key, std::size_t key_len, const char* label) {
    std::array<std::uint8_t, 32> out{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), key, static_cast<int>(key_len), reinterpret_cast<const unsigned char*>(label),
         std::strlen(label), out.data(), &len);
    return out;
}

std::string Hex(const std::uint8_t* data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        text.push_back(digits[data[i] >> 4]);
        text.push_back(digits[data[i] & 15]);
    }
    return text;
}

} // namespace

Secret Derive(const std::string& name, const std::string& password, int iterations) {
    std::string input = "lantern-room";
    input.push_back('\0');
    input += name;
    input.push_back('\0');
    input += password;
    static const char salt[] = "lantern-seamless-coop";
    std::array<std::uint8_t, 32> master{};
    PKCS5_PBKDF2_HMAC(input.data(), static_cast<int>(input.size()),
                      reinterpret_cast<const unsigned char*>(salt), sizeof(salt) - 1, iterations, EVP_sha256(),
                      static_cast<int>(master.size()), master.data());
    Secret secret;
    const auto id = Hmac(master.data(), master.size(), "room id");
    secret.room_id = Hex(id.data(), id.size());
    secret.key = Hmac(master.data(), master.size(), "frame key");
    OPENSSL_cleanse(master.data(), master.size());
    return secret;
}

std::vector<std::uint8_t> Seal(const Key& key, std::uint8_t sender, const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> out(kSealOverhead + size);
    RAND_bytes(out.data(), static_cast<int>(kNonceLen));
    std::vector<std::uint8_t> plain(1 + size);
    plain[0] = sender;
    if (size) std::memcpy(plain.data() + 1, data, size);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), out.data()) == 1 &&
              EVP_EncryptUpdate(ctx, out.data() + kNonceLen, &len, plain.data(), static_cast<int>(plain.size())) == 1 &&
              EVP_EncryptFinal_ex(ctx, out.data() + kNonceLen + len, &len) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen),
                                  out.data() + kNonceLen + plain.size()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) out.clear();
    return out;
}

std::optional<std::vector<std::uint8_t>> Open(const Key& key, std::uint8_t sender, const std::uint8_t* data,
                                              std::size_t size) {
    if (size < kSealOverhead) return std::nullopt;
    const std::size_t cipher_len = size - kNonceLen - kTagLen;
    std::vector<std::uint8_t> plain(cipher_len);
    std::array<std::uint8_t, kTagLen> tag{};
    std::memcpy(tag.data(), data + size - kTagLen, kTagLen);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), data) == 1 &&
              EVP_DecryptUpdate(ctx, plain.data(), &len, data + kNonceLen, static_cast<int>(cipher_len)) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen), tag.data()) == 1 &&
              EVP_DecryptFinal_ex(ctx, plain.data() + len, &len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || plain.empty() || plain[0] != sender) return std::nullopt;
    plain.erase(plain.begin());
    return plain;
}

} // namespace BbRoom
