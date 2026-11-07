// The converted Cheat Device Remastered without the game: it starts with a
// release folder present (and refuses without one), L + UP opens its menu, the
// menu is drawn into the overlay and the game is kept off the pad meanwhile.
// The picture is saved as cheatdevice_menu.ppm for a look.
#include "cheat_device.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

extern int LCS;
extern int global_gametimer;

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "lcs_cheatdevice_test";
    fs::remove_all(base);
    fs::create_directories(base);
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    int failures = 0;

    // No release: refused.
    if (lcs::cheat_device_start(runtime, base, 0x08804000u, 0u, 0u)) {
        std::printf("FAIL started without the release\n");
        ++failures;
    }

    fs::create_directories(base / "cheatdevice_remastered");
    std::ofstream(base / "cheatdevice_remastered" / "cheatdevice_remastered.prx") << "prx";
    std::ofstream(base / "cheatdevice_remastered" / "cheatdevice_names_lcs.ini") << "[NAMES_LCS]\n";

    // No game code to scan here: as if the scan had said Liberty City Stories.
    LCS = 1;
    if (!lcs::cheat_device_start(runtime, base, 0x08804000u, 0u, 0u)) {
        std::printf("FAIL did not start with the release\n");
        return 1;
    }
    // The game's clock, past the plugin's start-up delay.
    global_gametimer = 0x08900000;
    runtime.memory().store32(0x08900000u, 60000u);

    psprecomp::AllegrexContext context{};
    context.gpr[29] = 0x09F00000u;
    const std::uint32_t kLUp = 0x000100u | 0x000010u;
    // Frames: nothing (the menu becomes allowed once it has drawn), hold
    // L + UP (it opens on the press), then release.
    for (int frame = 0; frame < 6; ++frame)
        lcs::cheat_device_frame(runtime, context, frame >= 1 && frame < 3 ? kLUp : 0u, 128, 128, 128, 128);

    std::vector<std::uint8_t> rgba;
    std::uint64_t version = 0u;
    if (!lcs::cheat_device_overlay(rgba, version) ||
        rgba.size() != lcs::kCheatDeviceOverlayWidth * lcs::kCheatDeviceOverlayHeight * 4u) {
        std::printf("FAIL no menu picture\n");
        return 1;
    }
    std::size_t drawn = 0u;
    for (std::size_t index = 3u; index < rgba.size(); index += 4u)
        if (rgba[index] != 0u) ++drawn;
    if (drawn < 10000u) {
        std::printf("FAIL menu picture nearly empty (%zu pixels)\n", drawn);
        ++failures;
    }
    if (lcs::cheat_device_blocked_buttons() == 0u) {
        std::printf("FAIL the game keeps the pad while the menu is open\n");
        ++failures;
    }
    // Over grey, for viewing.
    const char *out = argc > 1 ? argv[1] : "cheatdevice_menu.ppm";
    if (std::FILE *file = std::fopen(out, "wb")) {
        std::fprintf(file, "P6\n%u %u\n255\n", lcs::kCheatDeviceOverlayWidth, lcs::kCheatDeviceOverlayHeight);
        for (std::size_t index = 0; index < rgba.size(); index += 4u) {
            const unsigned alpha = rgba[index + 3u];
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned value = (rgba[index + channel] * alpha + 90u * (255u - alpha)) / 255u;
                std::fputc(static_cast<int>(value), file);
            }
        }
        std::fclose(file);
    }
    fs::remove_all(base);
    std::printf("%s cheat device (%zu pixels drawn, %d bad)\n", failures == 0 ? "ok" : "FAIL", drawn, failures);
    return failures == 0 ? 0 : 1;
}
