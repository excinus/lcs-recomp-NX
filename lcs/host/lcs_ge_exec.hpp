#pragma once
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lcs {

void execute_ge_list_rendered(psprecomp::GuestMemory &memory, std::uint32_t list_address);

struct GeListPrescan {
    bool finished{};
    std::uint32_t finish_argument{};
};
GeListPrescan prescan_ge_list(psprecomp::GuestMemory &memory, std::uint32_t list_address);

bool rendered_list_finished();
std::uint32_t rendered_finish_argument();
std::uint32_t rendered_render_target();
std::uint32_t rendered_render_stride();

// Block transfers (TRXKICK) run since the last call: how many, and where the
// last one wrote. GE thread only.
struct GeBlockTransfers {
    std::uint32_t count{};
    std::uint32_t last_destination{};
    std::uint32_t last_width{};
    std::uint32_t last_height{};
};
GeBlockTransfers ge_block_transfers_take();

// What the lists since the last call drew: primitive counts by type and the
// first few primitives' state. GE thread only; for the log.
struct GeListDigest {
    struct Primitive {
        std::uint32_t type{};
        std::uint32_t count{};
        std::uint32_t vertex_type{};
        std::uint32_t framebuffer{};
        bool textured{};
        std::uint32_t texture_format{};
        std::uint32_t texture{};
        bool clear{};
    };
    std::uint32_t primitives{};
    std::array<std::uint32_t, 8> by_type{};
    std::array<Primitive, 12> first{};
    std::size_t recorded{};
};
GeListDigest ge_list_digest_take();

}
