/*
 *  Cheat Device Remastered for LCSNative: what the platform layer and the
 *  host glue share.
 *
 *  Copyright (C) 2026, LCSNative contributors
 *  GPL-3.0-or-later, see LICENSE in this folder.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

struct CdrCodePatch {
    const char *kind;
    std::uint32_t address;
    const char *target;
};

void cdr_bind_runtime(psprecomp::Runtime *runtime) noexcept;
void cdr_set_call_context(psprecomp::AllegrexContext *context) noexcept;
void cdr_set_folders(const std::string &plugin_folder, const std::string &card_root);
const std::string &cdr_plugin_folder() noexcept;
void cdr_set_pad(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly, std::uint8_t rx, std::uint8_t ry) noexcept;
std::vector<CdrCodePatch> cdr_code_patches();
void cdr_request_exit() noexcept;

// The overlay the converted drawString / drawBox feed (cdr_host.cpp).
void cdr_overlay_text(const char *text, int origin, short style, float scale, short shadow, float x, float y,
                      std::uint32_t color);
void cdr_overlay_box(float x, float y, float width, float height, std::uint32_t color);
