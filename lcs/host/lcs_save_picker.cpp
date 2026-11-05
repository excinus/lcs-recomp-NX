#include "lcs_save_picker.hpp"

#include "lcs_render_config.hpp"
#include "lcs_savedata_crypto.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace lcs {
namespace {

// Disc IDs of the game's releases. The game ID in a save folder's name is
// the disc ID of the release that made it.
constexpr const char *kLcsDiscIds[] = {
    "ULUS10041",  // North America
    "ULES00151",  // Europe
    "ULES00182",  // Europe (German)
    "ULJM05255",  // Japan
    "ULJM05297",  // Japan
    "NPJH50404",  // Japan (PSN)
};

const char *region_of(const std::string &game_id) {
    if (game_id.size() < 4u) return "OTHER RELEASE";
    if (game_id[2] == 'U' && game_id[3] == 'S') return "US";
    if (game_id[2] == 'E' && game_id[3] == 'S') return "EU";
    if (game_id[2] == 'J' && (game_id[3] == 'M' || game_id[3] == 'S')) return "JP";
    if (game_id[0] == 'N' && game_id[1] == 'P' && game_id[2] == 'J') return "JP";
    if (game_id[2] == 'K' && game_id[3] == 'S') return "KR";
    if (game_id[2] == 'A' && game_id[3] == 'S') return "ASIA";
    return "OTHER RELEASE";
}

bool looks_like_disc_id(const std::string &text) {
    if (text.size() < 9u) return false;
    for (std::size_t index = 0; index < 4u; ++index)
        if (!std::isupper(static_cast<unsigned char>(text[index]))) return false;
    for (std::size_t index = 4; index < 9u; ++index)
        if (!std::isdigit(static_cast<unsigned char>(text[index]))) return false;
    return true;
}

std::string upper(std::string text) {
    for (char &ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path, std::size_t limit) {
    std::vector<std::uint8_t> bytes;
    std::ifstream input(path, std::ios::binary);
    if (!input) return bytes;
    bytes.resize(limit);
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(limit));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(input.gcount(), 0)));
    return bytes;
}

std::string first_line(const std::string &text) {
    std::string line = text.substr(0, text.find_first_of("\r\n"));
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line;
}

// The 5x7 panel font has ASCII letters, digits and a little punctuation.
std::string printable(const std::string &text) {
    std::string out;
    for (unsigned char ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        if (ch < 0x20u || ch >= 0x7Fu) continue;
        if (!out.empty() && out.back() == ' ' && ch == ' ') continue;
        out.push_back(static_cast<char>(ch));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string slot_label(const std::string &slot) {
    // The game names its slots S0, S1...; the PSP showed them as entries 1, 2...
    if (slot.size() >= 2u && (slot[0] == 'S' || slot[0] == 's')) {
        bool digits = true;
        for (std::size_t index = 1; index < slot.size(); ++index)
            if (!std::isdigit(static_cast<unsigned char>(slot[index]))) digits = false;
        if (digits) return "SLOT " + std::to_string(std::atoi(slot.c_str() + 1) + 1);
    }
    return slot.empty() ? std::string("SAVE") : upper(slot);
}

void fill_details(SaveSlot &slot) {
    const std::filesystem::path data = slot.directory / "DATA.BIN";
    struct stat info {};
    if (::stat(data.string().c_str(), &info) == 0) {
        slot.exists = true;
        slot.modified = static_cast<std::int64_t>(info.st_mtime);
        const std::time_t time = static_cast<std::time_t>(info.st_mtime);
        if (const std::tm *local = std::localtime(&time)) {
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", local);
            slot.when = text;
        }
    }
    if (!slot.exists) return;
    if (const auto sfo = parse_save_sfo(read_file(slot.directory / "PARAM.SFO", 64u * 1024u))) {
        slot.title = printable(sfo->savedata_title.empty() ? sfo->title : sfo->savedata_title);
        slot.detail = printable(first_line(sfo->detail));
    }
    // The file decides, not PARAM.SFO: a save decrypted with a PC tool keeps
    // the PSP's PARAM.SFO, which still says encrypted.
    slot.encrypted = savedata_looks_encrypted(read_file(data, 16u * 1024u));
}

bool is_lcs_release(const std::filesystem::path &directory, const std::string &game_id) {
    for (const char *id : kLcsDiscIds)
        if (game_id == id) return true;
    // An ID this list does not know: trust the save's own PARAM.SFO.
    if (const auto sfo = parse_save_sfo(read_file(directory / "PARAM.SFO", 64u * 1024u))) {
        const std::string title = upper(sfo->title + " " + sfo->savedata_title);
        return title.find("LIBERTY CITY") != std::string::npos;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The list's state. The game thread opens it and polls for the answer; the
// window thread moves the selection and draws it.

enum class Stage { Closed, List, Confirm, Message, Answered };

struct PickerState {
    Stage stage{Stage::Closed};
    SavePickerMode mode{SavePickerMode::Load};
    std::vector<SaveSlot> slots;
    int selected{0};
    int top{0};
    char question[48]{};
    SavePickerOutcome outcome{SavePickerOutcome::Pending};
    SaveSlot chosen;
};

std::mutex g_mutex;
PickerState g_state;

constexpr int kVisibleRows = 6;

void answer(PickerState &state, SavePickerOutcome outcome) {
    state.outcome = outcome;
    if (outcome == SavePickerOutcome::Chosen) state.chosen = state.slots[static_cast<std::size_t>(state.selected)];
    state.stage = Stage::Answered;
}

void choose(PickerState &state) {
    if (state.slots.empty()) {
        answer(state, SavePickerOutcome::Cancelled);
        return;
    }
    const SaveSlot &slot = state.slots[static_cast<std::size_t>(state.selected)];
    if (state.mode == SavePickerMode::Load) {
        if (slot.foreign) {
            std::snprintf(state.question, sizeof(state.question), "%s SAVE: MAY NOT WORK. LOAD?",
                          region_of(slot.game_id));
            state.stage = Stage::Confirm;
            return;
        }
        answer(state, SavePickerOutcome::Chosen);
        return;
    }
    if (slot.exists) {
        std::snprintf(state.question, sizeof(state.question), "OVERWRITE %s?", slot_label(slot.slot).c_str());
        state.stage = Stage::Confirm;
        return;
    }
    answer(state, SavePickerOutcome::Chosen);
}

void keep_visible(PickerState &state) {
    if (state.selected < state.top) state.top = state.selected;
    if (state.selected >= state.top + kVisibleRows) state.top = state.selected - kVisibleRows + 1;
    state.top = std::clamp(state.top, 0, std::max(0, static_cast<int>(state.slots.size()) - kVisibleRows));
}

void copy_text(char *destination, std::size_t size, const std::string &text) {
    std::snprintf(destination, size, "%s", text.c_str());
}

}  // namespace

bool save_picker_enabled() noexcept {
#if defined(__SWITCH__)
    return true;
#else
    const char *value = std::getenv("LCS_SAVE_PICKER");
    return value != nullptr && value[0] == '1';
#endif
}

std::vector<SaveSlot> scan_save_slots(const std::filesystem::path &root, const std::string &game_id,
                                      const std::vector<std::string> &names, SavePickerMode mode) {
    std::vector<SaveSlot> own;
    std::vector<SaveSlot> foreign;
    std::error_code error;
    std::set<std::string> wanted(names.begin(), names.end());
    std::vector<std::string> order = names;
    if (names.empty() && std::filesystem::is_directory(root, error)) {
        for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(error) && name.size() > game_id.size() && name.rfind(game_id, 0) == 0)
                order.push_back(name.substr(game_id.size()));
        }
        std::sort(order.begin(), order.end());
        wanted.insert(order.begin(), order.end());
    }
    for (const std::string &name : order) {
        SaveSlot slot;
        slot.slot = name;
        slot.game_id = game_id;
        slot.directory = root / (game_id + name);
        fill_details(slot);
        if (slot.exists || mode == SavePickerMode::Save) own.push_back(std::move(slot));
    }
    if (mode == SavePickerMode::Load && std::filesystem::is_directory(root, error)) {
        for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
            if (!entry.is_directory(error)) continue;
            const std::string folder = entry.path().filename().string();
            if (!looks_like_disc_id(folder)) continue;
            const std::string id = folder.substr(0, 9);
            if (id == game_id) continue;
            const std::string name = folder.substr(9);
            if (name.empty() || (!wanted.empty() && !wanted.contains(name))) continue;
            if (!is_lcs_release(entry.path(), id)) continue;
            SaveSlot slot;
            slot.slot = name;
            slot.game_id = id;
            slot.directory = entry.path();
            slot.foreign = true;
            fill_details(slot);
            if (slot.exists) foreign.push_back(std::move(slot));
        }
        std::sort(foreign.begin(), foreign.end(), [](const SaveSlot &a, const SaveSlot &b) {
            return a.game_id != b.game_id ? a.game_id < b.game_id : a.slot < b.slot;
        });
    }
    for (SaveSlot &slot : foreign) own.push_back(std::move(slot));
    return own;
}

int save_focus_index(const std::vector<SaveSlot> &slots, std::uint32_t focus) noexcept {
    if (slots.empty()) return 0;
    const int count = static_cast<int>(slots.size());
    int best = 0;
    switch (focus) {
    case kSaveFocusLatest:
    case kSaveFocusOldest: {
        bool found = false;
        for (int index = 0; index < count; ++index) {
            const SaveSlot &slot = slots[static_cast<std::size_t>(index)];
            if (!slot.exists) continue;
            const SaveSlot &current = slots[static_cast<std::size_t>(best)];
            const bool better = focus == kSaveFocusLatest ? slot.modified > current.modified
                                                          : slot.modified < current.modified;
            if (!found || better) best = index;
            found = true;
        }
        return best;
    }
    case kSaveFocusFirstEmpty:
        for (int index = 0; index < count; ++index)
            if (!slots[static_cast<std::size_t>(index)].exists) return index;
        return 0;
    case kSaveFocusLastEmpty:
        for (int index = count - 1; index >= 0; --index)
            if (!slots[static_cast<std::size_t>(index)].exists) return index;
        return count - 1;
    case kSaveFocusLastList:
        return count - 1;
    default:
        return 0;
    }
}

void save_picker_begin(SavePickerMode mode, std::vector<SaveSlot> slots, int focus) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_state = PickerState{};
    g_state.mode = mode;
    g_state.slots = std::move(slots);
    g_state.selected = std::clamp(focus, 0, std::max(0, static_cast<int>(g_state.slots.size()) - 1));
    g_state.stage = Stage::List;
    if (g_state.slots.empty()) {
        std::snprintf(g_state.question, sizeof(g_state.question), "THERE IS NO SAVED DATA");
        g_state.stage = Stage::Message;
    }
    keep_visible(g_state);
}

bool save_picker_active() noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_state.stage != Stage::Closed && g_state.stage != Stage::Answered;
}

SavePickerOutcome save_picker_take(SaveSlot &chosen) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_state.stage != Stage::Answered) {
        // Closed without an answer (should not happen): treat as cancelled.
        return g_state.stage == Stage::Closed ? SavePickerOutcome::Cancelled : SavePickerOutcome::Pending;
    }
    const SavePickerOutcome outcome = g_state.outcome;
    chosen = g_state.chosen;
    g_state = PickerState{};
    return outcome;
}

void save_picker_abort() noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_state = PickerState{};
}

bool save_picker_handle(HostSettingsKey key) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    PickerState &state = g_state;
    switch (state.stage) {
    case Stage::Closed:
    case Stage::Answered:
        return false;
    case Stage::Message:
        if (key == HostSettingsKey::Confirm || key == HostSettingsKey::Close) {
            if (state.slots.empty()) answer(state, SavePickerOutcome::Cancelled);
            else state.stage = Stage::List;
        }
        return true;
    case Stage::Confirm:
        if (key == HostSettingsKey::Confirm) answer(state, SavePickerOutcome::Chosen);
        else if (key == HostSettingsKey::Close) state.stage = Stage::List;
        return true;
    case Stage::List:
        break;
    }
    const int count = static_cast<int>(state.slots.size());
    if (key == HostSettingsKey::Up && count > 0) state.selected = (state.selected + count - 1) % count;
    else if (key == HostSettingsKey::Down && count > 0) state.selected = (state.selected + 1) % count;
    else if (key == HostSettingsKey::Left && count > 0) state.selected = std::max(0, state.selected - kVisibleRows);
    else if (key == HostSettingsKey::Right && count > 0) state.selected = std::min(count - 1, state.selected + kVisibleRows);
    else if (key == HostSettingsKey::Confirm) choose(state);
    else if (key == HostSettingsKey::Close) answer(state, SavePickerOutcome::Cancelled);
    keep_visible(state);
    return true;
}

HostSettingsView save_picker_view() noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    const PickerState &state = g_state;
    HostSettingsView view{};
    view.centered = true;
    view.title = state.mode == SavePickerMode::Load ? "LOAD GAME" : "SAVE GAME";
#if defined(__SWITCH__)
    const bool nintendo = lcs_render_configuration().nx.nintendo_confirm;
    const char *yes = nintendo ? "A" : "B";
    const char *no = nintendo ? "B" : "A";
#else
    const char *yes = "ENTER";
    const char *no = "ESC";
#endif
    static thread_local char hint[48];
    if (state.stage == Stage::Confirm)
        std::snprintf(hint, sizeof(hint), "%s YES   %s NO", yes, no);
    else if (state.stage == Stage::Message)
        std::snprintf(hint, sizeof(hint), "%s OK", yes);
    else
        std::snprintf(hint, sizeof(hint), "%s %s   %s CANCEL", yes,
                      state.mode == SavePickerMode::Load ? "LOAD" : "SAVE", no);
    view.hint = hint;

    const int count = static_cast<int>(state.slots.size());
    const int shown = std::min(kVisibleRows, count - state.top);
    view.row_count = std::max(0, shown);
    view.selected = state.selected - state.top;
    for (int row = 0; row < view.row_count; ++row) {
        const SaveSlot &slot = state.slots[static_cast<std::size_t>(state.top + row)];
        std::string text = slot_label(slot.slot);
        if (slot.foreign) text += " " + std::string(region_of(slot.game_id));
        text.resize(std::max<std::size_t>(text.size(), 11u), ' ');
        if (!slot.exists) text += "NEW SAVE";
        else if (!slot.detail.empty()) text += slot.detail;
        else if (!slot.title.empty()) text += slot.title;
        else text += slot.when;
        copy_text(view.rows[row], sizeof(view.rows[row]), text);
    }
    if (state.stage == Stage::Confirm || state.stage == Stage::Message) {
        copy_text(view.footer[0], sizeof(view.footer[0]), state.question);
    } else if (count > 0) {
        const SaveSlot &slot = state.slots[static_cast<std::size_t>(state.selected)];
        std::string first = slot.exists ? (slot.title.empty() ? slot.detail : slot.title) : std::string();
        if (slot.foreign) first = std::string(slot.game_id) + (first.empty() ? "" : "  " + first);
        copy_text(view.footer[0], sizeof(view.footer[0]), first);
        if (slot.exists) copy_text(view.footer[1], sizeof(view.footer[1]), slot.when);
        if (count > kVisibleRows) {
            char position[48];
            std::snprintf(position, sizeof(position), "%s%s%d/%d", slot.when.c_str(),
                          slot.when.empty() ? "" : "   ", state.selected + 1, count);
            copy_text(view.footer[1], sizeof(view.footer[1]), position);
        }
    }
    return view;
}

// ---------------------------------------------------------------------------
// PARAM.SFO: "\0PSF", version 1.1, then sorted key entries.

std::vector<std::uint8_t> build_save_sfo(const SaveSfo &sfo) {
    struct Entry {
        std::string key;
        std::uint16_t format;  // 0x0204 UTF-8 text, 0x0404 integer
        std::vector<std::uint8_t> data;
        std::uint32_t max_size;
    };
    const auto text = [](const std::string &value, std::uint32_t max_size) {
        std::vector<std::uint8_t> data(value.begin(), value.end());
        if (data.size() + 1u > max_size) data.resize(max_size - 1u);
        data.push_back(0u);
        return data;
    };
    std::vector<Entry> entries{
        {"CATEGORY", 0x0204u, text("MS", 4u), 4u},
        {"PARENTAL_LEVEL", 0x0404u, {}, 4u},
        {"SAVEDATA_DETAIL", 0x0204u, text(sfo.detail, 1024u), 1024u},
        {"SAVEDATA_DIRECTORY", 0x0204u, text(sfo.directory, 64u), 64u},
        {"SAVEDATA_PARAMS", 0x0004u, std::vector<std::uint8_t>(128u, 0u), 128u},
        {"SAVEDATA_TITLE", 0x0204u, text(sfo.savedata_title, 128u), 128u},
        {"TITLE", 0x0204u, text(sfo.title, 128u), 128u},
    };
    for (Entry &entry : entries) {
        if (entry.key != "PARENTAL_LEVEL") continue;
        const std::uint32_t level = sfo.parental_level;
        entry.data = {static_cast<std::uint8_t>(level), static_cast<std::uint8_t>(level >> 8u),
                      static_cast<std::uint8_t>(level >> 16u), static_cast<std::uint8_t>(level >> 24u)};
    }
    std::vector<std::uint8_t> keys;
    std::vector<std::uint16_t> key_offsets;
    for (const Entry &entry : entries) {
        key_offsets.push_back(static_cast<std::uint16_t>(keys.size()));
        keys.insert(keys.end(), entry.key.begin(), entry.key.end());
        keys.push_back(0u);
    }
    while (keys.size() % 4u != 0u) keys.push_back(0u);
    const std::uint32_t key_start = 20u + static_cast<std::uint32_t>(entries.size()) * 16u;
    const std::uint32_t data_start = key_start + static_cast<std::uint32_t>(keys.size());
    std::vector<std::uint8_t> out(data_start, 0u);
    const auto put16 = [&](std::size_t at, std::uint32_t value) {
        out[at] = static_cast<std::uint8_t>(value);
        out[at + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t index = 0; index < 4u; ++index)
            out[at + index] = static_cast<std::uint8_t>(value >> (8u * index));
    };
    out[0] = 0u; out[1] = 'P'; out[2] = 'S'; out[3] = 'F';
    put32(4u, 0x00000101u);
    put32(8u, key_start);
    put32(12u, data_start);
    put32(16u, static_cast<std::uint32_t>(entries.size()));
    std::copy(keys.begin(), keys.end(), out.begin() + key_start);
    std::uint32_t data_offset = 0u;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const Entry &entry = entries[index];
        const std::size_t at = 20u + index * 16u;
        put16(at, key_offsets[index]);
        put16(at + 2u, entry.format);
        put32(at + 4u, static_cast<std::uint32_t>(entry.data.size()));
        put32(at + 8u, entry.max_size);
        put32(at + 12u, data_offset);
        std::vector<std::uint8_t> padded = entry.data;
        padded.resize(entry.max_size, 0u);
        out.insert(out.end(), padded.begin(), padded.end());
        data_offset += entry.max_size;
    }
    return out;
}

std::optional<SaveSfo> parse_save_sfo(const std::vector<std::uint8_t> &bytes) {
    if (bytes.size() < 20u || bytes[0] != 0u || bytes[1] != 'P' || bytes[2] != 'S' || bytes[3] != 'F')
        return std::nullopt;
    const auto get16 = [&](std::size_t at) -> std::uint32_t {
        return at + 2u <= bytes.size() ? bytes[at] | (bytes[at + 1u] << 8u) : 0u;
    };
    const auto get32 = [&](std::size_t at) -> std::uint32_t {
        if (at + 4u > bytes.size()) return 0u;
        return bytes[at] | (bytes[at + 1u] << 8u) | (bytes[at + 2u] << 16u) |
               (static_cast<std::uint32_t>(bytes[at + 3u]) << 24u);
    };
    const std::uint32_t key_start = get32(8u);
    const std::uint32_t data_start = get32(12u);
    const std::uint32_t count = get32(16u);
    if (count > 256u || 20u + static_cast<std::size_t>(count) * 16u > bytes.size()) return std::nullopt;
    SaveSfo sfo;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t at = 20u + static_cast<std::size_t>(index) * 16u;
        const std::size_t key_at = static_cast<std::size_t>(key_start) + get16(at);
        const std::uint32_t format = get16(at + 2u);
        const std::uint32_t length = get32(at + 4u);
        const std::size_t data_at = static_cast<std::size_t>(data_start) + get32(at + 12u);
        if (key_at >= bytes.size() || data_at > bytes.size() || length > bytes.size() - data_at) continue;
        std::string key;
        for (std::size_t cursor = key_at; cursor < bytes.size() && bytes[cursor] != 0u; ++cursor)
            key.push_back(static_cast<char>(bytes[cursor]));
        std::string value;
        if (format == 0x0204u || format == 0x0004u) {
            for (std::size_t cursor = data_at; cursor < data_at + length && bytes[cursor] != 0u; ++cursor)
                value.push_back(static_cast<char>(bytes[cursor]));
        }
        if (key == "TITLE") sfo.title = value;
        else if (key == "SAVEDATA_TITLE") sfo.savedata_title = value;
        else if (key == "SAVEDATA_DETAIL") sfo.detail = value;
        else if (key == "SAVEDATA_DIRECTORY") sfo.directory = value;
        else if (key == "PARENTAL_LEVEL" && format == 0x0404u) sfo.parental_level = get32(data_at);
        else if (key == "SAVEDATA_PARAMS" && length > 0u) sfo.crypt_mode = bytes[data_at];
    }
    return sfo;
}

}  // namespace lcs
