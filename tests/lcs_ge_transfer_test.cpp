// The GE's block transfer (TRXKICK): a 32-bit picture in RAM copied into a
// VRAM framebuffer at an offset.
#include "lcs_ge_exec.hpp"

#include "psprecomp/guest_memory.hpp"

#include <cstdio>

int main() {
    psprecomp::GuestMemory memory(64u * 1024u * 1024u);
    constexpr std::uint32_t kList = 0x08900000u;
    constexpr std::uint32_t kPicture = 0x08A00000u;  // 16x8, stride 16
    constexpr std::uint32_t kFramebuffer = 0x04088000u;  // stride 512
    for (std::uint32_t y = 0; y < 8u; ++y)
        for (std::uint32_t x = 0; x < 16u; ++x)
            memory.store32(kPicture + (y * 16u + x) * 4u, 0xFF000000u | (y << 8u) | x);
    const std::uint32_t list[] = {
        0xB0000000u | (kPicture & 0x00FFFFF0u),
        0xB1000000u | ((kPicture >> 8u) & 0x00FF0000u) | 16u,
        0xB2000000u | (kFramebuffer & 0x00FFFFF0u),
        0xB3000000u | ((kFramebuffer >> 8u) & 0x00FF0000u) | 512u,
        0xEB000000u | (2u << 10u) | 4u,           // source x 4, y 2
        0xEC000000u | (100u << 10u) | 200u,       // destination x 200, y 100
        0xEE000000u | ((3u - 1u) << 10u) | (5u - 1u),  // 5x3
        0xEA000001u,                              // 32-bit pixels
        0x0F000000u, 0x0C000000u,
    };
    for (std::uint32_t index = 0; index < sizeof(list) / sizeof(list[0]); ++index)
        memory.store32(kList + index * 4u, list[index]);
    lcs::execute_ge_list_rendered(memory, kList);
    int failures = 0;
    for (std::uint32_t y = 0; y < 3u; ++y)
        for (std::uint32_t x = 0; x < 5u; ++x) {
            const std::uint32_t got = memory.load32(kFramebuffer + ((100u + y) * 512u + 200u + x) * 4u);
            const std::uint32_t want = 0xFF000000u | ((2u + y) << 8u) | (4u + x);
            if (got != want) ++failures;
        }
    if (memory.load32(kFramebuffer + (100u * 512u + 205u) * 4u) != 0u) ++failures;  // right of the rectangle
    std::printf("%s block transfer (%d bad)\n", failures == 0 ? "ok" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
