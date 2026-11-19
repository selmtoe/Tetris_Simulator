#include "wasm_sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>

namespace tr {

namespace {

constexpr std::array<std::uint32_t, 64> K{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

class Sha256 {
public:
    Sha256() : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

    void update(const unsigned char* data, std::size_t size) {
        totalBytes_ += size;
        while (size > 0) {
            const std::size_t take = std::min(size, block_.size() - blockSize_);
            std::copy(data, data + take, block_.begin() + static_cast<std::ptrdiff_t>(blockSize_));
            blockSize_ += take;
            data += take;
            size -= take;
            if (blockSize_ == block_.size()) {
                transform(block_.data());
                blockSize_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finish() {
        const std::uint64_t bitLength = totalBytes_ * 8u;
        block_[blockSize_++] = 0x80u;
        if (blockSize_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockSize_), block_.end(), 0);
            transform(block_.data());
            blockSize_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockSize_), block_.begin() + 56, 0);
        for (int i = 0; i < 8; ++i) block_[56 + i] = static_cast<unsigned char>(bitLength >> (56 - 8 * i));
        transform(block_.data());
        std::array<unsigned char, 32> result{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            result[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
            result[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
            result[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
            result[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
        }
        return result;
    }

private:
    void transform(const unsigned char* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                       (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const std::uint32_t s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto working = state_;
        for (std::size_t i = 0; i < K.size(); ++i) {
            const std::uint32_t s1 = rotr(working[4], 6) ^ rotr(working[4], 11) ^ rotr(working[4], 25);
            const std::uint32_t choice = (working[4] & working[5]) ^ ((~working[4]) & working[6]);
            const std::uint32_t temp1 = working[7] + s1 + choice + K[i] + words[i];
            const std::uint32_t s0 = rotr(working[0], 2) ^ rotr(working[0], 13) ^ rotr(working[0], 22);
            const std::uint32_t majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const std::uint32_t temp2 = s0 + majority;
            working[7] = working[6]; working[6] = working[5]; working[5] = working[4];
            working[4] = working[3] + temp1; working[3] = working[2]; working[2] = working[1];
            working[1] = working[0]; working[0] = temp1 + temp2;
        }
        for (std::size_t i = 0; i < state_.size(); ++i) state_[i] += working[i];
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> block_{};
    std::size_t blockSize_ = 0;
    std::uint64_t totalBytes_ = 0;
};

} // namespace

std::string wasmSha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    Sha256 hash;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
    }
    const auto digest = hash.finish();
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const unsigned char value : digest) {
        result.push_back(Hex[value >> 4]);
        result.push_back(Hex[value & 15]);
    }
    return result;
}

} // namespace tr
