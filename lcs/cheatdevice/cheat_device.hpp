/*
 *  Cheat Device Remastered in LCSNative: what the rest of the port calls.
 *
 *  Cheat Device Remastered is by Freakler
 *  (https://github.com/Freakler/CheatDeviceRemastered), converted with his
 *  permission. It needs his release: the player copies its
 *  cheatdevice_remastered folder next to the game (see README.md here).
 *
 *  Copyright (C) 2026, LCSNative contributors
 *  GPL-3.0-or-later, see LICENSE in this folder.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

namespace lcs {

// Starts the cheat device for the game just loaded: checks for the release,
// scans the game's code the way the plugin did and loads its config.
// `base_folder` holds the port's files (the release goes in
// base_folder/cheatdevice_remastered). False (with a log line saying why)
// when it does not start.
bool cheat_device_start(psprecomp::Runtime &runtime, const std::filesystem::path &base_folder,
                        std::uint32_t text_address, std::uint32_t text_size, std::uint32_t data_size);

[[nodiscard]] bool cheat_device_running() noexcept;

// Once per frame on the game thread (at the vblank wait), with the player's
// pad (PSP button bits, analog sticks 0-255): runs the cheats, the menu's
// input and builds its picture.
void cheat_device_frame(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context,
                        std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly, std::uint8_t rx,
                        std::uint8_t ry);

// The buttons the game must not see while the menu has the pad (the plugin
// blocked them in the game's own pad routine).
[[nodiscard]] std::uint32_t cheat_device_blocked_buttons() noexcept;

// The menu and its displays as a picture over the frame (cheat device size,
// RGBA, straight alpha). False when there is nothing to draw. `version`
// changes with the picture; when it equals the caller's, `rgba` is left as is.
inline constexpr std::uint32_t kCheatDeviceOverlayWidth = 960u;
inline constexpr std::uint32_t kCheatDeviceOverlayHeight = 544u;
bool cheat_device_overlay(std::vector<std::uint8_t> &rgba, std::uint64_t &version);

// "Exit Game" in the menu.
[[nodiscard]] bool cheat_device_exit_requested() noexcept;

}  // namespace lcs
