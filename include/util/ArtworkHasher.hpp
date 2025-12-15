#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace ouroboros::util {

class ArtworkHasher {
public:
    // SHA-256 hash (returns 64-char hex string for content-addressed storage)
    static std::string hash_artwork(const std::vector<uint8_t>& data);

    // Fast FNV-1a hash for quick equality checks (same as ImageRenderer uses)
    static size_t fast_hash(const std::vector<uint8_t>& data);

private:
    // SHA-256 internal implementation
    static constexpr size_t SHA256_BLOCK_SIZE = 64;
    static constexpr size_t SHA256_HASH_SIZE = 32;

    static std::vector<uint8_t> sha256(const uint8_t* data, size_t len);
    static std::string to_hex(const std::vector<uint8_t>& bytes);

    // SHA-256 internal functions
    static uint32_t rotr(uint32_t x, uint32_t n);
    static uint32_t ch(uint32_t x, uint32_t y, uint32_t z);
    static uint32_t maj(uint32_t x, uint32_t y, uint32_t z);
    static uint32_t sigma0(uint32_t x);
    static uint32_t sigma1(uint32_t x);
    static uint32_t gamma0(uint32_t x);
    static uint32_t gamma1(uint32_t x);
};

}  // namespace ouroboros::util
