#include "display_window.hpp"

#if !defined(_WIN32)

#include "ge_gpu_backend.hpp"
#include "lcs_controls.hpp"
#include "lcs_menu.hpp"
#include "lcs_mouse.hpp"
#include "lcs_display_menu.hpp"
#include "lcs_key_bindings.hpp"
#include "lcs_render_config.hpp"
#include "host_font_5x7.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace lcs {
namespace {

SDL_Window *g_window{};
SDL_Renderer *g_renderer{};
SDL_Texture *g_texture{};
SDL_GameController *g_pad{};
std::thread::id g_window_thread{};
bool g_closed{};
bool g_focused{true};
std::uint32_t g_texture_width{};
std::uint32_t g_texture_height{};
std::vector<std::uint8_t> g_pending_frame{};
std::uint32_t g_pending_width{};
std::uint32_t g_pending_height{};
std::atomic<int> g_wheel{0};
std::atomic<std::int32_t> g_mouse_dx{0};
std::atomic<std::int32_t> g_mouse_dy{0};
std::mutex g_sdl_mutex;

void discard_pending_pointer() noexcept {
    g_wheel.store(0, std::memory_order_relaxed);
    g_mouse_dx.store(0, std::memory_order_relaxed);
    g_mouse_dy.store(0, std::memory_order_relaxed);
}

constexpr int sdl_scancode(HostKey key) noexcept {
    const int id = static_cast<int>(key);
    if (host_key_in(key, HostKey::A, HostKey::Z))
        return SDL_SCANCODE_A + (id - static_cast<int>(HostKey::A));
    if (key == HostKey::Digit0)
        return SDL_SCANCODE_0;
    if (host_key_in(key, HostKey::Digit1, HostKey::Digit9))
        return SDL_SCANCODE_1 + (id - static_cast<int>(HostKey::Digit1));
    if (host_key_in(key, HostKey::F1, HostKey::F12))
        return SDL_SCANCODE_F1 + (id - static_cast<int>(HostKey::F1));
    if (host_key_in(key, HostKey::Numpad0, HostKey::Numpad9)) {
        const int number = id - static_cast<int>(HostKey::Numpad0);
        return number == 0 ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1 + (number - 1);
    }
    switch (key) {
    case HostKey::Space: return SDL_SCANCODE_SPACE;
    case HostKey::Enter: return SDL_SCANCODE_RETURN;
    case HostKey::Escape: return SDL_SCANCODE_ESCAPE;
    case HostKey::Tab: return SDL_SCANCODE_TAB;
    case HostKey::Backspace: return SDL_SCANCODE_BACKSPACE;
    case HostKey::LeftShift: return SDL_SCANCODE_LSHIFT;
    case HostKey::RightShift: return SDL_SCANCODE_RSHIFT;
    case HostKey::LeftCtrl: return SDL_SCANCODE_LCTRL;
    case HostKey::RightCtrl: return SDL_SCANCODE_RCTRL;
    case HostKey::LeftAlt: return SDL_SCANCODE_LALT;
    case HostKey::RightAlt: return SDL_SCANCODE_RALT;
    case HostKey::Up: return SDL_SCANCODE_UP;
    case HostKey::Down: return SDL_SCANCODE_DOWN;
    case HostKey::Left: return SDL_SCANCODE_LEFT;
    case HostKey::Right: return SDL_SCANCODE_RIGHT;
    case HostKey::NumpadPlus: return SDL_SCANCODE_KP_PLUS;
    case HostKey::NumpadMinus: return SDL_SCANCODE_KP_MINUS;
    case HostKey::Delete: return SDL_SCANCODE_DELETE;
    case HostKey::Insert: return SDL_SCANCODE_INSERT;
    case HostKey::Home: return SDL_SCANCODE_HOME;
    case HostKey::End: return SDL_SCANCODE_END;
    case HostKey::PageUp: return SDL_SCANCODE_PAGEUP;
    case HostKey::PageDown: return SDL_SCANCODE_PAGEDOWN;
    case HostKey::Comma: return SDL_SCANCODE_COMMA;
    case HostKey::Period: return SDL_SCANCODE_PERIOD;
    case HostKey::Minus: return SDL_SCANCODE_MINUS;
    case HostKey::Equals: return SDL_SCANCODE_EQUALS;
    default: return -1;
    }
}

static_assert(sdl_scancode(HostKey::Digit0) == SDL_SCANCODE_0);
static_assert(sdl_scancode(HostKey::Digit1) == SDL_SCANCODE_1);
static_assert(sdl_scancode(HostKey::Digit9) == SDL_SCANCODE_9);

bool key_down(SDL_Scancode scancode) noexcept {
    const std::uint8_t *keys = SDL_GetKeyboardState(nullptr);
    return keys != nullptr && keys[scancode] != 0;
}

#if defined(__SWITCH__)
// Every connected controller drives the game: handheld, detached Joy-Cons, a
// Pro Controller, whichever the player picks up.
constexpr int kMaxPads = 8;
SDL_GameController *g_pads[kMaxPads]{};

void close_pad(SDL_JoystickID instance = -1) noexcept {
    for (SDL_GameController *&pad : g_pads) {
        if (pad == nullptr) continue;
        if (instance >= 0 && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) != instance)
            continue;
        SDL_GameControllerClose(pad);
        pad = nullptr;
    }
    g_pad = nullptr;
    for (SDL_GameController *pad : g_pads)
        if (pad != nullptr && g_pad == nullptr) g_pad = pad;
}

void open_pad(int device_index) noexcept {
    if (!SDL_IsGameController(device_index)) return;
    const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(device_index);
    for (SDL_GameController *pad : g_pads)
        if (pad != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == instance)
            return;
    for (SDL_GameController *&pad : g_pads) {
        if (pad != nullptr) continue;
        pad = SDL_GameControllerOpen(device_index);
        if (g_pad == nullptr) g_pad = pad;
        return;
    }
}

bool pad_button(SDL_GameControllerButton id) noexcept {
    for (SDL_GameController *pad : g_pads)
        if (pad != nullptr && SDL_GameControllerGetButton(pad, id) != 0) return true;
    return false;
}

// The largest deflection among the controllers.
Sint16 pad_axis(SDL_GameControllerAxis id) noexcept {
    Sint16 best = 0;
    for (SDL_GameController *pad : g_pads) {
        if (pad == nullptr) continue;
        const Sint16 value = SDL_GameControllerGetAxis(pad, id);
        if (std::abs(static_cast<int>(value)) > std::abs(static_cast<int>(best))) best = value;
    }
    return best;
}
#else
void close_pad() noexcept {
    if (g_pad == nullptr) return;
    SDL_GameControllerClose(g_pad);
    g_pad = nullptr;
}

void open_pad(int device_index) noexcept {
    if (g_pad != nullptr || !SDL_IsGameController(device_index)) return;
    g_pad = SDL_GameControllerOpen(device_index);
}

bool pad_button(SDL_GameControllerButton id) noexcept {
    return g_pad != nullptr && SDL_GameControllerGetButton(g_pad, id) != 0;
}

Sint16 pad_axis(SDL_GameControllerAxis id) noexcept {
    return g_pad != nullptr ? SDL_GameControllerGetAxis(g_pad, id) : Sint16{0};
}
#endif

void apply_window_fullscreen(bool enabled) noexcept {
    if (g_window == nullptr) return;
    const bool fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (fullscreen == enabled) return;
    SDL_SetWindowFullscreen(g_window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
}

void toggle_fullscreen() noexcept {
    if (g_window == nullptr) return;
    const bool next = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;
    apply_window_fullscreen(next);
    host_settings_note_fullscreen(next);
}

std::atomic<int> g_fullscreen_request{-1};

void apply_fullscreen_request() noexcept {
    const int request = g_fullscreen_request.exchange(-1, std::memory_order_relaxed);
    if (request < 0 || g_window == nullptr) return;
    const bool fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (fullscreen != (request != 0))
        SDL_SetWindowFullscreen(g_window, request != 0 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
}

#if defined(__SWITCH__)
// No keyboard on a Switch. Both sticks pressed in (L3 + R3) open and close the
// settings panel; neither click reaches the game while both are held. In the
// panel the d-pad moves and B closes.
bool g_stick_combo_held = false;

void poll_switch_settings_combo() noexcept {
    const bool both = pad_button(SDL_CONTROLLER_BUTTON_LEFTSTICK) &&
                      pad_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (both && !g_stick_combo_held) {
        const bool was_open = host_settings_open();
        host_settings_handle(HostSettingsKey::Toggle);
        if (was_open) discard_pending_pointer();
    }
    // Stays set until both are released, so the clicks do not leak afterwards.
    if (both) g_stick_combo_held = true;
    else if (!pad_button(SDL_CONTROLLER_BUTTON_LEFTSTICK) &&
             !pad_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK))
        g_stick_combo_held = false;
}

void handle_switch_pad_button(std::uint8_t button) noexcept {
    if (!host_settings_open()) return;
    // SDL names the buttons by position: SDL "A" is the bottom one (Switch B),
    // SDL "B" the right one (Switch A). The confirm button follows Settings >
    // Confirm button, as in the game's menus.
    const bool nintendo = lcs_render_configuration().nx.nintendo_confirm;
    const std::uint8_t confirm = nintendo ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
    const std::uint8_t back = nintendo ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
    if (button == confirm) {
        host_settings_handle(HostSettingsKey::Confirm);
        return;
    }
    if (button == back) {
        host_settings_handle(HostSettingsKey::Close);
        if (!host_settings_open()) discard_pending_pointer();
        return;
    }
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: host_settings_handle(HostSettingsKey::Up); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: host_settings_handle(HostSettingsKey::Down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: host_settings_handle(HostSettingsKey::Left); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: host_settings_handle(HostSettingsKey::Right); break;
    default: break;
    }
}
#endif

void pump_events() noexcept {
    apply_fullscreen_request();
    SDL_Event event;
    while (SDL_PollEvent(&event) == 1) {
        if (event.type == SDL_QUIT) g_closed = true;
        else if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
                 event.key.keysym.scancode == SDL_SCANCODE_F10) {
            const bool was_open = host_settings_open();
            host_settings_handle(HostSettingsKey::Toggle);
            if (was_open) discard_pending_pointer();
        } else if (event.type == SDL_KEYDOWN && host_settings_open()) {
            const SDL_Scancode code = event.key.keysym.scancode;
            if (code == SDL_SCANCODE_UP) host_settings_handle(HostSettingsKey::Up);
            else if (code == SDL_SCANCODE_DOWN) host_settings_handle(HostSettingsKey::Down);
            else if (code == SDL_SCANCODE_LEFT) host_settings_handle(HostSettingsKey::Left);
            else if (code == SDL_SCANCODE_RIGHT) host_settings_handle(HostSettingsKey::Right);
            else if ((code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER) && event.key.repeat == 0)
                host_settings_handle(HostSettingsKey::Confirm);
            else if (code == SDL_SCANCODE_ESCAPE) {
                host_settings_handle(HostSettingsKey::Close);
                discard_pending_pointer();
            }
        } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_F11 &&
                 event.key.repeat == 0)
            toggle_fullscreen();
        else if (event.type == SDL_MOUSEWHEEL) {
            if (!host_settings_open())
                g_wheel.fetch_add(event.wheel.y, std::memory_order_relaxed);
        } else if (event.type == SDL_MOUSEMOTION) {
            if (!host_settings_open()) {
                g_mouse_dx.fetch_add(event.motion.xrel, std::memory_order_relaxed);
                g_mouse_dy.fetch_add(event.motion.yrel, std::memory_order_relaxed);
            }
#if defined(__SWITCH__)
        } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            handle_switch_pad_button(event.cbutton.button);
#endif
        } else if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_pad(event.cdevice.which);
#if defined(__SWITCH__)
        else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
            close_pad(event.cdevice.which);
#else
        else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
            close_pad();
#endif
        else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            g_focused = true;
        else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            g_focused = false;
        else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)
            g_closed = true;
    }
#if defined(__SWITCH__)
    poll_switch_settings_combo();
#endif
}

std::uint32_t unpack_pixel(const std::uint8_t *src, std::uint32_t format) {
    std::uint32_t r = 0u;
    std::uint32_t g = 0u;
    std::uint32_t b = 0u;
    switch (format) {
    case 0u: {
        const std::uint16_t value = static_cast<std::uint16_t>(src[0] | (src[1] << 8));
        r = (value & 0x1Fu) * 255u / 31u;
        g = ((value >> 5u) & 0x3Fu) * 255u / 63u;
        b = ((value >> 11u) & 0x1Fu) * 255u / 31u;
        break;
    }
    case 1u: {
        const std::uint16_t value = static_cast<std::uint16_t>(src[0] | (src[1] << 8));
        r = (value & 0x1Fu) * 255u / 31u;
        g = ((value >> 5u) & 0x1Fu) * 255u / 31u;
        b = ((value >> 10u) & 0x1Fu) * 255u / 31u;
        break;
    }
    case 2u: {
        const std::uint16_t value = static_cast<std::uint16_t>(src[0] | (src[1] << 8));
        r = (value & 0xFu) * 17u;
        g = ((value >> 4u) & 0xFu) * 17u;
        b = ((value >> 8u) & 0xFu) * 17u;
        break;
    }
    default:
        r = src[0];
        g = src[1];
        b = src[2];
        break;
    }
    return r | (g << 8u) | (b << 16u) | 0xFF000000u;
}

std::uint32_t bytes_per_pixel(std::uint32_t format) { return format == 3u ? 4u : 2u; }

void fill_rect(SDL_Renderer *renderer, int x, int y, int w, int h) noexcept {
    const SDL_Rect rect{x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

void draw_text(SDL_Renderer *renderer, int x, int y, int scale, const char *text) noexcept {
    for (int column = 0; text[column] != '\0'; ++column) {
        const unsigned char code = static_cast<unsigned char>(text[column]);
        const int index = code < 128u ? kGlyphIndex[code] : -1;
        const std::array<std::uint8_t, 7> &rows =
            index >= 0 ? kGlyph5x7[static_cast<std::size_t>(index)] : kGlyph5x7[0];
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = rows[static_cast<std::size_t>(row)];
            for (int bit = 0; bit < 5; ++bit) {
                if ((bits & (0x10 >> bit)) == 0) continue;
                fill_rect(renderer, x + (column * 6 + bit) * scale, y + row * scale, scale, scale);
            }
        }
    }
}

std::chrono::steady_clock::time_point g_fps_window_start{};
std::uint32_t g_fps_window_frames{};
double g_host_fps{};

void note_host_frame() noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (g_fps_window_start.time_since_epoch().count() == 0) g_fps_window_start = now;
    ++g_fps_window_frames;
    const double seconds = std::chrono::duration<double>(now - g_fps_window_start).count();
    if (seconds < 0.5) return;
    g_host_fps = static_cast<double>(g_fps_window_frames) / seconds;
    g_fps_window_frames = 0u;
    g_fps_window_start = now;
}

void paint_fps(SDL_Renderer *renderer) noexcept {
    if (renderer == nullptr || !lcs_render_configuration().display.show_fps) return;
    int output_w = 960;
    int output_h = 544;
    SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
    const int scale = std::max(2, output_h / 280);
    const int margin = 8 * scale;
    char label[24];
    if (g_host_fps > 0.0)
        std::snprintf(label, sizeof(label), "FPS %.0f", std::clamp(g_host_fps, 0.0, 999.0));
    else
        std::snprintf(label, sizeof(label), "FPS --");
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 8, 12, 28, 210);
    fill_rect(renderer, margin, margin, 9 * 6 * scale, 11 * scale);
    SDL_SetRenderDrawColor(renderer, 232, 248, 255, 255);
    draw_text(renderer, margin + 2 * scale, margin + 2 * scale, scale, label);
}

void paint_host_settings(SDL_Renderer *renderer) noexcept {
    if (!host_settings_open() || renderer == nullptr) return;
    int output_w = 960;
    int output_h = 544;
    SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
    const int scale = std::max(2, output_h / 280);
    const HostSettingsView view = host_settings_view();
    const int panel_w = 42 * 6 * scale;
    const int panel_h = 16 * 8 * scale;
    const int margin = 8 * scale;
    const int origin_x = view.centered ? std::max(margin, (output_w - panel_w) / 2)
                                       : std::max(margin, output_w - panel_w - margin);
    const int origin_y = view.centered ? std::max(margin, (output_h - panel_h) / 2) : margin;
    SDL_SetRenderDrawColor(renderer, 8, 12, 28, 230);
    fill_rect(renderer, origin_x, origin_y, panel_w, panel_h);
    SDL_SetRenderDrawColor(renderer, 180, 200, 230, 255);
    draw_text(renderer, origin_x + 8 * scale, origin_y + 6 * scale, scale, view.title);
    draw_text(renderer, origin_x + 8 * scale, origin_y + 16 * scale, scale, view.hint);
    for (int row = 0; row < std::clamp(view.row_count, 0, kHostSettingsMaxRows); ++row) {
        const int y = origin_y + (28 + row * 12) * scale;
        if (row == view.selected) {
            SDL_SetRenderDrawColor(renderer, 40, 70, 120, 255);
            fill_rect(renderer, origin_x + 4 * scale, y - scale, panel_w - 8 * scale, 9 * scale);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 170, 190, 220, 255);
        }
        draw_text(renderer, origin_x + 8 * scale, y, scale, view.rows[row]);
    }
    SDL_SetRenderDrawColor(renderer, 220, 180, 80, 255);
    for (int line = 0; line < 2; ++line) {
        draw_text(renderer, origin_x + 8 * scale,
                  origin_y + (28 + (std::clamp(view.row_count, 0, kHostSettingsMaxRows) + line) * 12) * scale,
                  scale, view.footer[line]);
    }
}

SDL_Rect game_texture_destination(std::uint32_t source_width, std::uint32_t source_height) noexcept {
    int output_w = static_cast<int>(source_width);
    int output_h = static_cast<int>(source_height);
    if (g_renderer != nullptr)
        SDL_GetRendererOutputSize(g_renderer, &output_w, &output_h);
    const DisplayConfiguration &display = lcs_render_configuration().display;
    const PresentationRectangle fitted = calculate_presentation_rectangle(
        static_cast<std::uint32_t>(std::max(output_w, 0)),
        static_cast<std::uint32_t>(std::max(output_h, 0)), source_width, source_height,
        display.aspect_mode, display.integer_scale);
    return SDL_Rect{fitted.x, fitted.y, std::max(0, fitted.width), std::max(0, fitted.height)};
}

void present_game_texture(std::uint32_t source_width, std::uint32_t source_height) noexcept {
    if (g_renderer == nullptr || g_texture == nullptr) return;
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    const SDL_Rect destination = game_texture_destination(source_width, source_height);
    if (destination.w > 0 && destination.h > 0)
        SDL_RenderCopy(g_renderer, g_texture, nullptr, &destination);
    paint_fps(g_renderer);
    paint_host_settings(g_renderer);
    SDL_RenderPresent(g_renderer);
}

void blit_rgba(const void *pixels, std::uint32_t width, std::uint32_t height, int pitch) {
    if (g_renderer == nullptr || width == 0u || height == 0u || pixels == nullptr) return;
    if (g_texture == nullptr || g_texture_width != width || g_texture_height != height) {
        if (g_texture != nullptr) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                      static_cast<int>(width), static_cast<int>(height));
        g_texture_width = width;
        g_texture_height = height;
        if (g_texture != nullptr) SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_NONE);
    }
    if (g_texture == nullptr) return;
    SDL_UpdateTexture(g_texture, nullptr, pixels, pitch);
    note_host_frame();
    present_game_texture(width, height);
}

bool present_swapchain_rgba(const std::uint8_t *pixels, std::uint32_t width,
                            std::uint32_t height) noexcept {
    const std::span<const std::byte> rgba{
        reinterpret_cast<const std::byte *>(pixels),
        static_cast<std::size_t>(width) * height * 4u};
    return ge_gpu_backend_present_guest_rgba(rgba, width, height);
}

void present_rgba_bytes(const std::uint8_t *pixels, std::uint32_t width, std::uint32_t height) {
    if (pixels == nullptr || width == 0u || height == 0u) return;
    if (present_swapchain_rgba(pixels, width, height)) return;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4u;
    // Wayland only shows a window whose buffers are committed on the thread that created it.
    // GE completion runs on a worker thread, so hand the pixels to the window thread.
    if (std::this_thread::get_id() != g_window_thread) {
        g_pending_frame.assign(pixels, pixels + bytes);
        g_pending_width = width;
        g_pending_height = height;
        return;
    }
    blit_rgba(pixels, width, height, static_cast<int>(width * 4u));
}

void present_pending_frame() {
    if (g_pending_frame.empty() || g_pending_width == 0u || g_pending_height == 0u) return;
    if (present_swapchain_rgba(g_pending_frame.data(), g_pending_width, g_pending_height)) {
        g_pending_frame.clear();
        return;
    }
    blit_rgba(g_pending_frame.data(), g_pending_width, g_pending_height,
              static_cast<int>(g_pending_width * 4u));
    g_pending_frame.clear();
}

void create_software_renderer() {
    if (g_window == nullptr || g_renderer != nullptr) return;
#if defined(__SWITCH__)
    // Scaling 480x272 to the screen on the CPU would cost the game's cores;
    // SDL's GLES2 renderer runs on the Switch GPU through Mesa.
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (g_renderer == nullptr)
#endif
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (g_renderer == nullptr) {
        std::cerr << "[window] SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        return;
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(g_window, &width, &height);
    SDL_RendererInfo info{};
    const char *renderer_name = SDL_GetRendererInfo(g_renderer, &info) == 0 ? info.name : "unknown";
    std::cerr << "[window] shown " << width << "x" << height
              << " renderer=" << renderer_name << "  F10 host settings\n";
}

}  // namespace

void display_window_init() {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    if (g_window != nullptr) return;
    g_window_thread = std::this_thread::get_id();
#if defined(__SWITCH__)
    // A touch would otherwise arrive as mouse look plus a left click (Attack).
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "[window] SDL_Init failed: " << SDL_GetError() << "\n";
        return;
    }
    const bool nearest = lcs_render_configuration().display.upscale_filter == DisplayUpscaleFilter::Nearest;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, nearest ? "0" : "1");
#if defined(__SWITCH__)
    // SDL picks the window's EGL config now, so ask for desktop GL before the
    // window exists when the OpenGL GE will want a context on it.
    if (lcs_render_configuration().rendering.backend != RenderingBackend::Software) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    }
    // One full-screen layer; the system scales it to 1080p when docked.
    const DisplaySurfaceDimensions surface{1280u, 720u};
    const Uint32 window_flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN;
    g_window = SDL_CreateWindow("LCSNative - GTA: Liberty City Stories",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                static_cast<int>(surface.width), static_cast<int>(surface.height),
                                window_flags | SDL_WINDOW_OPENGL);
#else
    const DisplaySurfaceDimensions surface = resolve_window_dimensions();
    // Takes the Wayland surface, so create it only when the swapchain cannot.
    // SDL_WINDOW_VULKAN fails when the video driver has no Vulkan support.
    const Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN;
    g_window = SDL_CreateWindow("LCSNative - GTA: Liberty City Stories",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                static_cast<int>(surface.width), static_cast<int>(surface.height),
                                window_flags | SDL_WINDOW_VULKAN);
#endif
    if (g_window == nullptr) {
        std::cerr << "[window] Vulkan window unavailable: " << SDL_GetError() << "\n";
        g_window = SDL_CreateWindow("LCSNative - GTA: Liberty City Stories",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    static_cast<int>(surface.width), static_cast<int>(surface.height),
                                    window_flags);
    }
    if (g_window == nullptr) {
        std::cerr << "[window] SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return;
    }
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    pump_events();
#if !defined(__SWITCH__)
    apply_window_fullscreen(lcs_render_configuration().display.fullscreen);
#endif
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        open_pad(index);
#if !defined(__SWITCH__)
        if (g_pad != nullptr) break;
#endif
    }
    ge_gpu_backend_set_native_window(g_window);
}

void display_window_attach_gpu_backend() {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    if (g_window == nullptr) return;
    ge_gpu_backend_set_native_window(g_window);
    if (ge_gpu_backend_guest_present_available()) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(g_window, &width, &height);
        std::cerr << "[window] shown " << width << "x" << height
#if defined(__SWITCH__)
                  << " presenter=gpu  L3+R3 settings\n";
#else
                  << " presenter=gpu  F10 host settings\n";
#endif
        return;
    }
    create_software_renderer();
}

bool display_window_profile_key_pressed() {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    pump_events();
    static bool was_down = false;
    const bool down = g_window != nullptr && g_focused && key_down(SDL_SCANCODE_P);
    const bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

bool display_window_closed() { return g_closed; }

void display_window_request_fullscreen(bool enabled) noexcept {
    g_fullscreen_request.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void display_window_pump() {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    present_pending_frame();
    pump_events();
    if (host_settings_open() && g_renderer != nullptr && g_texture != nullptr && g_pending_frame.empty())
        present_game_texture(g_texture_width, g_texture_height);
}

void display_window_present(psprecomp::Runtime &runtime, std::uint32_t frame_buffer,
                            std::uint32_t buffer_width, std::uint32_t pixel_format,
                            std::uint32_t width, std::uint32_t height) {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    if (g_closed || g_window == nullptr) return;
    static auto last_present = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_present < std::chrono::milliseconds(33)) return;
    last_present = now;
    if (frame_buffer == 0u || width == 0u || height == 0u || buffer_width == 0u) return;
    const std::uint32_t stride_bytes = buffer_width * bytes_per_pixel(pixel_format);
    const std::size_t total_bytes = static_cast<std::size_t>(stride_bytes) * height;
    const std::uint8_t *source = runtime.memory().raw_pointer(frame_buffer, total_bytes);
    if (source == nullptr) return;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    const std::uint32_t bpp = bytes_per_pixel(pixel_format);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *row = source + static_cast<std::size_t>(y) * stride_bytes;
        for (std::uint32_t x = 0; x < width; ++x)
            pixels[static_cast<std::size_t>(y) * width + x] =
                unpack_pixel(row + static_cast<std::size_t>(x) * bpp, pixel_format);
    }
    present_rgba_bytes(reinterpret_cast<const std::uint8_t *>(pixels.data()), width, height);
}

void display_window_present_rgba(std::span<const std::byte> rgba, std::uint32_t width,
                                 std::uint32_t height) {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    if (g_window == nullptr || width == 0u || height == 0u) return;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    if (rgba.size() < pixel_count * 4u) return;
    present_rgba_bytes(reinterpret_cast<const std::uint8_t *>(rgba.data()), width, height);
}

void display_window_shutdown() {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    close_pad();
    if (g_texture != nullptr) {
        SDL_DestroyTexture(g_texture);
        g_texture = nullptr;
    }
    if (g_renderer != nullptr) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = nullptr;
    }
    if (g_window != nullptr) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
}

HostInputState display_window_input() {
    std::lock_guard<std::mutex> guard(g_sdl_mutex);
    static HostInputState cached{};
    static std::chrono::steady_clock::time_point cached_at{};
    const auto poll_time = std::chrono::steady_clock::now();
    if (host_settings_open()) discard_pending_pointer();
    if (cached_at.time_since_epoch().count() != 0 &&
        poll_time - cached_at < std::chrono::milliseconds(4))
        return cached;
    cached_at = poll_time;
    pump_events();
    if (host_settings_open()) {
        discard_pending_pointer();
        cached = {};
        lcs_camera_set_axes(0, 0);
        lcs_set_host_drive_inputs(false, false);
        return cached;
    }

    HostInputState input{};
    const auto publish = [&]() -> HostInputState {
        lcs_camera_set_axes(input.camera_x, input.camera_y);
        lcs_set_host_drive_inputs(input.accelerate, input.brake);
        cached = input;
        return cached;
    };
    const int wheel = g_wheel.exchange(0, std::memory_order_relaxed);
    const std::int32_t mouse_dx = g_mouse_dx.exchange(0, std::memory_order_relaxed);
    const std::int32_t mouse_dy = g_mouse_dy.exchange(0, std::memory_order_relaxed);
    const bool focused = g_window != nullptr && g_focused;
    const bool menu = lcs_menu_active();
    const bool in_game = lcs_camera_in_use() && !menu;
    const bool minimized = g_window != nullptr &&
                           (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) != 0;
    const bool capture = focused && in_game && !minimized && lcs_camera_hook_enabled();
    if ((SDL_GetRelativeMouseMode() == SDL_TRUE) != capture)
        SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
    if ((SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE) != menu)
        SDL_ShowCursor(menu ? SDL_ENABLE : SDL_DISABLE);
    const bool driving = lcs_player_in_vehicle();
    const ControlsConfiguration &controls = lcs_render_configuration().controls;
    if (focused) {
        const std::uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
        const auto mouse_held = [mouse](std::uint32_t mask) {
            return (mouse & mask) != 0;
        };
        const auto is_down = [&](HostKey key) {
            const int code = sdl_scancode(key);
            if (code >= 0) return key_down(static_cast<SDL_Scancode>(code));
            switch (key) {
            case HostKey::MouseLeft: return mouse_held(SDL_BUTTON_LMASK);
            case HostKey::MouseRight: return mouse_held(SDL_BUTTON_RMASK);
            case HostKey::MouseMiddle: return mouse_held(SDL_BUTTON_MMASK);
            case HostKey::Mouse4: return mouse_held(SDL_BUTTON_X1MASK);
            case HostKey::Mouse5: return mouse_held(SDL_BUTTON_X2MASK);
            default: return false;
            }
        };
        const KeyboardSample keys = sample_keyboard(controls.bindings, driving, is_down);
        input.buttons |= keys.buttons;
        input.accelerate = keys.accelerate;
        input.brake = keys.brake;
        input.analog_x = static_cast<std::uint8_t>(
            std::clamp(128 + keys.move_x * keys.reach, 0, 255));
        input.analog_y = static_cast<std::uint8_t>(
            std::clamp(128 + keys.move_y * keys.reach, 0, 255));
        const int sensitivity = static_cast<int>(controls.mouse_sensitivity);
        const auto camera_response = [sensitivity](std::int32_t delta) {
            const double scaled = std::abs(delta) * (sensitivity / 12.0);
            const double magnitude = 127.0 * scaled / (scaled + 12.0);
            return static_cast<int>(std::lround(delta < 0 ? -magnitude : magnitude));
        };
        input.camera_x = camera_response(mouse_dx);
        input.camera_y = camera_response(-mouse_dy);
        if (capture) lcs_add_mouse_camera_delta(mouse_dx, mouse_dy);
        if (controls.invert_camera_y) input.camera_y = -input.camera_y;
    }

    static int wheel_hold = 0;
    static std::uint32_t wheel_button = 0u;
    if (focused && wheel != 0) {
        if (lcs_player_aiming()) {
            wheel_button = wheel > 0 ? kPspSquare : kPspCross;
            wheel_hold = 8;
        } else {
            wheel_button = binding_wheel_buttons(controls.bindings, wheel > 0, driving);
            wheel_hold = 4;
        }
    }
    if (wheel_hold > 0) {
        --wheel_hold;
        input.buttons |= wheel_button;
    }
    if (!focused || g_pad == nullptr) return publish();

#if defined(__SWITCH__)
    // Nintendo layout in menus: A (the right button) confirms and B goes back.
    // The game's menus confirm with Cross, the bottom button, so the two trade
    // places while a menu is up. Gameplay keeps the PSP positions.
    const bool swap_confirm = menu && lcs_render_configuration().nx.nintendo_confirm;
    const bool stick_combo = g_stick_combo_held;
#else
    constexpr bool swap_confirm = false;
    constexpr bool stick_combo = false;
#endif
    const auto pad_down = [&](PadButton button) {
        const auto held = [&](SDL_GameControllerButton id) { return pad_button(id); };
        const auto axis = [&](SDL_GameControllerAxis id) { return pad_axis(id); };
        switch (button) {
        case PadButton::A: return held(swap_confirm ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A);
        case PadButton::B: return held(swap_confirm ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B);
        case PadButton::X: return held(SDL_CONTROLLER_BUTTON_X);
        case PadButton::Y: return held(SDL_CONTROLLER_BUTTON_Y);
        case PadButton::LeftShoulder: return held(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        case PadButton::RightShoulder: return held(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        case PadButton::Start: return held(SDL_CONTROLLER_BUTTON_START);
        case PadButton::Back: return held(SDL_CONTROLLER_BUTTON_BACK);
        case PadButton::DpadUp: return held(SDL_CONTROLLER_BUTTON_DPAD_UP);
        case PadButton::DpadDown: return held(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        case PadButton::DpadLeft: return held(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        case PadButton::DpadRight: return held(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        case PadButton::LeftClick: return !stick_combo && held(SDL_CONTROLLER_BUTTON_LEFTSTICK);
        case PadButton::RightClick: return !stick_combo && held(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        case PadButton::LeftTrigger: return axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192;
        case PadButton::RightTrigger: return axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192;
        case PadButton::Count: return false;
        }
        return false;
    };
    const PadSample pad = sample_pad(controls.bindings, driving, pad_down);
    input.buttons |= pad.buttons;
    if (pad.accelerate) input.accelerate = true;
    if (pad.brake) input.brake = true;
    const auto apply_stick = [&](SDL_GameControllerAxis x_axis, SDL_GameControllerAxis y_axis,
                                 StickRole role) {
        const StickReading reading = interpret_stick(
            role, pad_axis(x_axis), pad_axis(y_axis),
            false, controls.invert_camera_y);
        if (reading.move) {
            input.analog_x = reading.move_x;
            input.analog_y = reading.move_y;
        }
        if (reading.camera) {
            input.camera_x = reading.camera_x;
            input.camera_y = reading.camera_y;
        }
    };
    apply_stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, controls.bindings.left_stick);
    apply_stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, controls.bindings.right_stick);
    return publish();
}

}  // namespace lcs

#endif
