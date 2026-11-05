#include "lcs_savedata_crypto.hpp"

#include <cmath>
#include <cstring>

namespace lcs {
namespace {

// ---------------------------------------------------------------------------
// AES-128, decryption of single blocks (FIPS-197). Small and table-light: a
// save is decrypted once, when it is loaded.

constexpr std::uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

using Block = std::array<std::uint8_t, 16>;

struct Aes128 {
    std::array<Block, 11> round_keys{};
    std::uint8_t inverse_sbox[256]{};

    explicit Aes128(const Block &key) {
        for (int index = 0; index < 256; ++index) inverse_sbox[kSbox[index]] = static_cast<std::uint8_t>(index);
        round_keys[0] = key;
        std::uint8_t rcon = 1u;
        for (int round = 1; round <= 10; ++round) {
            const Block &previous = round_keys[round - 1];
            Block &next = round_keys[round];
            std::uint8_t word[4] = {kSbox[previous[13]], kSbox[previous[14]], kSbox[previous[15]], kSbox[previous[12]]};
            word[0] ^= rcon;
            rcon = static_cast<std::uint8_t>((rcon << 1) ^ ((rcon & 0x80u) ? 0x1Bu : 0u));
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    const std::uint8_t before = column == 0 ? word[row] : next[(column - 1) * 4 + row];
                    next[column * 4 + row] = previous[column * 4 + row] ^ before;
                }
            }
        }
    }

    static std::uint8_t times(std::uint8_t value, std::uint8_t factor) {
        std::uint8_t result = 0u;
        while (factor != 0u) {
            if (factor & 1u) result ^= value;
            value = static_cast<std::uint8_t>((value << 1) ^ ((value & 0x80u) ? 0x1Bu : 0u));
            factor >>= 1;
        }
        return result;
    }

    [[nodiscard]] Block decrypt(const Block &input) const {
        Block state = input;
        const auto add = [&](int round) {
            for (int index = 0; index < 16; ++index) state[index] ^= round_keys[round][index];
        };
        const auto inverse_shift_and_sub = [&] {
            Block shifted{};
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    shifted[((column + row) % 4) * 4 + row] = inverse_sbox[state[column * 4 + row]];
            state = shifted;
        };
        add(10);
        for (int round = 9; round >= 0; --round) {
            inverse_shift_and_sub();
            add(round);
            if (round == 0) break;
            for (int column = 0; column < 4; ++column) {
                std::uint8_t *c = &state[column * 4];
                const std::uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
                c[0] = times(a0, 14) ^ times(a1, 11) ^ times(a2, 13) ^ times(a3, 9);
                c[1] = times(a0, 9) ^ times(a1, 14) ^ times(a2, 11) ^ times(a3, 13);
                c[2] = times(a0, 13) ^ times(a1, 9) ^ times(a2, 14) ^ times(a3, 11);
                c[3] = times(a0, 11) ^ times(a1, 13) ^ times(a2, 9) ^ times(a3, 14);
            }
        }
        return state;
    }
};

// ---------------------------------------------------------------------------
// The KIRK engine's fixed AES keys used by the savedata schemes (key slots
// 0x04/0x53 mode 1, 0x0E/0x57 mode 3, 0x12/0x64 mode 5) and the masks the
// system applies around the header step.

constexpr Block kKirk04 = {0x99, 0x24, 0x4c, 0xd2, 0x58, 0xf5, 0x1b, 0xcb, 0xb0, 0x61, 0x9c, 0xa7, 0x38, 0x30, 0x07, 0x5f};
constexpr Block kKirk53 = {0xaf, 0xfe, 0x8e, 0xb1, 0x3d, 0xd1, 0x7e, 0xd8, 0x0a, 0x61, 0x24, 0x1c, 0x95, 0x92, 0x56, 0xb6};
constexpr Block kKirk0E = {0xc8, 0x71, 0xfd, 0xb3, 0xbc, 0xc5, 0xd2, 0xf2, 0xe2, 0xd7, 0x72, 0x9d, 0xdf, 0x82, 0x68, 0x82};
constexpr Block kKirk57 = {0x1c, 0x9b, 0xc4, 0x90, 0xe3, 0x06, 0x64, 0x81, 0xfa, 0x59, 0xfd, 0xb6, 0x00, 0xbb, 0x28, 0x70};
constexpr Block kKirk12 = {0x5d, 0xc7, 0x11, 0x39, 0xd0, 0x19, 0x38, 0xbc, 0x02, 0x7f, 0xdd, 0xdc, 0xb0, 0x83, 0x7d, 0x9d};
constexpr Block kKirk64 = {0x03, 0xb3, 0x02, 0xe8, 0x5f, 0xf3, 0x81, 0xb1, 0x3b, 0x8d, 0xaa, 0x2a, 0x90, 0xff, 0x5e, 0x61};

constexpr Block kMode3In = {0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03};
constexpr Block kMode3Out = {0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72};
constexpr Block kMode5In = {0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00};
constexpr Block kMode5Out = {0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B};

void xor_into(Block &target, const Block &source) {
    for (int index = 0; index < 16; ++index) target[index] ^= source[index];
}

}  // namespace

int savedata_crypt_mode(std::uint8_t params_flag) noexcept {
    switch (params_flag) {
    case 0x00: return 0;
    case 0x01: return 1;
    case 0x21: return 3;
    case 0x41: return 5;
    default: return 1;
    }
}

std::optional<std::vector<std::uint8_t>> decrypt_psp_savedata(std::span<const std::uint8_t> file, int mode,
                                                             const std::array<std::uint8_t, 16> &game_key) {
    if (file.size() <= 16u) return std::nullopt;
    const Block *header_key = nullptr;
    const Block *stream_key = nullptr;
    const Block *mask_in = nullptr;
    const Block *mask_out = nullptr;
    bool keyed = true;
    switch (mode) {
    case 1: header_key = &kKirk04; stream_key = &kKirk53; keyed = false; break;
    case 3: header_key = &kKirk0E; stream_key = &kKirk57; mask_in = &kMode3In; mask_out = &kMode3Out; break;
    case 5: header_key = &kKirk12; stream_key = &kKirk64; mask_in = &kMode5In; mask_out = &kMode5Out; break;
    default: return std::nullopt;
    }

    // The counter prefix: the header, keyed by the game, through the header key.
    Block seed{};
    std::memcpy(seed.data(), file.data(), 16u);
    if (keyed) xor_into(seed, game_key);
    if (mask_in != nullptr) xor_into(seed, *mask_in);
    seed = Aes128(*header_key).decrypt(seed);
    if (mask_out != nullptr) xor_into(seed, *mask_out);

    // Keystream block j: AES-decrypt(counter j) XOR counter j-1 (CBC over the
    // counters, from a zero block), counter j = seed[0..11] | j as LE u32, j from 1.
    const Aes128 stream(*stream_key);
    const std::size_t size = file.size() - 16u;
    std::vector<std::uint8_t> out(size);
    Block previous{};
    Block counter = seed;
    for (std::size_t offset = 0u, block = 1u; offset < size; offset += 16u, ++block) {
        const auto number = static_cast<std::uint32_t>(block);
        counter[12] = static_cast<std::uint8_t>(number);
        counter[13] = static_cast<std::uint8_t>(number >> 8u);
        counter[14] = static_cast<std::uint8_t>(number >> 16u);
        counter[15] = static_cast<std::uint8_t>(number >> 24u);
        Block keystream = stream.decrypt(counter);
        xor_into(keystream, previous);
        previous = counter;
        const std::size_t count = size - offset < 16u ? size - offset : 16u;
        for (std::size_t index = 0; index < count; ++index)
            out[offset + index] = file[16u + offset + index] ^ keystream[index];
    }
    return out;
}

bool savedata_looks_encrypted(std::span<const std::uint8_t> data) noexcept {
    const std::size_t length = data.size() < 16384u ? data.size() : 16384u;
    if (length < 1024u) return false;
    std::size_t counts[256]{};
    for (std::size_t index = 0; index < length; ++index) ++counts[data[index]];
    double entropy = 0.0;
    for (std::size_t count : counts) {
        if (count == 0u) continue;
        const double p = static_cast<double>(count) / static_cast<double>(length);
        entropy -= p * std::log2(p);
    }
    return entropy > 7.0;
}

}  // namespace lcs
