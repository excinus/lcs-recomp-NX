#pragma once

// The PSP's save list (sceUtilitySavedata LISTLOAD / LISTSAVE).
//
// On a PSP the system draws a list of the game's save slots and the player
// picks one; the game only names the slots it may use. Here the host draws
// that list over the game's frame (through the settings panel's overlay) and
// hands the choice back to the savedata utility in lcs_profile.cpp.
//
// The list also offers saves made by the game's other releases (EU, JP...):
// their folders carry another disc ID but the same slot names.

#include "lcs_display_menu.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lcs {

struct SaveSlot {
    std::string slot;      // the save name inside the game's ID ("S0")
    std::string game_id;   // the ID the folder starts with ("ULUS10041")
    std::filesystem::path directory;
    bool exists{false};
    bool foreign{false};    // another release's save
    bool encrypted{false};  // DATA.BIN as a PSP stores it (decrypted on load with the game's key)
    std::string title;      // PARAM.SFO SAVEDATA_TITLE
    std::string detail;     // PARAM.SFO SAVEDATA_DETAIL, first line
    std::vector<std::string> detail_lines;  // all of it
    std::string game_title; // PARAM.SFO TITLE
    std::string when;       // "2026-11-04 18:02"
    std::int64_t modified{0};
    // ICON0.PNG as RGBA (the caller decodes it; empty when there is none).
    std::vector<std::uint8_t> icon;
    std::uint32_t icon_width{0};
    std::uint32_t icon_height{0};
};

enum class SavePickerMode { Load, Save };
enum class SavePickerOutcome { Pending, Chosen, Cancelled };

// The list is offered on the Switch, and elsewhere when LCS_SAVE_PICKER=1.
// Without it, the utility uses the slot the game proposes, as before.
[[nodiscard]] bool save_picker_enabled() noexcept;

// PSP savedata focus values (SceUtilitySavedataParam.focus).
inline constexpr std::uint32_t kSaveFocusLatest = 3u;
inline constexpr std::uint32_t kSaveFocusOldest = 4u;
inline constexpr std::uint32_t kSaveFocusFirstEmpty = 6u;
inline constexpr std::uint32_t kSaveFocusLastEmpty = 7u;
inline constexpr std::uint32_t kSaveFocusLastList = 2u;

// Folders under `root` (PSP/SAVEDATA) for `game_id` and the slot `names`.
// Load lists the slots that exist, then other releases' saves of the same
// slots; Save lists every slot of this release, used or not. An empty `names`
// means every folder that starts with `game_id`.
[[nodiscard]] std::vector<SaveSlot> scan_save_slots(const std::filesystem::path &root,
                                                    const std::string &game_id,
                                                    const std::vector<std::string> &names,
                                                    SavePickerMode mode);

// The entry the list starts on for a PSP focus value.
[[nodiscard]] int save_focus_index(const std::vector<SaveSlot> &slots, std::uint32_t focus) noexcept;

// Shows the list. The game thread then polls save_picker_take() each update.
void save_picker_begin(SavePickerMode mode, std::vector<SaveSlot> slots, int focus);
[[nodiscard]] bool save_picker_active() noexcept;
// Pending until the player answers; then the outcome once, and the list closes.
[[nodiscard]] SavePickerOutcome save_picker_take(SaveSlot &chosen);
// Drops the list without an answer (the game shut the utility down).
void save_picker_abort() noexcept;

bool save_picker_handle(HostSettingsKey key) noexcept;
// The list as text rows, for presenters that only draw the text panel.
[[nodiscard]] HostSettingsView save_picker_view() noexcept;

// The list as the PSP drew it: a full-screen RGBA picture (kSavePickerImage
// size) with the saves' icons, the selected save's details and the button
// hints. False when the list is not up. `version` changes whenever the
// picture does; when it equals the caller's, `rgba` is left alone.
inline constexpr std::uint32_t kSavePickerImageWidth = 960u;
inline constexpr std::uint32_t kSavePickerImageHeight = 544u;
bool save_picker_image(std::vector<std::uint8_t> &rgba, std::uint64_t &version);

// PARAM.SFO of a save: written next to DATA.BIN so the list (and a PSP
// emulator) can name the save.
struct SaveSfo {
    std::string title;           // TITLE: the game's name
    std::string savedata_title;  // SAVEDATA_TITLE
    std::string detail;          // SAVEDATA_DETAIL
    std::string directory;       // SAVEDATA_DIRECTORY
    std::uint32_t parental_level{0};
    std::optional<std::uint8_t> crypt_mode;  // SAVEDATA_PARAMS[0] when present
};

[[nodiscard]] std::vector<std::uint8_t> build_save_sfo(const SaveSfo &sfo);
[[nodiscard]] std::optional<SaveSfo> parse_save_sfo(const std::vector<std::uint8_t> &bytes);

}  // namespace lcs
