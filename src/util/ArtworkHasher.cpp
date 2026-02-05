#include "util/ArtworkHasher.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace ouroboros::util {

// SHA-256 constants (first 32 bits of the fractional parts of the cube roots of the first 64 primes)
static constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

constexpr uint32_t ArtworkHasher::rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

constexpr uint32_t ArtworkHasher::ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

constexpr uint32_t ArtworkHasher::maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

constexpr uint32_t ArtworkHasher::sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

constexpr uint32_t ArtworkHasher::sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

constexpr uint32_t ArtworkHasher::gamma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

constexpr uint32_t ArtworkHasher::gamma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

std::vector<uint8_t> ArtworkHasher::sha256(const uint8_t* data, size_t len) {
    // Initial hash values (first 32 bits of the fractional parts of the square roots of the first 8 primes)
    uint32_t H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Pre-processing: adding padding
    size_t msg_len = len;
    size_t bit_len = msg_len * 8;

    // Calculate padded length (multiple of 512 bits = 64 bytes)
    size_t padded_len = msg_len + 1; // +1 for the '1' bit
    while ((padded_len % 64) != 56) {  // Leave room for 64-bit length
        padded_len++;
    }
    padded_len += 8;  // Add 64-bit length

    std::vector<uint8_t> padded(padded_len);
    std::memcpy(padded.data(), data, msg_len);

    // Append '1' bit (0x80 in byte form)
    padded[msg_len] = 0x80;

    // Append length in bits as 64-bit big-endian
    for (int i = 0; i < 8; ++i) {
        padded[padded_len - 1 - i] = (bit_len >> (i * 8)) & 0xFF;
    }

    // Process message in 512-bit chunks
    for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
        uint32_t W[64];

        // Prepare message schedule
        for (int t = 0; t < 16; ++t) {
            W[t] = (padded[chunk + t * 4] << 24) |
                   (padded[chunk + t * 4 + 1] << 16) |
                   (padded[chunk + t * 4 + 2] << 8) |
                   (padded[chunk + t * 4 + 3]);
        }

        for (int t = 16; t < 64; ++t) {
            W[t] = gamma1(W[t - 2]) + W[t - 7] + gamma0(W[t - 15]) + W[t - 16];
        }

        // Initialize working variables
        uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
        uint32_t e = H[4], f = H[5], g = H[6], h = H[7];

        // Main loop
        for (int t = 0; t < 64; ++t) {
            uint32_t T1 = h + sigma1(e) + ch(e, f, g) + K[t] + W[t];
            uint32_t T2 = sigma0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + T1;
            d = c;
            c = b;
            b = a;
            a = T1 + T2;
        }

        // Add compressed chunk to current hash value
        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }

    // Produce final hash (big-endian)
    std::vector<uint8_t> hash(32);
    for (int i = 0; i < 8; ++i) {
        hash[i * 4]     = (H[i] >> 24) & 0xFF;
        hash[i * 4 + 1] = (H[i] >> 16) & 0xFF;
        hash[i * 4 + 2] = (H[i] >> 8) & 0xFF;
        hash[i * 4 + 3] = H[i] & 0xFF;
    }

    return hash;
}

std::string ArtworkHasher::to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

std::string ArtworkHasher::hash_artwork(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return "";
    }

    auto hash_bytes = sha256(data.data(), data.size());
    return to_hex(hash_bytes);
}

size_t ArtworkHasher::fast_hash(const std::vector<uint8_t>& data) {
    // FNV-1a hash (same algorithm as ImageRenderer for consistency)
    size_t hash = 14695981039346656037ULL;  // FNV offset basis

    // For large data (>64KB), sample every 1KB instead of every byte
    size_t step = (data.size() > 65536) ? (data.size() / 65536) : 1;

    for (size_t i = 0; i < data.size(); i += step) {
        hash ^= data[i];
        hash *= 1099511628211ULL;  // FNV prime
    }

    return hash;
}

}  // namespace ouroboros::util
