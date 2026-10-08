#pragma once
// SHA-1 and AES-128-CBC decryption, enough to check and decrypt the player's WAD (no third-party
// crypto library needed).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class Sha1 {
public:
    void update(const uint8_t* data, size_t size) {
        length_ += size;
        while (size > 0) {
            size_t take = std::min(size, sizeof block_ - used_);
            std::memcpy(block_ + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
            if (used_ == sizeof block_) {
                compress();
                used_ = 0;
            }
        }
    }

    // Finishes; call once.
    std::array<uint8_t, 20> digest() {
        finish();
        std::array<uint8_t, 20> out{};
        for (int i = 0; i < 20; i++) {
            out[i] = static_cast<uint8_t>(h_[i / 4] >> (24 - 8 * (i % 4)));
        }
        return out;
    }

    // Finishes; call once.
    std::string hex() {
        finish();
        char text[41];
        for (int i = 0; i < 5; i++) {
            std::snprintf(text + 8 * i, 9, "%08x", h_[i]);
        }
        return text;
    }

private:
    void finish() {
        uint64_t bits = length_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used_ != 56) {
            update(&zero, 1);
        }
        uint8_t tail[8];
        for (int i = 0; i < 8; i++) {
            tail[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        }
        update(tail, 8);
    }

    static uint32_t rotl(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

    void compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(block_[4 * i]) << 24) | (uint32_t(block_[4 * i + 1]) << 16) | (uint32_t(block_[4 * i + 2]) << 8) | block_[4 * i + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f;
            uint32_t k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t block_[64] = {};
    size_t used_ = 0;
    uint64_t length_ = 0;
};

// AES-128 decryption in CBC mode, in place (size a multiple of 16; iv is updated so a long buffer
// can be decrypted in pieces).
class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]);
    void decrypt_cbc(uint8_t* data, size_t size, uint8_t iv[16]) const;

private:
    void decrypt_block(uint8_t block[16]) const;
    uint8_t round_keys_[176];
};
