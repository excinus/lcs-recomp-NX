#pragma once

// Reading saves that a PSP (or PPSSPP) encrypted.
//
// A PSP stores DATA.BIN encrypted when the game hands the savedata utility a
// key, as Liberty City Stories does. PARAM.SFO's SAVEDATA_PARAMS[0] names the
// scheme: 0x01 mode 1 (no game key), 0x21 mode 3, 0x41 mode 5 (game key).
// The file is a 16-byte header followed by the data; the data is XORed with a
// keystream of AES-128 decryptions of counter blocks (CBC-chained), whose
// 12-byte prefix comes from the header, the game key and the KIRK engine's
// fixed keys. This is the decryption only: saves are written back plain.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lcs {

// SAVEDATA_PARAMS[0] to a mode (1, 3, 5), or 0 for a plain save.
[[nodiscard]] int savedata_crypt_mode(std::uint8_t params_flag) noexcept;

// The decrypted data, or nothing for a file too short or a mode it does not
// know. It does not check the result: a wrong key gives noise.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decrypt_psp_savedata(
    std::span<const std::uint8_t> file, int mode, const std::array<std::uint8_t, 16> &game_key);

// True when `data` looks like an encrypted file rather than a game's own data
// (nearly 8 bits of entropy per byte over its first 16 KiB).
[[nodiscard]] bool savedata_looks_encrypted(std::span<const std::uint8_t> data) noexcept;

}  // namespace lcs
