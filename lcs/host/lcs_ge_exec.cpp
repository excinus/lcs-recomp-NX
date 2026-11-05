#include "lcs_ge_exec.hpp"

#include "ge_renderer.hpp"
#include "lcs_menu.hpp"

#include <array>
#include <cstring>
#include <string>

namespace lcs {
namespace {

std::array<std::uint32_t, 256> ge_commands{};
GeTransformState ge_transform{};
std::uint32_t ge_base_register = 0u;
std::uint32_t ge_offset_address = 0u;
std::uint32_t ge_vertex_address = 0u;
std::uint32_t ge_index_address = 0u;
bool ge_finish_seen = false;
std::uint32_t ge_finish_arg = 0u;

constexpr std::uint32_t kMaxCommands = 2'000'000u;

GeBlockTransfers block_transfers{};
GeListDigest list_digest{};

void note_primitive(std::uint32_t data) {
    GeListDigest &digest = list_digest;
    const std::uint32_t primitive = (data >> 16u) & 7u;
    ++digest.primitives;
    ++digest.by_type[primitive];
    if (digest.recorded < digest.first.size()) {
        GeListDigest::Primitive &record = digest.first[digest.recorded++];
        record.type = primitive;
        record.count = data & 0xFFFFu;
        record.vertex_type = ge_commands[0x12u] & 0x00FFFFFFu;
        record.framebuffer = ((ge_commands[0x9Cu] & 0x00FFFFF0u) | ((ge_commands[0x9Du] & 0x00FF0000u) << 8u));
        record.textured = (ge_commands[0x1Eu] & 1u) != 0u;
        record.texture_format = ge_commands[0xC3u] & 0xFu;
        record.texture = (ge_commands[0xA0u] & 0x00FFFFF0u) | ((ge_commands[0xA8u] & 0x000F0000u) << 8u);
        record.clear = (ge_commands[0xD3u] & 1u) != 0u;
    }
}

// TRXKICK (0xEA): the GE's rectangle copy between two buffers in guest
// memory, as the game uses it to put a whole picture (a loading card) into a
// framebuffer. Registers: 0xB0/0xB1 source address and stride, 0xB2/0xB3
// destination, 0xEB/0xEC source and destination x,y, 0xEE size; bit 0 of the
// kick selects 32-bit pixels.
void run_block_transfer(psprecomp::GuestMemory &memory, std::uint32_t kick) {
    const auto address = [](std::uint32_t low, std::uint32_t high) {
        return ((low & 0x00FFFFF0u) | ((high & 0x00FF0000u) << 8u)) & 0x0FFFFFFFu;
    };
    const std::uint32_t source = address(ge_commands[0xB0u], ge_commands[0xB1u]);
    const std::uint32_t destination = address(ge_commands[0xB2u], ge_commands[0xB3u]);
    const std::uint32_t source_stride = ge_commands[0xB1u] & 0x7F8u;
    const std::uint32_t destination_stride = ge_commands[0xB3u] & 0x7F8u;
    const std::uint32_t source_x = ge_commands[0xEBu] & 0x3FFu;
    const std::uint32_t source_y = (ge_commands[0xEBu] >> 10u) & 0x3FFu;
    const std::uint32_t destination_x = ge_commands[0xECu] & 0x3FFu;
    const std::uint32_t destination_y = (ge_commands[0xECu] >> 10u) & 0x3FFu;
    const std::uint32_t width = (ge_commands[0xEEu] & 0x3FFu) + 1u;
    const std::uint32_t height = ((ge_commands[0xEEu] >> 10u) & 0x3FFu) + 1u;
    const std::uint32_t bytes_per_pixel = (kick & 1u) != 0u ? 4u : 2u;
    ++block_transfers.count;
    block_transfers.last_destination = destination;
    block_transfers.last_width = width;
    block_transfers.last_height = height;
    if (source_stride == 0u || destination_stride == 0u) return;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
    for (std::uint32_t row = 0u; row < height; ++row) {
        const std::uint32_t from = source + ((source_y + row) * source_stride + source_x) * bytes_per_pixel;
        const std::uint32_t to = destination + ((destination_y + row) * destination_stride + destination_x) * bytes_per_pixel;
        const std::uint8_t *in = memory.raw_pointer(from, row_bytes);
        std::uint8_t *out = memory.raw_pointer(to, row_bytes);
        if (in == nullptr || out == nullptr) return;
        std::memmove(out, in, row_bytes);
    }
}

std::uint32_t relative_address(std::uint32_t data24) {
    const std::uint32_t extended = ((ge_base_register & 0x000F0000u) << 8u) | (data24 & 0x00FFFFFFu);
    return (ge_offset_address + extended) & 0x0FFFFFFFu;
}

}  // namespace

void execute_ge_list_rendered(psprecomp::GuestMemory &memory, std::uint32_t list_address) {
    ge_finish_seen = false;
    ge_finish_arg = 0u;
    if (list_address == 0u) return;

    std::array<std::uint32_t, 16> call_stack{};
    std::uint32_t call_depth = 0u;
    std::uint32_t pc = list_address;

    for (std::uint32_t executed = 0u; executed < kMaxCommands; ++executed) {
        if (!memory.contains(pc, 4u)) break;
        const std::uint32_t word = memory.load32(pc);
        const std::uint32_t op_pc = pc;
        pc = (pc + 4u) & 0x0FFFFFFFu;
        const std::uint32_t command = word >> 24u;
        const std::uint32_t data = word & 0x00FFFFFFu;
        ge_commands[command] = data;

        if (command >= 0x2Au && command <= 0x3Fu)
            update_ge_transform_state(ge_transform, command, data);

        switch (command) {
        case 0x00u:
            break;
        case 0x01u:
            ge_vertex_address = relative_address(data);
            break;
        case 0x02u:
            ge_index_address = relative_address(data);
            break;
        case 0x10u:
            ge_base_register = data;
            break;
        case 0x13u:
            ge_offset_address = data << 8u;
            break;
        case 0x08u:
            pc = relative_address(data);
            break;
        case 0x09u:
            break;
        case 0x0Au:
            if (call_depth < call_stack.size()) call_stack[call_depth++] = pc;
            pc = relative_address(data);
            break;
        case 0x0Bu:
            if (call_depth != 0u) pc = call_stack[--call_depth];
            break;
        case 0x0Fu:
            ge_finish_seen = true;
            ge_finish_arg = data;
            break;
        case 0xEAu:
            run_block_transfer(memory, data);
            break;
        case 0x0Cu:
            flush_ge_deferred_rasterization(memory);
            return;
        case 0x04u: {
            GeRenderStats stats{};
            std::string error;
            lcs_set_ge_command(op_pc);
            note_primitive(data);
            if (!render_ge_primitive(memory, ge_commands, ge_transform, ge_vertex_address,
                                     ge_index_address, data, stats, error)) {
                flush_ge_deferred_rasterization(memory);
                return;
            }
            ge_vertex_address = stats.next_vertex_address;
            ge_index_address = stats.next_index_address;
            break;
        }
        default:
            break;
        }
    }
    flush_ge_deferred_rasterization(memory);
}

GeListDigest ge_list_digest_take() {
    const GeListDigest taken = list_digest;
    list_digest = GeListDigest{};
    return taken;
}

GeBlockTransfers ge_block_transfers_take() {
    const GeBlockTransfers taken = block_transfers;
    block_transfers = GeBlockTransfers{};
    return taken;
}

GeListPrescan prescan_ge_list(psprecomp::GuestMemory &memory, std::uint32_t list_address) {
    GeListPrescan result{};
    if (list_address == 0u) return result;
    // Its own copy of BASE and OFFSET, carried from list to list as the GE
    // carries them: the game thread scans a list while the GE worker may still
    // be executing an earlier one with the real registers.
    static std::uint32_t shadow_base = 0u;
    static std::uint32_t shadow_offset = 0u;
    std::uint32_t &base_register = shadow_base;
    std::uint32_t &offset_address = shadow_offset;
    const auto relative = [&](std::uint32_t data24) {
        const std::uint32_t extended = ((base_register & 0x000F0000u) << 8u) | (data24 & 0x00FFFFFFu);
        return (offset_address + extended) & 0x0FFFFFFFu;
    };
    std::array<std::uint32_t, 16> call_stack{};
    std::uint32_t call_depth = 0u;
    std::uint32_t pc = list_address;
    for (std::uint32_t executed = 0u; executed < kMaxCommands; ++executed) {
        if (!memory.contains(pc, 4u)) break;
        const std::uint32_t word = memory.load32(pc);
        pc = (pc + 4u) & 0x0FFFFFFFu;
        const std::uint32_t command = word >> 24u;
        const std::uint32_t data = word & 0x00FFFFFFu;
        switch (command) {
        case 0x10u: base_register = data; break;
        case 0x13u: offset_address = data << 8u; break;
        case 0x08u: pc = relative(data); break;
        case 0x0Au:
            if (call_depth < call_stack.size()) call_stack[call_depth++] = pc;
            pc = relative(data);
            break;
        case 0x0Bu:
            if (call_depth != 0u) pc = call_stack[--call_depth];
            break;
        case 0x0Fu:
            result.finished = true;
            result.finish_argument = data;
            break;
        case 0x0Cu:
            return result;
        default:
            break;
        }
    }
    return result;
}

bool rendered_list_finished() { return ge_finish_seen; }
std::uint32_t rendered_finish_argument() { return ge_finish_arg; }

std::uint32_t rendered_render_target() {
    return 0x04000000u | (ge_commands[0x9Cu] & 0x001FFFF0u);
}

std::uint32_t rendered_render_stride() { return ge_commands[0x9Du] & 0x7FCu; }

}
