#pragma once
#include "psprecomp/guest_memory.hpp"

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

}
