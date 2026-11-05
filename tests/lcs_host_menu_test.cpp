#include "lcs_display_menu.hpp"
#include "lcs_key_bindings.hpp"
#include "lcs_render_config.hpp"
#include "lcs_save_picker.hpp"
#include "lcs_savedata_crypto.hpp"

#include <cstdint>
#include <cstdlib>
#include <random>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace lcs {
void display_window_request_fullscreen(bool) noexcept {}
}

namespace {

int fail(const char *message) {
    std::cerr << "FAIL " << message << '\n';
    return 1;
}

int menu_keys() {
    using namespace lcs;
    if (host_settings_open()) return fail("menu started open");
    if (!host_settings_handle(HostSettingsKey::Toggle) || !host_settings_open())
        return fail("toggle did not open");
    const HostSettingsView opened = host_settings_view();
    if (opened.selected != 0) return fail("initial selection");
    const char *labels[]{"View distance", "Fullscreen", "Resolution", "FPS counter", "Frame rate"};
    for (const char *label : labels) {
        bool found = false;
        for (const char (&row)[48] : opened.rows)
            if (std::string(row).find(label) != std::string::npos) found = true;
        if (!found) return fail(label);
    }
    if (!host_settings_handle(HostSettingsKey::Up)) return fail("up");
    const int moved = host_settings_view().selected;
    if (moved == opened.selected) return fail("up did not move");
    if (!host_settings_handle(HostSettingsKey::Down)) return fail("down");
    if (host_settings_view().selected != opened.selected) return fail("down did not return");
    if (!host_settings_handle(HostSettingsKey::Close) || host_settings_open())
        return fail("close");
    std::cout << "open=1 moved=" << moved << " returned=0 closed=1 rows=5\n";
    return 0;
}

int present_policy() {
    using namespace lcs;
    if (!host_settings_handle(HostSettingsKey::Toggle)) return fail("open");
    const Dx12FramePresentDecision open = decide_dx12_frame_present(true);
    if (!open.present_directly) return fail("menu did not present directly");
    if (open.map_full_frame_readback) return fail("menu mapped a full-frame readback");
    if (!open.composite_settings) return fail("menu was not composited");
    if (open.composite_fps) return fail("menu turned the fps counter on");
    if (std::string(open.settings.rows[0]).find("View distance") == std::string::npos)
        return fail("missing view distance");
    if (std::string(open.settings.rows[1]).find("Fullscreen") == std::string::npos)
        return fail("missing fullscreen");
    if (std::string(open.settings.rows[2]).find("Resolution") == std::string::npos)
        return fail("missing resolution");
    if (std::string(open.settings.rows[3]).find("FPS counter") == std::string::npos)
        return fail("missing fps");
    if (std::string(open.settings.rows[4]).find("Frame rate") == std::string::npos)
        return fail("missing frame rate");
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kSettingsOverlayWidth) *
                                   kSettingsOverlayHeight * 4u);
    rasterize_settings_overlay(open.settings, rgba.data(), kSettingsOverlayWidth, kSettingsOverlayHeight);
    std::size_t text_pixels = 0u;
    for (std::size_t index = 0u; index + 3u < rgba.size(); index += 4u) {
        if (rgba[index + 3u] == 255u && rgba[index] > 80u) ++text_pixels;
    }
    if (text_pixels < 40u) return fail("settings view was not painted into the frame");
    if (!host_settings_handle(HostSettingsKey::Close)) return fail("close");
    const Dx12FramePresentDecision closed = decide_dx12_frame_present(true);
    if (!closed.present_directly || closed.map_full_frame_readback || closed.composite_settings)
        return fail("closing the menu disabled direct presentation");
    std::cout << "direct=1 readback=0 composite=1 text_pixels=" << text_pixels
              << " closed_direct=1 closed_readback=0 closed_composite=0\n";
    return 0;
}

int fps_counter() {
    using namespace lcs;
    lcs_set_show_fps(false);
    const Dx12FramePresentDecision hidden = decide_dx12_frame_present(true);
    if (!hidden.present_directly || hidden.map_full_frame_readback || hidden.composite_fps)
        return fail("hidden counter changed presentation");
    lcs_set_show_fps(true);
    const Dx12FramePresentDecision shown = decide_dx12_frame_present(true);
    if (!shown.present_directly || shown.map_full_frame_readback || !shown.composite_fps)
        return fail("counter did not composite on the direct present");
    if (shown.composite_settings) return fail("counter opened the menu");
    char label[24];
    host_fps_format(label, sizeof(label));
    if (std::string(label).find("FPS") == std::string::npos) return fail("fps label");
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kFpsOverlayWidth) *
                                   kFpsOverlayHeight * 4u);
    rasterize_fps_overlay(rgba.data(), kFpsOverlayWidth, kFpsOverlayHeight);
    std::size_t text_pixels = 0u;
    std::size_t badge_pixels = 0u;
    for (std::size_t index = 0u; index + 3u < rgba.size(); index += 4u) {
        if (rgba[index + 3u] == 255u && rgba[index] > 200u) ++text_pixels;
        if (rgba[index + 3u] == 210u) ++badge_pixels;
    }
    if (text_pixels < 40u) return fail("fps text was not painted");
    if (badge_pixels == 0u) return fail("fps badge was transparent");
    lcs_set_show_fps(false);
    const Dx12FramePresentDecision closed = decide_dx12_frame_present(true);
    if (!closed.present_directly || closed.map_full_frame_readback || closed.composite_fps)
        return fail("turning the counter off left it on the frame");
    std::cout << "hidden_direct=1 shown_direct=1 shown_readback=0 shown_composite=1"
                 " text_pixels=" << text_pixels << " closed_composite=0\n";
    return 0;
}

int bindings() {
    using namespace lcs;
    const ControlBindings defaults = default_control_bindings();
    if (!binding_has(defaults, BindAction::Sprint, HostKey::Space)) return fail("default sprint");
    if (!binding_has(defaults, BindAction::Aim, HostKey::MouseRight)) return fail("default aim");
    if (!binding_has(defaults, BindAction::Handbrake, HostKey::Space)) return fail("default handbrake");
    if (binding_wheel_buttons(defaults, true, false) != kPspLeft) return fail("wheel up");
    if (binding_wheel_buttons(defaults, false, true) != kPspRight) return fail("wheel down");

    const auto held = [](HostKey key) {
        return key == HostKey::W || key == HostKey::Space || key == HostKey::MouseRight ||
               key == HostKey::LeftAlt || key == HostKey::Up || key == HostKey::A;
    };
    const KeyboardSample foot = sample_keyboard(defaults, false, held);
    if (foot.move_x != -1 || foot.move_y != -1 || !foot.accelerate || foot.brake || foot.reach != 60)
        return fail("foot axes");
    if ((foot.buttons & kPspCross) == 0 || (foot.buttons & kPspRTrigger) == 0 ||
        (foot.buttons & kPspUp) == 0)
        return fail("foot buttons");

    const KeyboardSample car = sample_keyboard(defaults, true, held);
    if (car.move_x != -1 || car.move_y != -1 || !car.accelerate || car.brake)
        return fail("car axes");
    if ((car.buttons & kPspCross) != 0 || (car.buttons & kPspRTrigger) == 0)
        return fail("car buttons");

    const auto aim_only = [](HostKey key) { return key == HostKey::MouseRight; };
    if ((sample_keyboard(defaults, true, aim_only).buttons & kPspRTrigger) != 0)
        return fail("aim suppressed in a vehicle");

    ControlBindings edited = defaults;
    std::string warning;
    if (!apply_control_binding(edited, "Jump", "Shift", warning) || !warning.empty() ||
        !binding_has(edited, BindAction::Jump, HostKey::LeftShift) ||
        !binding_has(edited, BindAction::Jump, HostKey::RightShift))
        return fail("shift alias");
    if (!apply_control_binding(edited, "Aim", "F10", warning) ||
        warning.find("unchanged") == std::string::npos ||
        !binding_has(edited, BindAction::Aim, HostKey::MouseRight))
        return fail("reserved key");
    if (!apply_control_binding(edited, "Sprint", "None", warning) || !warning.empty() ||
        edited.count[bind_index(BindAction::Sprint)] != 0u)
        return fail("clear");

    const auto path = std::filesystem::temp_directory_path() / "lcs-key-bindings-test.ini";
    {
        std::ofstream out(path);
        out << "[Controls]\n"
               "MouseSensitivity=40\n"
               "Sprint=E\n"
               "WeaponPrevious=Q\n"
               "Attack=MouseMiddle, Nope\n"
               "Handbrake=MouseRight\n";
    }
    const LcsConfiguration loaded = load_lcs_render_configuration(path);
    std::filesystem::remove(path);
    if (loaded.controls.mouse_sensitivity != 40u) return fail("sensitivity");
    if (!binding_has(loaded.controls.bindings, BindAction::Sprint, HostKey::E) ||
        loaded.controls.bindings.count[bind_index(BindAction::Sprint)] != 1u)
        return fail("loaded sprint");
    if (binding_has(loaded.controls.bindings, BindAction::WeaponPrevious, HostKey::WheelUp))
        return fail("wheel left bound");
    if (!binding_has(loaded.controls.bindings, BindAction::Attack, HostKey::MouseMiddle) ||
        !binding_has(loaded.controls.bindings, BindAction::MoveForward, HostKey::W))
        return fail("partial rebind");
    if ((sample_keyboard(loaded.controls.bindings, true, aim_only).buttons & kPspRTrigger) == 0)
        return fail("handbrake rebound");
    bool saw_unknown = false;
    for (const std::string &message : loaded.warnings)
        if (message.find("nope") != std::string::npos) saw_unknown = true;
    if (!saw_unknown) return fail("missing warning");

    const auto shipped_path = std::filesystem::path(__FILE__).parent_path() / ".." / "lcs" /
                              "config" / "LCSNative.ini";
    const LcsConfiguration shipped = load_lcs_render_configuration(shipped_path);
    if (!shipped.warnings.empty()) {
        for (const std::string &message : shipped.warnings)
            std::cerr << message << '\n';
        return fail("shipped ini");
    }
    if (!binding_has(shipped.controls.bindings, BindAction::CenterCamera, HostKey::H) ||
        !binding_has(shipped.controls.bindings, BindAction::CenterCamera, HostKey::MouseMiddle))
        return fail("shipped center");

    const auto pad_a = [](PadButton button) { return button == PadButton::A; };
    const auto pad_rt = [](PadButton button) { return button == PadButton::RightTrigger; };
    const auto pad_rb = [](PadButton button) { return button == PadButton::RightShoulder; };
    const auto pad_lt = [](PadButton button) { return button == PadButton::LeftTrigger; };
    const auto pad_l3 = [](PadButton button) { return button == PadButton::LeftClick; };
    const PadSample pad_foot = sample_pad(defaults, false, pad_rt);
    if (!pad_foot.accelerate || (pad_foot.buttons & kPspRTrigger) == 0)
        return fail("trigger on foot");
    const PadSample pad_car = sample_pad(defaults, true, pad_rt);
    if (!pad_car.accelerate || (pad_car.buttons & kPspRTrigger) != 0)
        return fail("trigger in a vehicle");
    if ((sample_pad(defaults, true, pad_a).buttons & kPspCross) == 0)
        return fail("pad run in a vehicle");
    if ((sample_pad(defaults, true, pad_rb).buttons & kPspRTrigger) == 0)
        return fail("shoulder handbrake");
    if (!sample_pad(defaults, true, pad_lt).brake ||
        (sample_pad(defaults, true, pad_lt).buttons & kPspLTrigger) != 0)
        return fail("left trigger");
    if (!apply_pad_binding(edited, "Cross", "Attack", warning) || !warning.empty() ||
        (sample_pad(edited, false, pad_a).buttons & kPspCircle) == 0)
        return fail("pad rebind");
    if (!apply_stick_binding(edited, "LeftStick", "Camera", warning) || !warning.empty() ||
        edited.left_stick != StickRole::Camera)
        return fail("stick swap");
    const StickReading sdl_move = interpret_stick(StickRole::Move, 0, -20000, false, false);
    if (!sdl_move.move || sdl_move.move_y != stick_to_psp(-20000, false))
        return fail("sdl stick");
    const StickReading xinput_look = interpret_stick(StickRole::Camera, 0, 20000, true, false);
    if (!xinput_look.camera ||
        xinput_look.camera_y != static_cast<int>(stick_to_psp(20000, false)) - 128)
        return fail("xinput stick");
    if ((sample_pad(defaults, true, pad_l3).buttons & kPspDown) == 0)
        return fail("left stick horn");
    if (!pad_has(shipped.controls.bindings, PadButton::A, BindAction::Sprint) ||
        !pad_has(shipped.controls.bindings, PadButton::LeftClick, BindAction::Down) ||
        shipped.controls.bindings.left_stick != StickRole::Move ||
        shipped.controls.bindings.right_stick != StickRole::Camera ||
        shipped.controls.bindings.pad_count[static_cast<std::size_t>(PadButton::RightClick)] != 0u)
        return fail("shipped pad");
    std::cout << "defaults=1 rebound=1 shipped=1 pad=1\n";
    return 0;
}

int f10_key() {
    using namespace lcs;
    const Win32HostKeyDecision sys_down =
        classify_win32_host_key(kWin32SysKeyDown, kWin32VkF10, false);
    const Win32HostKeyDecision sys_up =
        classify_win32_host_key(kWin32SysKeyUp, kWin32VkF10, false);
    const Win32HostKeyDecision key_down =
        classify_win32_host_key(kWin32KeyDown, kWin32VkF10, false);
    const Win32HostKeyDecision key_up =
        classify_win32_host_key(kWin32KeyUp, kWin32VkF10, false);
    const Win32HostKeyDecision system_menu =
        classify_win32_host_key(kWin32SysCommand, kWin32ScKeyMenu, false);
    if (!sys_down.consume || !sys_down.toggle_settings) return fail("system F10 press");
    if (!sys_up.consume || sys_up.toggle_settings) return fail("system F10 release");
    if (!key_down.consume || !key_down.toggle_settings) return fail("F10 press");
    if (!key_up.consume || key_up.toggle_settings) return fail("F10 release");
    if (!system_menu.consume || system_menu.toggle_settings) return fail("system key menu");
    std::cout << "sys_down=consume+toggle sys_up=consume key_down=consume+toggle"
                 " key_up=consume syscommand=consume\n";
    return 0;
}

int find_row(const lcs::HostSettingsView &view, const char *label) {
    for (int row = 0; row < view.row_count; ++row)
        if (std::string(view.rows[row]).find(label) != std::string::npos) return row;
    return -1;
}

// The panel writes its changes back to the ini, and they are there on the
// next start.
int persist() {
    using namespace lcs;
    const auto path = std::filesystem::temp_directory_path() / "lcs-persist-test.ini";
    {
        std::ofstream out(path);
        out << "; comment kept\n[Display]\nShowFPS=false\n[Timing]\nFrameRate=30\n";
    }
#if defined(_WIN32)
    _putenv_s("PSPRECOMP_CONFIG", path.string().c_str());
#else
    setenv("PSPRECOMP_CONFIG", path.string().c_str(), 1);
#endif
    initialize_lcs_render_configuration(std::filesystem::temp_directory_path());
    if (lcs_render_configuration().display.show_fps) return fail("fps started on");
    if (!host_settings_handle(HostSettingsKey::Toggle)) return fail("open");
    const int row = find_row(host_settings_view(), "FPS counter");
    if (row < 0) return fail("fps row");
    while (host_settings_view().selected != row) host_settings_handle(HostSettingsKey::Down);
    if (!host_settings_handle(HostSettingsKey::Confirm)) return fail("confirm");
    if (!lcs_render_configuration().display.show_fps) return fail("confirm did not change the row");
    host_settings_handle(HostSettingsKey::Close);
    std::ifstream input(path);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.find("ShowFPS=true") == std::string::npos) return fail("ShowFPS not written");
    if (text.find("; comment kept") == std::string::npos) return fail("comment lost");
    initialize_lcs_render_configuration(std::filesystem::temp_directory_path());
    if (!lcs_render_configuration().display.show_fps) return fail("fps not on after a restart");
    std::filesystem::remove(path);
#if defined(_WIN32)
    _putenv_s("PSPRECOMP_CONFIG", "");
#else
    unsetenv("PSPRECOMP_CONFIG");
#endif
    std::cout << "written=1 reloaded=1\n";
    return 0;
}

int triggers() {
    using namespace lcs;
    ControlBindings bindings = default_control_bindings();
    const auto zr = [](PadButton button) { return button == PadButton::RightTrigger; };
    const auto zl = [](PadButton button) { return button == PadButton::LeftTrigger; };
    apply_switch_trigger_mode(bindings, SwitchTriggerMode::Dpad);
    if ((sample_pad(bindings, false, zr).buttons & kPspRight) == 0) return fail("zr weapon");
    if ((sample_pad(bindings, true, zl).buttons & kPspLeft) == 0) return fail("zl radio in a vehicle");
    if (sample_pad(bindings, true, zr).accelerate) return fail("zr accelerates in d-pad mode");
    apply_switch_trigger_mode(bindings, SwitchTriggerMode::Drive);
    if (!sample_pad(bindings, true, zr).accelerate || (sample_pad(bindings, true, zr).buttons & kPspRTrigger) != 0)
        return fail("drive zr");
    apply_switch_trigger_mode(bindings, SwitchTriggerMode::Shoulders);
    if ((sample_pad(bindings, false, zl).buttons & kPspLTrigger) == 0) return fail("shoulder zl");
    std::cout << "dpad=1 drive=1 shoulders=1\n";
    return 0;
}

// SAVEDATA_PARAMS[0] = 0x21, as a PSP writes it for a keyed save.
std::vector<std::uint8_t> mark_sfo_encrypted(std::vector<std::uint8_t> bytes) {
    const std::string key = "SAVEDATA_PARAMS";
    const std::uint32_t key_start = bytes[8] | (bytes[9] << 8) | (bytes[10] << 16) | (bytes[11] << 24);
    const std::uint32_t data_start = bytes[12] | (bytes[13] << 8) | (bytes[14] << 16) | (bytes[15] << 24);
    const std::uint32_t count = bytes[16];
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t at = 20u + index * 16u;
        const std::size_t name = key_start + (bytes[at] | (bytes[at + 1] << 8));
        if (std::string(reinterpret_cast<const char *>(&bytes[name])) != key) continue;
        const std::size_t data = data_start + (bytes[at + 12] | (bytes[at + 13] << 8));
        bytes[data] = 0x21u;
    }
    return bytes;
}

void write_bytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

int saves() {
    using namespace lcs;
    const auto root = std::filesystem::temp_directory_path() / "lcs-save-picker-test";
    std::filesystem::remove_all(root);
    const std::vector<std::uint8_t> plain(73752u, 0u);
    std::vector<std::uint8_t> noise(73752u);
    std::mt19937 random(7u);
    for (std::uint8_t &byte : noise) byte = static_cast<std::uint8_t>(random());
    SaveSfo sfo;
    sfo.title = "GTA: Liberty City Stories";
    sfo.savedata_title = "Liberty City Stories";
    sfo.detail = "The Sicilian Gambit\n34%";
    sfo.directory = "ULUS10041S0";
    const auto sfo_bytes = build_save_sfo(sfo);
    const auto parsed = parse_save_sfo(sfo_bytes);
    if (!parsed || parsed->detail != sfo.detail || parsed->title != sfo.title ||
        !parsed->crypt_mode || *parsed->crypt_mode != 0u)
        return fail("sfo round trip");
    write_bytes(root / "ULUS10041S0" / "DATA.BIN", plain);
    write_bytes(root / "ULUS10041S0" / "PARAM.SFO", sfo_bytes);
    write_bytes(root / "ULUS10041S2" / "DATA.BIN", plain);
    // A save decrypted on a PC keeps the PSP's PARAM.SFO, which says encrypted.
    write_bytes(root / "ULUS10041S2" / "PARAM.SFO", mark_sfo_encrypted(sfo_bytes));
    write_bytes(root / "ULES00151S1" / "DATA.BIN", plain);
    write_bytes(root / "ULES00151S3" / "DATA.BIN", noise);
    write_bytes(root / "ULUS10160S0" / "DATA.BIN", plain);  // another game: left out
    const std::vector<std::string> names{"S0", "S1", "S2", "S3"};

    const auto load = scan_save_slots(root, "ULUS10041", names, SavePickerMode::Load);
    if (load.size() != 4u) return fail("load list size");
    if (load[0].slot != "S0" || load[1].slot != "S2" || load[0].foreign || !load[2].foreign ||
        load[2].game_id != "ULES00151" || !load[3].encrypted || load[2].encrypted)
        return fail("load list order");
    if (load[0].detail != "The Sicilian Gambit") return fail("detail line");
    if (load[1].encrypted) return fail("plain data under an encrypted PARAM.SFO");
    if (const auto marked = parse_save_sfo(mark_sfo_encrypted(sfo_bytes));
        !marked || !marked->crypt_mode || *marked->crypt_mode != 0x21u)
        return fail("test SFO not marked");

    save_picker_begin(SavePickerMode::Load, load, 0);
    if (!save_picker_active() || !host_settings_open()) return fail("list not up");
    const HostSettingsView view = host_settings_view();
    if (!view.centered || view.row_count != 4 || std::string(view.rows[0]).find("SLOT 1") == std::string::npos)
        return fail("list view");
    SaveSlot chosen;
    if (save_picker_take(chosen) != SavePickerOutcome::Pending) return fail("answered early");
    host_settings_handle(HostSettingsKey::Down);
    host_settings_handle(HostSettingsKey::Confirm);
    if (save_picker_take(chosen) != SavePickerOutcome::Chosen || chosen.slot != "S2") return fail("pick S2");
    if (host_settings_open()) return fail("list stayed up");

    save_picker_begin(SavePickerMode::Load, load, 2);
    host_settings_handle(HostSettingsKey::Confirm);  // another release loads as any other
    if (save_picker_take(chosen) != SavePickerOutcome::Chosen || chosen.game_id != "ULES00151")
        return fail("foreign pick");

    save_picker_begin(SavePickerMode::Load, load, 3);
    std::vector<std::uint8_t> picture;
    std::uint64_t picture_version = 0u;
    if (!save_picker_image(picture, picture_version) ||
        picture.size() != kSavePickerImageWidth * kSavePickerImageHeight * 4u)
        return fail("list picture");
    const std::uint64_t first_version = picture_version;
    if (!save_picker_image(picture, picture_version) || picture_version != first_version)
        return fail("picture redrawn without a change");
    host_settings_handle(HostSettingsKey::Up);
    if (!save_picker_image(picture, picture_version) || picture_version == first_version)
        return fail("picture not redrawn after a move");
    host_settings_handle(HostSettingsKey::Close);
    if (save_picker_take(chosen) != SavePickerOutcome::Cancelled) return fail("cancel");

    const auto save = scan_save_slots(root, "ULUS10041", names, SavePickerMode::Save);
    if (save.size() != 4u || !save[0].exists || save[1].exists || save[1].foreign)
        return fail("save list");
    if (save_focus_index(save, kSaveFocusFirstEmpty) != 1) return fail("first empty");
    save_picker_begin(SavePickerMode::Save, save, 0);
    host_settings_handle(HostSettingsKey::Confirm);  // overwrite?
    host_settings_handle(HostSettingsKey::Close);    // no
    host_settings_handle(HostSettingsKey::Down);
    host_settings_handle(HostSettingsKey::Confirm);  // empty slot: no question
    if (save_picker_take(chosen) != SavePickerOutcome::Chosen || chosen.slot != "S1") return fail("save pick");

    save_picker_begin(SavePickerMode::Load, {}, 0);
    host_settings_handle(HostSettingsKey::Confirm);
    if (save_picker_take(chosen) != SavePickerOutcome::Cancelled) return fail("empty list");
    std::filesystem::remove_all(root);
    std::cout << "sfo=1 load=4 foreign=1 encrypted=1 save=4\n";
    return 0;
}

// PSP savedata decryption: known answers (4116-byte input, bytes (i*7+3)&255,
// key 00..0F), cross-checked against an independent transcription of the PSP
// scheme; 4100 bytes span three 2048-byte chunks of the keystream.
int crypto() {
    using namespace lcs;
    std::vector<std::uint8_t> file(16u + 4100u);
    for (std::size_t index = 0; index < file.size(); ++index) file[index] = static_cast<std::uint8_t>((index * 7u + 3u) & 255u);
    std::array<std::uint8_t, 16> key{};
    for (std::size_t index = 0; index < 16u; ++index) key[index] = static_cast<std::uint8_t>(index);
    const auto hex = [](const std::vector<std::uint8_t> &data, std::size_t at, std::size_t count) {
        std::string text;
        char part[3];
        for (std::size_t index = at; index < at + count; ++index) {
            std::snprintf(part, sizeof(part), "%02x", data[index]);
            text += part;
        }
        return text;
    };
    const struct { int mode; const char *head; const char *tail; } cases[] = {
        {1, "14180f6f1ce4d63b", "776607d6828e921cb1e8"},
        {3, "435aaaac52b31f99", "06aa908010cc4a52c5e0"},
        {5, "8755a63ed76d2132", "2c830f1187c29cabfd0d"},
    };
    for (const auto &item : cases) {
        const auto out = decrypt_psp_savedata(file, item.mode, key);
        if (!out || out->size() != 4100u) return fail("decrypt size");
        if (hex(*out, 0, 8) != item.head || hex(*out, 4090, 10) != item.tail) {
            std::cerr << "mode " << item.mode << ": " << hex(*out, 0, 8) << " .. " << hex(*out, 4090, 10) << "\n";
            return fail("decrypt known answer");
        }
    }
    if (savedata_crypt_mode(0x21u) != 3 || savedata_crypt_mode(0x41u) != 5 || savedata_crypt_mode(0u) != 0)
        return fail("crypt mode flags");
    std::vector<std::uint8_t> zeros(8192u, 0u);
    if (savedata_looks_encrypted(zeros)) return fail("zeros look encrypted");
    if (!savedata_looks_encrypted(*decrypt_psp_savedata(std::vector<std::uint8_t>(16u + 8192u, 0u), 3, key)))
        return fail("keystream does not look encrypted");
    std::cout << "modes=1,3,5 known_answers=ok\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode == "menu") return menu_keys();
    if (mode == "present") return present_policy();
    if (mode == "f10") return f10_key();
    if (mode == "bindings") return bindings();
    if (mode == "fps") return fps_counter();
    if (mode == "persist") return persist();
    if (mode == "triggers") return triggers();
    if (mode == "saves") return saves();
    if (mode == "crypto") return crypto();
    return 2;
}
