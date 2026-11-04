// Button prompt pictures of the HUD are the game's own textures, so a different
// controller style means repainting those textures as they are uploaded. The
// launcher writes the replacement RGBA8 files; this picks them by the content
// hash the GE renderer already computes for every texture.

#include "lcs_prompt_textures.hpp"

#include "lcs_render_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lcs {
namespace {

std::string signature_name(std::uint64_t signature) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx",
                  static_cast<unsigned long long>(signature));
    return buffer;
}

std::string lowercase(std::string value) {
    for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::filesystem::path configuration_root() {
    const LcsConfiguration &config = lcs_render_configuration();
    if (config.source_path.has_parent_path()) return config.source_path.parent_path();
    return config.executable_directory;
}

std::uint32_t mip_dimension(std::uint32_t value, std::uint32_t level) {
    return std::max<std::uint32_t>(1u, value >> level);
}

std::size_t chain_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
    std::size_t total = 0u;
    for (std::uint32_t level = 0u; level < levels; ++level)
        total += static_cast<std::size_t>(mip_dimension(width, level)) *
                 mip_dimension(height, level) * 4u;
    return total;
}

struct OverrideCache {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::vector<std::byte>> loaded;
    std::unordered_set<std::uint64_t> missing;
};

OverrideCache &override_cache() {
    static OverrideCache cache;
    return cache;
}

std::vector<std::byte> read_rgba_file(const std::filesystem::path &path,
                                      std::size_t expected_bytes) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<std::size_t>(size) != expected_bytes) return {};
    std::vector<std::byte> pixels(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(pixels.data()),
                     static_cast<std::streamsize>(pixels.size())))
        return {};
    return pixels;
}

// The game ships the mip chain it wants, so a repainted mip 0 has to keep
// matching it.
void rebuild_mip_chain(std::span<std::byte> rgba8, std::uint32_t width, std::uint32_t height,
                       std::uint32_t levels) {
    std::size_t offset = 0u;
    for (std::uint32_t level = 0u; level + 1u < levels; ++level) {
        const std::uint32_t source_width = mip_dimension(width, level);
        const std::uint32_t source_height = mip_dimension(height, level);
        const std::size_t source_bytes =
            static_cast<std::size_t>(source_width) * source_height * 4u;
        const std::uint32_t target_width = mip_dimension(width, level + 1u);
        const std::uint32_t target_height = mip_dimension(height, level + 1u);
        const std::size_t target_bytes =
            static_cast<std::size_t>(target_width) * target_height * 4u;
        const std::size_t target_offset = offset + source_bytes;
        offset = target_offset;
        if (target_offset + target_bytes > rgba8.size()) return;
        const auto *source = reinterpret_cast<const std::uint8_t *>(rgba8.data() +
                                                                    (target_offset - source_bytes));
        auto *target = reinterpret_cast<std::uint8_t *>(rgba8.data() + target_offset);
        for (std::uint32_t y = 0u; y < target_height; ++y) {
            for (std::uint32_t x = 0u; x < target_width; ++x) {
                const std::uint32_t x0 = std::min(x * 2u + 0u, source_width - 1u);
                const std::uint32_t x1 = std::min(x * 2u + 1u, source_width - 1u);
                const std::uint32_t y0 = std::min(y * 2u + 0u, source_height - 1u);
                const std::uint32_t y1 = std::min(y * 2u + 1u, source_height - 1u);
                const std::uint8_t *a = source + (y0 * source_width + x0) * 4u;
                const std::uint8_t *b = source + (y0 * source_width + x1) * 4u;
                const std::uint8_t *c = source + (y1 * source_width + x0) * 4u;
                const std::uint8_t *d = source + (y1 * source_width + x1) * 4u;
                std::uint8_t *out = target + (y * target_width + x) * 4u;
                for (std::uint32_t channel = 0u; channel < 4u; ++channel)
                    out[channel] = static_cast<std::uint8_t>(
                        (a[channel] + b[channel] + c[channel] + d[channel] + 2u) / 4u);
            }
        }
    }
}

struct DumpState {
    std::mutex mutex;
    std::unordered_set<std::uint64_t> seen;
    std::filesystem::path directory;
    bool enabled{false};
    bool screen_space_only{true};
};

bool fill_dump_state(DumpState &state) {
    const char *text = std::getenv("LCS_TEXTURE_DUMP");
    if (text == nullptr || *text == '\0' || std::strcmp(text, "0") == 0) return true;
    std::filesystem::path directory;
    if (std::strcmp(text, "1") == 0) directory = configuration_root() / "textures_dump";
    else if (std::strcmp(text, "all") == 0) directory = configuration_root() / "textures_dump";
    else directory = std::filesystem::path(text);
    state.screen_space_only = std::strcmp(text, "all") != 0;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return true;
    state.directory = directory;
    state.enabled = true;
    return true;
}

DumpState &dump_state() {
    static DumpState state;
    static const bool initialized = fill_dump_state(state);
    (void)initialized;
    return state;
}

}

const std::string &prompt_texture_style() {
    static std::string style;
    const std::string configured = lowercase(lcs_render_configuration().display.prompt_style);
    if (configured.empty() || configured == "original" || configured == "psp" ||
        configured == "playstation" || configured == "off" || configured == "none")
        style.clear();
    else
        style = configured;
    return style;
}

std::filesystem::path find_override_file(const std::string &style, std::uint64_t signature,
                                         std::uint32_t width, std::uint32_t height) {
    const std::string name = signature_name(signature) + ".rgba";
    const LcsConfiguration &config = lcs_render_configuration();
    std::vector<std::filesystem::path> roots;
    roots.push_back(configuration_root());
    if (!config.executable_directory.empty()) {
        roots.push_back(config.executable_directory);
        roots.push_back(config.executable_directory / "config");
        if (config.executable_directory.has_parent_path())
            roots.push_back(config.executable_directory.parent_path() / "config");
    }
    const std::size_t expected = static_cast<std::size_t>(width) * height * 4u;
    for (const auto &root : roots) {
        const auto path = root / "textures" / style / name;
        std::vector<std::byte> probe = read_rgba_file(path, expected);
        if (!probe.empty()) return path;
    }
    return {};
}

void blit_spacebar_cell(std::span<std::byte> rgba8, std::uint32_t width, std::uint32_t col,
                        std::uint32_t row) {
    const std::uint32_t cell = 16u;
    const std::uint32_t x0 = col * cell;
    const std::uint32_t y0 = row * cell;
    if ((x0 + cell) > width) return;
    const std::size_t need = static_cast<std::size_t>(width) * (y0 + cell) * 4u;
    if (rgba8.size() < need) return;
    auto *base = reinterpret_cast<std::uint8_t *>(rgba8.data());
    for (std::uint32_t y = 0; y < cell; ++y) {
        for (std::uint32_t x = 0; x < cell; ++x) {
            std::uint8_t *px = base + ((y0 + y) * width + (x0 + x)) * 4u;
            px[0] = px[1] = px[2] = px[3] = 0;
        }
    }
    const int y_top = 7;
    const int y_bot = 11;
    for (int y = y_top; y <= y_bot; ++y) {
        for (int x = 1; x <= 14; ++x) {
            const bool edge = y == y_top || y == y_bot || x == 1 || x == 14;
            if (!edge) continue;
            std::uint8_t *px = base + ((y0 + static_cast<std::uint32_t>(y)) * width +
                                       (x0 + static_cast<std::uint32_t>(x))) *
                                          4u;
            px[0] = px[1] = px[2] = px[3] = 255;
        }
    }
}

void patch_kb_letter_r(std::uint64_t signature, std::uint32_t width, std::uint32_t height,
                       std::span<std::byte> rgba8) {
    if (width != 256u || height != 256u) return;
    // 5ff05f6c2afa636b and 37bf52ac7d5c44c4 HUD fonts.
    if (signature != 0x5ff05f6c2afa636bull && signature != 0x37bf52ac7d5c44c4ull) return;
    blit_spacebar_cell(rgba8, width, 2u, 3u);   // ASCII 'R'
    if (signature == 0x37bf52ac7d5c44c4ull)
        blit_spacebar_cell(rgba8, width, 5u, 15u);  // last-row R button
}

// ---------------------------------------------------------------------------
// "switch" style: Nintendo Switch face buttons painted over the PSP glyphs as
// each texture is decoded, so it needs no files from the disc. The game's own
// glyph is measured first (disk size, ring, fill and symbol colours) and the
// new one keeps those, so it sits in the HUD like the original.
//
// By position, as the controls map them: Cross (bottom) = B, Circle (right) =
// A, Square (left) = Y, Triangle (top) = X. The pause-menu chips show what
// confirms and cancels there, so with Switch.ConfirmButton=A (Nintendo layout,
// the default) Cross shows A and Circle shows B.

namespace {

enum class PspFace { Cross, Circle, Square, Triangle };

struct SwitchPromptCell {
    std::uint64_t signature;
    std::uint32_t texture_size;
    std::uint32_t column;
    std::uint32_t row;
    PspFace face;
    bool menu_chip;
};

// Cells from tools/prompt_pack.py (ATLAS_CELLS, STANDALONE): two HUD font
// atlases with 16x16 glyphs on their last row, and the 16x16 pause-menu chips.
constexpr SwitchPromptCell kSwitchPromptCells[] = {
    {0x37bf52ac7d5c44c4ull, 256u, 0u, 15u, PspFace::Triangle, false},
    {0x37bf52ac7d5c44c4ull, 256u, 1u, 15u, PspFace::Square, false},
    {0x37bf52ac7d5c44c4ull, 256u, 2u, 15u, PspFace::Cross, false},
    {0x37bf52ac7d5c44c4ull, 256u, 3u, 15u, PspFace::Circle, false},
    {0x5ff05f6c2afa636bull, 256u, 0u, 15u, PspFace::Triangle, false},
    {0x5ff05f6c2afa636bull, 256u, 1u, 15u, PspFace::Circle, false},
    {0x5ff05f6c2afa636bull, 256u, 2u, 15u, PspFace::Cross, false},
    {0x5ff05f6c2afa636bull, 256u, 3u, 15u, PspFace::Square, false},
    {0x48e344e67bdb8e5dull, 16u, 0u, 0u, PspFace::Circle, true},
    {0x76678d09c8dbc3d3ull, 16u, 0u, 0u, PspFace::Cross, true},
};

// 7x8 letters with two-pixel strokes, the weight of the game's symbols.
constexpr std::array<const char *, 8> kLetterA = {
    "..###..", ".##.##.", "##...##", "##...##", "#######", "##...##", "##...##", "##...##"};
constexpr std::array<const char *, 8> kLetterB = {
    "######.", "##...##", "##...##", "######.", "##...##", "##...##", "##...##", "######."};
constexpr std::array<const char *, 8> kLetterX = {
    "##...##", "##...##", ".##.##.", "..###..", "..###..", ".##.##.", "##...##", "##...##"};
constexpr std::array<const char *, 8> kLetterY = {
    "##...##", "##...##", ".##.##.", "..###..", "..###..", "..###..", "..###..", "..###.."};

char switch_letter(PspFace face, bool menu_chip) {
    const bool nintendo = menu_chip && lcs_render_configuration().nx.nintendo_confirm;
    switch (face) {
    case PspFace::Cross: return nintendo ? 'A' : 'B';
    case PspFace::Circle: return nintendo ? 'B' : 'A';
    case PspFace::Square: return 'Y';
    case PspFace::Triangle: return 'X';
    }
    return 'A';
}

const std::array<const char *, 8> &letter_rows(char letter) {
    switch (letter) {
    case 'A': return kLetterA;
    case 'B': return kLetterB;
    case 'X': return kLetterX;
    default: return kLetterY;
    }
}

struct Rgba {
    float r{}, g{}, b{}, a{};
};

struct GlyphStyle {
    float center_x{7.5f};
    float center_y{7.5f};
    float radius{7.0f};
    Rgba fill{40.0f, 40.0f, 44.0f, 255.0f};
    Rgba ring{200.0f, 200.0f, 200.0f, 255.0f};
    Rgba ink{240.0f, 240.0f, 240.0f, 255.0f};
    bool white_mask{false};  // white pixels shaped by alpha, tinted by the text colour
};

Rgba median_of(std::vector<Rgba> pixels, float Rgba::*channel_for_sort) {
    if (pixels.empty()) return {};
    std::sort(pixels.begin(), pixels.end(), [&](const Rgba &x, const Rgba &y) {
        return x.*channel_for_sort < y.*channel_for_sort;
    });
    return pixels[pixels.size() / 2u];
}

float luminance(const Rgba &c) { return (c.r * 0.299f + c.g * 0.587f + c.b * 0.114f) * (c.a / 255.0f); }

GlyphStyle measure_glyph(const std::uint8_t *cell, std::uint32_t stride) {
    GlyphStyle style;
    std::vector<Rgba> opaque;
    float sum_x = 0.0f, sum_y = 0.0f;
    std::uint32_t whites = 0u;
    for (std::uint32_t y = 0u; y < 16u; ++y) {
        for (std::uint32_t x = 0u; x < 16u; ++x) {
            const std::uint8_t *p = cell + (static_cast<std::size_t>(y) * stride + x) * 4u;
            if (p[3] < 64u) continue;
            opaque.push_back({float(p[0]), float(p[1]), float(p[2]), float(p[3])});
            sum_x += float(x) + 0.5f;
            sum_y += float(y) + 0.5f;
            if (p[0] > 225u && p[1] > 225u && p[2] > 225u) ++whites;
        }
    }
    if (opaque.size() < 24u) return style;  // nothing recognisable: defaults
    const float count = float(opaque.size());
    style.center_x = std::clamp(sum_x / count, 6.0f, 10.0f);
    style.center_y = std::clamp(sum_y / count, 6.0f, 10.0f);
    style.white_mask = whites * 10u >= opaque.size() * 9u;
    // The original's extent, from its farthest opaque pixels.
    float extent = 0.0f;
    for (std::uint32_t y = 0u; y < 16u; ++y)
        for (std::uint32_t x = 0u; x < 16u; ++x)
            if (cell[(static_cast<std::size_t>(y) * stride + x) * 4u + 3u] >= 64u)
                extent = std::max(extent, std::hypot(float(x) + 0.5f - style.center_x,
                                                     float(y) + 0.5f - style.center_y));
    style.radius = std::clamp(extent, 5.5f, 7.75f);
    if (style.white_mask) {
        Rgba alpha_max{255.0f, 255.0f, 255.0f, 0.0f};
        for (const Rgba &c : opaque) alpha_max.a = std::max(alpha_max.a, c.a);
        style.fill = alpha_max;
        return style;
    }
    // Ring: the outermost pixels. Fill and ink: the darker and brighter inside.
    std::vector<Rgba> ring, inside;
    for (std::uint32_t y = 0u; y < 16u; ++y) {
        for (std::uint32_t x = 0u; x < 16u; ++x) {
            const std::uint8_t *p = cell + (static_cast<std::size_t>(y) * stride + x) * 4u;
            if (p[3] < 64u) continue;
            const float d = std::hypot(float(x) + 0.5f - style.center_x, float(y) + 0.5f - style.center_y);
            const Rgba c{float(p[0]), float(p[1]), float(p[2]), float(p[3])};
            if (d >= style.radius - 1.5f) ring.push_back(c);
            else if (d <= style.radius - 2.5f) inside.push_back(c);
        }
    }
    if (!ring.empty()) style.ring = median_of(ring, &Rgba::g);
    if (inside.size() >= 8u) {
        std::sort(inside.begin(), inside.end(),
                  [](const Rgba &x, const Rgba &y) { return luminance(x) < luminance(y); });
        style.fill = inside[inside.size() / 5u];
        Rgba bright = inside[inside.size() - 1u - inside.size() / 8u];
        // The PSP symbols are coloured; Switch letters are neutral.
        const float grey = std::max({bright.r, bright.g, bright.b});
        style.ink = {grey, grey, grey, bright.a};
    }
    if (luminance(style.ink) < luminance(style.fill) + 60.0f)
        style.ink = {235.0f, 235.0f, 235.0f, 255.0f};
    return style;
}

void blend_pixel(std::uint8_t *p, const Rgba &c, float coverage) {
    if (coverage <= 0.0f) return;
    coverage = std::min(coverage, 1.0f);
    const float a = (c.a / 255.0f) * coverage;
    const float dst_a = p[3] / 255.0f;
    const float out_a = a + dst_a * (1.0f - a);
    if (out_a <= 0.0f) return;
    const auto mix = [&](float src, std::uint8_t dst) {
        return static_cast<std::uint8_t>(std::clamp(
            (src * a + float(dst) * dst_a * (1.0f - a)) / out_a + 0.5f, 0.0f, 255.0f));
    };
    p[0] = mix(c.r, p[0]);
    p[1] = mix(c.g, p[1]);
    p[2] = mix(c.b, p[2]);
    p[3] = static_cast<std::uint8_t>(std::clamp(out_a * 255.0f + 0.5f, 0.0f, 255.0f));
}

void paint_switch_glyph(std::uint8_t *cell, std::uint32_t stride, char letter) {
    const GlyphStyle style = measure_glyph(cell, stride);
    for (std::uint32_t y = 0u; y < 16u; ++y)
        std::memset(cell + static_cast<std::size_t>(y) * stride * 4u, 0, 16u * 4u);
    // Disk and ring, 4x4 supersampled.
    for (std::uint32_t y = 0u; y < 16u; ++y) {
        for (std::uint32_t x = 0u; x < 16u; ++x) {
            float disk = 0.0f, ring = 0.0f;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float d = std::hypot(float(x) + (float(sx) + 0.5f) / 4.0f - style.center_x,
                                               float(y) + (float(sy) + 0.5f) / 4.0f - style.center_y);
                    if (d <= style.radius) {
                        disk += 1.0f / 16.0f;
                        if (d >= style.radius - 1.25f) ring += 1.0f / 16.0f;
                    }
                }
            }
            std::uint8_t *p = cell + (static_cast<std::size_t>(y) * stride + x) * 4u;
            blend_pixel(p, style.fill, disk);
            if (!style.white_mask) blend_pixel(p, style.ring, ring);
        }
    }
    // The letter, centred on the disk.
    const auto &rows = letter_rows(letter);
    const int left = static_cast<int>(std::lround(style.center_x - 3.5f));
    const int top = static_cast<int>(std::lround(style.center_y - 4.0f));
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 7; ++column) {
            if (rows[static_cast<std::size_t>(row)][column] != '#') continue;
            const int x = left + column;
            const int y = top + row;
            if (x < 0 || y < 0 || x >= 16 || y >= 16) continue;
            std::uint8_t *p = cell + (static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)) * 4u;
            if (style.white_mask) {
                p[3] = 0u;  // cut out, as the system font draws its button glyphs
            } else {
                p[0] = static_cast<std::uint8_t>(style.ink.r);
                p[1] = static_cast<std::uint8_t>(style.ink.g);
                p[2] = static_cast<std::uint8_t>(style.ink.b);
                p[3] = static_cast<std::uint8_t>(style.ink.a);
            }
        }
    }
}

bool paint_switch_prompts(std::uint64_t signature, std::uint32_t width, std::uint32_t height,
                          std::span<std::byte> rgba8) {
    bool painted = false;
    for (const SwitchPromptCell &cell : kSwitchPromptCells) {
        if (cell.signature != signature || width != cell.texture_size || height != cell.texture_size)
            continue;
        const std::size_t x0 = cell.column * 16u;
        const std::size_t y0 = cell.row * 16u;
        if ((y0 + 16u) * width * 4u > rgba8.size() || x0 + 16u > width) continue;
        auto *base = reinterpret_cast<std::uint8_t *>(rgba8.data()) + (y0 * width + x0) * 4u;
        paint_switch_glyph(base, width, switch_letter(cell.face, cell.menu_chip));
        painted = true;
    }
    return painted;
}

bool is_switch_style(const std::string &style) {
    return style == "switch" || style == "nintendo" || style == "nx";
}

}  // namespace

std::uint32_t prompt_texture_variant(std::uint64_t signature) {
    if (signature == 0u || !is_switch_style(prompt_texture_style())) return 0u;
    for (const SwitchPromptCell &cell : kSwitchPromptCells)
        if (cell.signature == signature)
            return cell.menu_chip && lcs_render_configuration().nx.nintendo_confirm ? 2u : 1u;
    return 0u;
}

void prompt_texture_apply(std::uint64_t signature, std::uint32_t width, std::uint32_t height,
                          std::uint32_t mip_levels, std::span<std::byte> rgba8) {
    if (signature == 0u || width == 0u || height == 0u || rgba8.empty()) return;
    const std::string &style = prompt_texture_style();
    if (style.empty()) return;
    if (is_switch_style(style)) {
        if (paint_switch_prompts(signature, width, height, rgba8) && mip_levels > 1u &&
            rgba8.size() >= chain_bytes(width, height, mip_levels))
            rebuild_mip_chain(rgba8, width, height, mip_levels);
        return;
    }
    OverrideCache &cache = override_cache();
    std::vector<std::byte> pixels;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto found = cache.loaded.find(signature);
        if (found != cache.loaded.end()) {
            pixels = found->second;
        } else {
            const auto path = find_override_file(style, signature, width, height);
            if (!path.empty()) {
                pixels = read_rgba_file(path, static_cast<std::size_t>(width) * height * 4u);
                if (!pixels.empty()) cache.loaded.insert({signature, pixels});
            }
        }
    }
    const std::size_t mip0_bytes = static_cast<std::size_t>(width) * height * 4u;
    if (!pixels.empty()) {
        const std::size_t copy_bytes = std::min(mip0_bytes, rgba8.size());
        std::memcpy(rgba8.data(), pixels.data(), copy_bytes);
    }
    if (style == "kb") patch_kb_letter_r(signature, width, height, rgba8);
    if (mip_levels > 1u && rgba8.size() >= chain_bytes(width, height, mip_levels))
        rebuild_mip_chain(rgba8, width, height, mip_levels);
}

void prompt_texture_dump(std::uint64_t signature, std::uint32_t width, std::uint32_t height,
                         std::uint32_t address, std::uint32_t format, bool screen_space,
                         std::span<const std::byte> rgba8) {
    DumpState &state = dump_state();
    if (!state.enabled || signature == 0u || width == 0u || height == 0u) return;
    if (state.screen_space_only && !screen_space) return;
    const std::size_t mip0_bytes = static_cast<std::size_t>(width) * height * 4u;
    if (rgba8.size() < mip0_bytes) return;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.seen.insert(signature).second) return;
    const std::string name = signature_name(signature) + "_" + std::to_string(width) + "x" +
                             std::to_string(height);
    std::ofstream image(state.directory / (name + ".rgba"), std::ios::binary);
    if (image)
        image.write(reinterpret_cast<const char *>(rgba8.data()),
                    static_cast<std::streamsize>(mip0_bytes));
    std::ofstream manifest(state.directory / "manifest.txt", std::ios::app);
    if (manifest)
        manifest << name << " address=0x" << std::hex << address << std::dec
                 << " format=" << format << '\n';
}

}
