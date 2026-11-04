#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace lcs {

// Folder name under <ini dir>/textures that holds the replacement pictures.
// Empty means the game draws its own button prompts.
[[nodiscard]] const std::string &prompt_texture_style();

// Overwrites the decoded mip 0 of a texture whose content hashes to
// `signature` with the shipped picture and rebuilds the mip chain from it.
void prompt_texture_apply(std::uint64_t signature, std::uint32_t width, std::uint32_t height,
                          std::uint32_t mip_levels, std::span<std::byte> rgba8);

// Changes when the picture prompt_texture_apply would paint for this texture
// changes (the "switch" style's menu chips follow Switch.ConfirmButton), so a
// renderer can drop a cached copy. 0 for textures that are never repainted.
[[nodiscard]] std::uint32_t prompt_texture_variant(std::uint64_t signature);

// Writes the first seen picture of every texture to the dump folder so the
// button prompt atlas can be identified offline. `screen_space` marks draws
// that use through (2D) vertices, which is where the HUD lives.
void prompt_texture_dump(std::uint64_t signature, std::uint32_t width, std::uint32_t height,
                         std::uint32_t address, std::uint32_t format, bool screen_space,
                         std::span<const std::byte> rgba8);

}
