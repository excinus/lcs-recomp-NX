/*
 *  Cheat Device Remastered in LCSNative: starting it, the frame hook, the
 *  pad and the overlay its menu is drawn into.
 *
 *  On the PSP the plugin hooked the game: a call in the frame-rate limiter
 *  (sceKernelGetSystemTimeWidePatched: the cheats), the game's pad routine
 *  (buttonInput, and blocking keys while the menu is open) and the world
 *  renderer (draw: the menu, through the game's font renderer). The port's
 *  game code is recompiled and cannot be patched that way, so this file
 *  drives the same functions from the port's vblank wait, filters the pad in
 *  the port's sceCtrl, and draws the menu's text and boxes itself.
 *
 *  Copyright (C) 2026, LCSNative contributors
 *  GPL-3.0-or-later, see LICENSE in this folder.
 */
#include "cheat_device.hpp"

#include "cdr_font.hpp"
#include "cdr_host_internal.h"
#include "cdr_platform.h"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

// The converted plugin (main.cpp, cheats.cpp, lang.cpp).
extern const char *basefolder;
extern short flag_keys_disable;
extern short flag_menu_running;
extern int gametimer;
extern int LCS;
extern int currLanguageID;
extern const int menuopendelay;
int module_start(SceSize argc, void *argp);
int OnModuleStart(u32 text_addr, u32 text_size, u32 data_size);
int cdr_check_scan_results(void);
SceInt64 sceKernelGetSystemTimeWidePatched(void);
void buttonInput();
void draw();
void langTableSetup(int langIndex);

namespace {

bool g_running = false;
std::atomic<bool> g_exit_requested{false};
std::string g_base_folder_text;

// ---------------------------------------------------------------------------
// The overlay: what draw() queued this frame, then rasterized.

struct QueuedText {
    std::string text;
    int origin;
    float scale;
    bool shadow;
    float x, y;
    std::uint32_t color;
};
struct QueuedBox {
    float x, y, width, height;
    std::uint32_t color;
};

std::vector<QueuedText> g_texts;
std::vector<QueuedBox> g_boxes;

std::mutex g_overlay_mutex;
std::vector<std::uint8_t> g_overlay;
std::uint64_t g_overlay_version = 0u;
bool g_overlay_visible = false;
bool g_overlay_was_empty = true;

constexpr int kWidth = static_cast<int>(lcs::kCheatDeviceOverlayWidth);
constexpr int kHeight = static_cast<int>(lcs::kCheatDeviceOverlayHeight);
constexpr float kScreenScale = 2.0f;  // the plugin's 480x272 to the overlay's 960x544

// The plugin's colours are 0xAABBGGRR.
struct Rgba {
    std::uint8_t r, g, b, a;
};
Rgba unpack(std::uint32_t color) {
    return Rgba{static_cast<std::uint8_t>(color), static_cast<std::uint8_t>(color >> 8u),
                static_cast<std::uint8_t>(color >> 16u), static_cast<std::uint8_t>(color >> 24u)};
}

void blend(std::uint8_t *pixel, Rgba color, int coverage) {
    const int alpha = color.a * coverage / 255;
    if (alpha <= 0) return;
    const int inverse = 255 - alpha;
    const int out_alpha = alpha + pixel[3] * inverse / 255;
    if (out_alpha <= 0) return;
    const auto mix = [&](int source, int destination) {
        return static_cast<std::uint8_t>((source * alpha + destination * pixel[3] * inverse / 255) / out_alpha);
    };
    pixel[0] = mix(color.r, pixel[0]);
    pixel[1] = mix(color.g, pixel[1]);
    pixel[2] = mix(color.b, pixel[2]);
    pixel[3] = static_cast<std::uint8_t>(out_alpha);
}

void fill_box(std::vector<std::uint8_t> &image, const QueuedBox &box) {
    const int x0 = std::max(0, static_cast<int>(box.x * kScreenScale + 0.5f));
    const int y0 = std::max(0, static_cast<int>(box.y * kScreenScale + 0.5f));
    const int x1 = std::min(kWidth, static_cast<int>((box.x + box.width) * kScreenScale + 0.5f));
    const int y1 = std::min(kHeight, static_cast<int>((box.y + box.height) * kScreenScale + 0.5f));
    const Rgba color = unpack(box.color);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) blend(&image[(static_cast<std::size_t>(y) * kWidth + x) * 4u], color, 255);
}

// Characters of the text as Latin-1 (the menu is UTF-8; the font covers
// Latin-1, which is what the plugin's translations use).
std::vector<unsigned char> to_latin1(const std::string &text) {
    std::vector<unsigned char> out;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char c = static_cast<unsigned char>(text[index]);
        if (c < 0x80u) {
            out.push_back(c);
        } else if ((c & 0xE0u) == 0xC0u && index + 1u < text.size()) {
            const unsigned code = ((c & 0x1Fu) << 6u) | (static_cast<unsigned char>(text[index + 1u]) & 0x3Fu);
            out.push_back(code < 256u ? static_cast<unsigned char>(code) : '?');
            ++index;
        } else if ((c & 0xF0u) == 0xE0u) {
            out.push_back('?');
            index += 2u;
        } else if ((c & 0xF8u) == 0xF0u) {
            out.push_back('?');
            index += 3u;
        } else {
            out.push_back(c);  // already Latin-1
        }
    }
    return out;
}

// GTA's ~x~ codes: colours, ~n~ new line; others are dropped.
struct Run {
    std::vector<unsigned char> chars;
    Rgba color;
    bool newline;
};
std::vector<Run> split_runs(const std::vector<unsigned char> &text, Rgba base) {
    std::vector<Run> runs;
    Run current{{}, base, false};
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '~' && index + 2u < text.size() && text[index + 2u] == '~') {
            const char code = static_cast<char>(std::tolower(text[index + 1u]));
            Rgba color = current.color;
            bool newline = false;
            switch (code) {
            case 'r': color = Rgba{0xE0, 0x30, 0x30, base.a}; break;
            case 'g': color = Rgba{0x40, 0xD0, 0x40, base.a}; break;
            case 'b': color = Rgba{0x50, 0x80, 0xF0, base.a}; break;
            case 'y': color = Rgba{0xF0, 0xE0, 0x50, base.a}; break;
            case 'p': color = Rgba{0xB0, 0x60, 0xE0, base.a}; break;
            case 'o': color = Rgba{0xF0, 0x90, 0x30, base.a}; break;
            case 'l': color = Rgba{0x00, 0x00, 0x00, base.a}; break;
            case 'w': case 'h': color = Rgba{0xFF, 0xFF, 0xFF, base.a}; break;
            case 's': color = base; break;
            case 'n': newline = true; break;
            default: break;
            }
            if (!current.chars.empty() || newline) runs.push_back(current);
            current = Run{{}, color, newline};
            index += 2u;
            continue;
        }
        current.chars.push_back(text[index]);
    }
    if (!current.chars.empty()) runs.push_back(current);
    return runs;
}

const cdr_font::Glyph *glyph_for(unsigned char c) {
    if (c < 32u) return nullptr;
    const cdr_font::Glyph &glyph = cdr_font::kGlyphs[c - 32u];
    if (glyph.advance == 0u && c != 32u) return &cdr_font::kGlyphs['?' - 32];
    return &glyph;
}

float run_width(const std::vector<unsigned char> &chars, float pixel_scale) {
    float width = 0.0f;
    for (unsigned char c : chars)
        if (const cdr_font::Glyph *glyph = glyph_for(c)) width += static_cast<float>(glyph->advance) * pixel_scale;
    return width;
}

void draw_glyph(std::vector<std::uint8_t> &image, const cdr_font::Glyph &glyph, float pen_x, float baseline,
                float pixel_scale, Rgba color) {
    if (glyph.width == 0u || glyph.height == 0u) return;
    const float left = pen_x + static_cast<float>(glyph.left) * pixel_scale;
    const float top = baseline - static_cast<float>(cdr_font::kAscent) * pixel_scale +
                      static_cast<float>(glyph.top) * pixel_scale;
    const int x0 = std::max(0, static_cast<int>(left));
    const int y0 = std::max(0, static_cast<int>(top));
    const int x1 = std::min(kWidth, static_cast<int>(left + glyph.width * pixel_scale + 1.0f));
    const int y1 = std::min(kHeight, static_cast<int>(top + glyph.height * pixel_scale + 1.0f));
    const std::uint8_t *alpha = cdr_font::kAlpha + glyph.offset;
    for (int y = y0; y < y1; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f - top) / pixel_scale - 0.5f;
        for (int x = x0; x < x1; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f - left) / pixel_scale - 0.5f;
            // Bilinear sample of the glyph's coverage.
            const int ix = static_cast<int>(std::floor(sx));
            const int iy = static_cast<int>(std::floor(sy));
            const float fx = sx - static_cast<float>(ix);
            const float fy = sy - static_cast<float>(iy);
            const auto sample = [&](int gx, int gy) -> float {
                if (gx < 0 || gy < 0 || gx >= glyph.width || gy >= glyph.height) return 0.0f;
                return static_cast<float>(alpha[gy * glyph.width + gx]);
            };
            const float value = (sample(ix, iy) * (1.0f - fx) + sample(ix + 1, iy) * fx) * (1.0f - fy) +
                                (sample(ix, iy + 1) * (1.0f - fx) + sample(ix + 1, iy + 1) * fx) * fy;
            const int coverage = static_cast<int>(value + 0.5f);
            if (coverage > 0) blend(&image[(static_cast<std::size_t>(y) * kWidth + x) * 4u], color, coverage);
        }
    }
}

void draw_text(std::vector<std::uint8_t> &image, const QueuedText &text) {
    // The game's font at scale 1.0 is about as tall as this one's line.
    const float pixel_scale = std::max(0.2f, text.scale);
    const Rgba base = unpack(text.color);
    const std::vector<Run> runs = split_runs(to_latin1(text.text), base);
    // Lines, for alignment.
    std::vector<std::vector<const Run *>> lines(1);
    for (const Run &run : runs) {
        if (run.newline) lines.emplace_back();
        lines.back().push_back(&run);
    }
    float y = text.y * kScreenScale;
    const float line_height = static_cast<float>(cdr_font::kLineHeight) * pixel_scale;
    for (const auto &line : lines) {
        float width = 0.0f;
        for (const Run *run : line) width += run_width(run->chars, pixel_scale);
        float x = text.x * kScreenScale;
        if (text.origin == 2 /* ALIGN_CENTER */) x -= width / 2.0f;
        else if (text.origin == 4 /* ALIGN_RIGHT */) x -= width;
        else if (text.origin == 6 /* ALIGN_SCREENCENTER */) x = (kWidth - width) / 2.0f;
        const float baseline = y + static_cast<float>(cdr_font::kAscent) * pixel_scale;
        for (const Run *run : line) {
            for (unsigned char c : run->chars) {
                const cdr_font::Glyph *glyph = glyph_for(c);
                if (glyph == nullptr) continue;
                if (text.shadow)
                    draw_glyph(image, *glyph, x + 2.0f, baseline + 2.0f, pixel_scale, Rgba{0, 0, 0, run->color.a});
                draw_glyph(image, *glyph, x, baseline, pixel_scale, run->color);
                x += static_cast<float>(glyph->advance) * pixel_scale;
            }
        }
        y += line_height;
    }
}

void publish_overlay() {
    const bool empty = g_texts.empty() && g_boxes.empty();
    if (empty) {
        if (!g_overlay_was_empty) {
            std::lock_guard lock(g_overlay_mutex);
            g_overlay_visible = false;
            ++g_overlay_version;
        }
        g_overlay_was_empty = true;
        return;
    }
    static std::vector<std::uint8_t> image;
    image.assign(static_cast<std::size_t>(kWidth) * kHeight * 4u, 0u);
    // The plugin queued boxes and texts separately and drew boxes first.
    for (const QueuedBox &box : g_boxes) fill_box(image, box);
    for (const QueuedText &text : g_texts) draw_text(image, text);
    g_texts.clear();
    g_boxes.clear();
    g_overlay_was_empty = false;
    std::lock_guard lock(g_overlay_mutex);
    g_overlay.swap(image);
    g_overlay_visible = true;
    ++g_overlay_version;
}

bool file_exists(const std::filesystem::path &path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

}  // namespace

// ---------------------------------------------------------------------------
// The converted drawString / drawBox.

void cdr_overlay_text(const char *text, int origin, short, float scale, short shadow, float x, float y,
                      std::uint32_t color) {
    if (text == nullptr || *text == '\0') return;
    g_texts.push_back(QueuedText{text, origin, scale, shadow != 0, x, y, color});
}

void cdr_overlay_box(float x, float y, float width, float height, std::uint32_t color) {
    g_boxes.push_back(QueuedBox{x, y, width, height, color});
}

void cdr_request_exit() noexcept { g_exit_requested = true; }

namespace lcs {

bool cheat_device_start(psprecomp::Runtime &runtime, const std::filesystem::path &base_folder,
                        std::uint32_t text_address, std::uint32_t text_size, std::uint32_t data_size) {
    const std::filesystem::path plugin = base_folder / "cheatdevice_remastered";
    // The release from https://github.com/Freakler/CheatDeviceRemastered/releases
    // must be there: its names list is read at run time, and the plugin
    // itself is the proof that the player downloaded it.
    const bool release = file_exists(plugin / "cheatdevice_remastered.prx") &&
                         file_exists(plugin / "cheatdevice_names_lcs.ini");
    if (!release) {
        std::cout << "[cheatdevice] not started: copy the cheatdevice_remastered folder of the official "
                     "release (https://github.com/Freakler/CheatDeviceRemastered/releases) to "
                  << plugin.string() << " (it needs cheatdevice_remastered.prx and cheatdevice_names_lcs.ini)\n"
                  << std::flush;
        return false;
    }
    cdr_bind_runtime(&runtime);
    g_base_folder_text = plugin.generic_string() + "/";
    cdr_set_folders(g_base_folder_text, base_folder.generic_string() + "/");
    basefolder = g_base_folder_text.c_str();

    module_start(0, nullptr);
    if (OnModuleStart(text_address, text_size, data_size) != 0 || LCS != 1) {
        std::cout << "[cheatdevice] not started: the game's code did not match Liberty City Stories\n"
                  << std::flush;
        return false;
    }
    const int missing = cdr_check_scan_results();
    const auto patches = cdr_code_patches();
    // Where the plugin would have patched the game's code: the places later
    // stages hook in the recompiled code (addresses of this game version).
    for (const CdrCodePatch &patch : patches)
        std::printf("[cheatdevice] code patch %s at 0x%08X -> %s\n", patch.kind, patch.address, patch.target);
    std::cout << "[cheatdevice] Cheat Device Remastered by Freakler "
                 "(github.com/Freakler/CheatDeviceRemastered) ready: L + UP opens the menu ("
              << missing << " locations not found; " << patches.size()
              << " code patches of the PSP plugin are not applied in this port)\n"
              << std::flush;
    g_running = true;
    return true;
}

bool cheat_device_running() noexcept { return g_running; }

void cheat_device_frame(psprecomp::Runtime &, psprecomp::AllegrexContext &context, std::uint32_t buttons,
                        std::uint8_t lx, std::uint8_t ly, std::uint8_t rx, std::uint8_t ry) {
    if (!g_running) return;
    cdr_set_call_context(&context);
    cdr_scratch_reset();
    cdr_set_pad(buttons, lx, ly, rx, ry);

    // What the hooked limiter call ran: values, checks and the cheats.
    (void)sceKernelGetSystemTimeWidePatched();
    // The hooked pad routine.
    buttonInput();
    // The hooked world render.
    if (gametimer >= menuopendelay) draw();
    static bool language_set = false;
    if (!language_set && gametimer >= 1000) {
        langTableSetup(currLanguageID);
        language_set = true;
    }
    cdr_set_call_context(nullptr);
    publish_overlay();
}

std::uint32_t cheat_device_blocked_buttons() noexcept {
    if (!g_running || flag_keys_disable != 1) return 0u;
    // The ones the plugin cleared in the game's pad routine (not START or
    // the analog stick).
    return PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT |
           PSP_CTRL_RIGHT | PSP_CTRL_SELECT | PSP_CTRL_SQUARE | PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS |
           PSP_CTRL_CIRCLE;
}

bool cheat_device_overlay(std::vector<std::uint8_t> &rgba, std::uint64_t &version) {
    std::lock_guard lock(g_overlay_mutex);
    if (!g_overlay_visible) {
        version = g_overlay_version;
        return false;
    }
    if (version != g_overlay_version) {
        rgba = g_overlay;
        version = g_overlay_version;
    }
    return true;
}

bool cheat_device_exit_requested() noexcept { return g_exit_requested.load(); }

}  // namespace lcs
