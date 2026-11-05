// OpenGL GE backend check (the Switch's renderer), run on a desktop.
//
// Feeds synthetic GE draws to the OpenGL backend, reads the frame back and
// checks pixels: orientation, scissor, textures, blending, depth, culling and
// render-to-texture. The expected colors are what the Vulkan backend renders
// for the same draws (compared pixel for pixel with Mesa's lavapipe and
// llvmpipe when this test was written).
//
// Needs a display with OpenGL 3.3 (Mesa's llvmpipe under Xvfb is enough):
//   cmake -DLCS_GE_OPENGL=ON ... && xvfb-run -a ./lcs_ge_gl_backend_test
// Exit 0 pass, 1 fail, 77 skipped (no OpenGL display).
#include "ge_gpu_backend.hpp"
#include "lcs_render_config.hpp"

#include <SDL.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace lcs;

namespace {

constexpr std::uint32_t kDisplay = 0x04088000u;

GeGpuDrawDescriptor base_draw() {
    GeGpuDrawDescriptor d{};
    d.framebuffer_address = kDisplay;
    d.framebuffer_stride = 512u;
    d.framebuffer_format = 3u;
    d.scissor_x0 = 0;
    d.scissor_y0 = 0;
    d.scissor_x1 = 479;
    d.scissor_y1 = 271;
    d.color_write_mask = 0u;
    return d;
}

GeGpuVertex v(float x, float y, float z, std::uint32_t rgba, float u = 0.0f, float vv = 0.0f) {
    GeGpuVertex out{};
    out.x = x;
    out.y = y;
    out.z = z;
    out.w = 1.0f;
    out.rgba = rgba;
    out.u = u;
    out.v = vv;
    return out;
}

void quad(std::vector<GeGpuVertex> &out, float x0, float y0, float x1, float y1, float z, std::uint32_t rgba,
          float u0 = 0, float v0 = 0, float u1 = 0, float v1 = 0) {
    out.push_back(v(x0, y0, z, rgba, u0, v0));
    out.push_back(v(x1, y0, z, rgba, u1, v0));
    out.push_back(v(x0, y1, z, rgba, u0, v1));
    out.push_back(v(x1, y0, z, rgba, u1, v0));
    out.push_back(v(x1, y1, z, rgba, u1, v1));
    out.push_back(v(x0, y1, z, rgba, u0, v1));
}

GeGpuHardwareTransform hw_identity(bool cull, bool ccw) {
    GeGpuHardwareTransform t{};
    t.model_to_clip = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    t.model_to_view_z = {0, 0, 1, 0};
    t.viewport_scale_x = 240.0f;
    t.viewport_scale_y = -136.0f;
    t.viewport_scale_z = 0.0f;
    t.viewport_center_x = 2048.0f;
    t.viewport_center_y = 2048.0f;
    t.viewport_center_z = 40000.0f;
    t.viewport_offset_x = 2048.0f - 240.0f;
    t.viewport_offset_y = 2048.0f - 136.0f;
    t.depth_clip_enabled = false;
    t.cull_enabled = cull;
    t.accept_counter_clockwise = ccw;
    t.primitive = 3u;
    return t;
}

bool run_frame(int frame, const char *out_dir) {
    ge_gpu_backend_set_display_framebuffer(kDisplay, 480u, 272u);
    // 1. Background quad, dark blue, and a red quad in the top-left corner.
    {
        GeGpuDrawDescriptor d = base_draw();
        ge_gpu_backend_record_draw(d);
        std::vector<GeGpuVertex> tri;
        quad(tri, 0, 0, 480, 272, 0, 0xFF400000u);
        quad(tri, 10, 10, 110, 60, 0, 0xFF0000FFu);
        ge_gpu_backend_accumulate_color_triangles(d, tri);
    }
    // 2. Scissored green quad: only x 200..239 of 150..300 shows.
    {
        GeGpuDrawDescriptor d = base_draw();
        d.scissor_x0 = 200;
        d.scissor_x1 = 239;
        std::vector<GeGpuVertex> tri;
        quad(tri, 150, 80, 300, 120, 0, 0xFF00FF00u);
        ge_gpu_backend_accumulate_color_triangles(d, tri);
    }
    // 3. Textured quad with a 2x2 texture (R, G / B, W), nearest, replace.
    {
        GeGpuDrawDescriptor d = base_draw();
        d.texture_enabled = true;
        d.texture_format = 3u;
        d.texture_address = 0x04100000u;
        d.texture_buffer_width = 2u;
        d.texture_width = 2u;
        d.texture_height = 2u;
        d.texture_function = 3u;
        d.texture_use_alpha = true;
        d.texture_clamp_u = d.texture_clamp_v = true;
        const std::uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        if (ge_gpu_backend_texture_needed(d))
            (void)ge_gpu_backend_upload_decoded_texture(
                d, 2u, 2u, std::span<const std::byte>(reinterpret_cast<const std::byte *>(texels), 16u));
        std::vector<GeGpuVertex> tri;
        quad(tri, 320, 10, 400, 90, 0, 0xFFFFFFFFu, 0, 0, 2, 2);
        ge_gpu_backend_accumulate_color_triangles(d, tri);
    }
    // 4. Alpha blend: 50% white over the background.
    {
        GeGpuDrawDescriptor d = base_draw();
        d.blend_enabled = true;
        d.blend_equation = 0u;
        d.blend_source_factor = 2u;
        d.blend_dest_factor = 3u;
        std::vector<GeGpuVertex> tri;
        quad(tri, 10, 150, 60, 200, 0, 0x80FFFFFFu);
        ge_gpu_backend_accumulate_color_triangles(d, tri);
    }
    // 5. Depth: far yellow quad drawn after a near magenta quad, GEQUAL test (PSP: bigger is nearer).
    {
        GeGpuDrawDescriptor d = base_draw();
        d.depth_test_enabled = true;
        d.depth_write_enabled = true;
        d.depth_function = 7u;
        std::vector<GeGpuVertex> tri;
        quad(tri, 100, 150, 180, 230, 50000, 0xFFFF00FFu);
        quad(tri, 140, 190, 220, 260, 10000, 0xFF00FFFFu);
        ge_gpu_backend_accumulate_color_triangles(d, tri);
    }
    // 6. Hardware transform, culling. Triangle A is counter-clockwise on screen
    //    (y up), B clockwise. Drawn with cull + accept_ccw, then cull + !accept_ccw.
    for (int pass = 0; pass < 2; ++pass) {
        GeGpuDrawDescriptor d = base_draw();
        const GeGpuHardwareTransform t = hw_identity(true, pass == 0);
        const float dx = pass == 0 ? 0.0f : 0.5f;
        std::vector<GeGpuVertex> vs = {
            v(0.10f + dx, -0.9f, 0, 0xFF00FFFFu), v(0.30f + dx, -0.9f, 0, 0xFF00FFFFu), v(0.20f + dx, -0.5f, 0, 0xFF00FFFFu),
            v(0.10f + dx, -0.4f, 0, 0xFFFFFF00u), v(0.20f + dx, -0.0f, 0, 0xFFFFFF00u), v(0.30f + dx, -0.4f, 0, 0xFFFFFF00u),
        };
        ge_gpu_backend_accumulate_hardware_triangles(d, t, vs, {});
    }
    // 7. Render to an off-screen target, then sample it on the display (feedback).
    {
        GeGpuDrawDescriptor off = base_draw();
        off.framebuffer_address = 0x04110000u;
        off.framebuffer_stride = 64u;
        off.scissor_x1 = 63;
        off.scissor_y1 = 63;
        ge_gpu_backend_record_draw(off);
        std::vector<GeGpuVertex> tri;
        quad(tri, 0, 0, 64, 32, 0, 0xFF0080FFu);
        quad(tri, 0, 32, 64, 64, 0, 0xFFFF8000u);
        ge_gpu_backend_accumulate_color_triangles(off, tri);

        GeGpuDrawDescriptor d = base_draw();
        d.texture_enabled = true;
        d.texture_format = 3u;
        d.texture_address = 0x04110000u;
        d.texture_buffer_width = 64u;
        d.texture_width = 64u;
        d.texture_height = 64u;
        d.texture_function = 3u;
        d.texture_use_alpha = true;
        d.texture_clamp_u = d.texture_clamp_v = true;
        std::vector<GeGpuVertex> shown;
        // Logical target 64x64 rendered at the full internal size, so sample its
        // whole surface: u, v in texels of the 64x64 texture, scaled to 480x272.
        quad(shown, 400, 150, 470, 260, 0, 0xFFFFFFFFu, 0, 0, 64.0f, 64.0f);
        ge_gpu_backend_accumulate_color_triangles(d, shown);
    }
    // 8. The packed 0115 path (u8 uv, 5551 colour, s16 position), indexed:
    //    a green quad at clip x 0.7..0.9, y 0.6..0.9 (screen 408..456, 14..54).
    {
        GeGpuDrawDescriptor d = base_draw();
        const GeGpuHardwareTransform t = hw_identity(false, false);
        const auto pack = [](float x, float y) {
            std::array<std::byte, 10> bytes{};
            const std::uint16_t color = 0x83E0u;  // alpha bit, green 31
            const auto px = static_cast<std::int16_t>(x * 32768.0f);
            const auto py = static_cast<std::int16_t>(y * 32768.0f);
            std::memcpy(bytes.data() + 2, &color, 2);
            std::memcpy(bytes.data() + 4, &px, 2);
            std::memcpy(bytes.data() + 6, &py, 2);
            return bytes;
        };
        std::vector<std::byte> packed;
        for (const auto &corner : {pack(0.7f, 0.6f), pack(0.9f, 0.6f), pack(0.7f, 0.9f), pack(0.9f, 0.9f)})
            packed.insert(packed.end(), corner.begin(), corner.end());
        const std::uint32_t indices[] = {0u, 1u, 2u, 2u, 1u, 3u};
        (void)ge_gpu_backend_accumulate_hardware_packed_0115(d, t, packed, 4u, indices);
    }
    const bool shown = ge_gpu_backend_finish_color_frame(static_cast<std::uint64_t>(frame));
    // The Vulkan readback lands at the next finish.
    (void)ge_gpu_backend_finish_color_frame(static_cast<std::uint64_t>(frame) + 1u);
    const GeGpuBackendReport report = ge_gpu_backend_report();
    const std::uint32_t w = report.offscreen_width;
    const std::uint32_t h = report.offscreen_height;
    std::vector<std::byte> rgba(static_cast<std::size_t>(w) * h * 4u);
    if (!ge_gpu_backend_copy_game_frame_rgba(rgba)) {
        std::fprintf(stderr, "no frame (shown=%d, %s)\n", shown ? 1 : 0, report.message.c_str());
        return false;
    }
    const std::string path = std::string(out_dir) + "/frame.ppm";
    FILE *file = std::fopen(path.c_str(), "wb");
    std::fprintf(file, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
        std::fwrite(reinterpret_cast<const char *>(rgba.data()) + i * 4u, 1, 3, file);
    std::fclose(file);
    std::printf("wrote %s (%ux%u) %s\n", path.c_str(), w, h, report.message.c_str());
    return true;
}

}  // namespace

struct Expect {
    const char *what;
    int x;
    int y;
    std::uint8_t r, g, b;
};

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "lcs_ge_gl_backend_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        FILE *ini = std::fopen((dir / "LCSNative.ini").string().c_str(), "w");
        if (ini == nullptr) return 1;
        std::fputs("[Rendering]\nBackend=OpenGL\nInternalResolutionMode=Scale\nInternalScale=1\n"
                   "[Widescreen]\nEnabled=false\n", ini);
        std::fclose(ini);
    }
    setenv("LCS_GL_READBACK", "1", 1);
    setenv("PSPRECOMP_CONFIG", (dir / "LCSNative.ini").string().c_str(), 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("skipped: SDL video unavailable (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Window *window = SDL_CreateWindow("lcs_ge_gl_backend_test", 0, 0, 480, 272,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        std::printf("skipped: no OpenGL window (%s)\n", SDL_GetError());
        return 77;
    }
    initialize_lcs_render_configuration(dir);
    ge_gpu_backend_set_native_window(window);
    std::string error;
    if (!initialize_ge_gpu_backend(error) || !ge_gpu_backend_active()) {
        std::printf("skipped: OpenGL GE unavailable (%s | %s)\n", error.c_str(),
                    ge_gpu_backend_report().message.c_str());
        return 77;
    }
    // Twice: the second frame is the steady state with every texture resident.
    // It runs on another thread, as the GE worker does in the game, so the
    // context moves between threads.
    ge_gpu_describe_next_frame().store(true);  // the stalled-frame log runs once
    bool ok = run_frame(1, dir.string().c_str());
    bool second = false;
    std::thread worker([&] { second = run_frame(3, dir.string().c_str()); });
    worker.join();
    ok = ok && second;
    std::vector<std::byte> rgba(480u * 272u * 4u);
    ok = ok && ge_gpu_backend_copy_game_frame_rgba(rgba);
    const Expect expects[] = {
        {"top-left quad (orientation)", 50, 30, 255, 0, 0},
        {"inside scissor", 220, 100, 0, 255, 0},
        {"outside scissor", 170, 100, 0, 0, 64},
        {"texel 0,0", 330, 20, 255, 0, 0},
        {"texel 1,0", 390, 20, 0, 255, 0},
        {"texel 0,1", 330, 80, 0, 0, 255},
        {"texel 1,1", 390, 80, 255, 255, 255},
        {"alpha blend", 30, 170, 128, 128, 160},
        {"depth: near quad", 150, 180, 255, 0, 255},
        {"depth: far quad hidden", 160, 210, 255, 0, 255},
        {"depth: far quad alone", 200, 250, 255, 255, 0},
        {"cull: kept triangle", 288, 226, 255, 255, 0},
        {"cull: culled triangle", 288, 160, 0, 0, 64},
        {"render target top", 435, 170, 255, 128, 0},
        {"render target bottom", 435, 240, 0, 128, 255},
        {"packed 0115 quad", 432, 34, 0, 255, 0},
    };
    int failures = 0;
    for (const Expect &e : expects) {
        if (!ok) break;
        const std::size_t at = (static_cast<std::size_t>(e.y) * 480u + static_cast<std::size_t>(e.x)) * 4u;
        const int r = static_cast<int>(rgba[at]), g = static_cast<int>(rgba[at + 1u]),
                  b = static_cast<int>(rgba[at + 2u]);
        const bool match = std::abs(r - e.r) <= 2 && std::abs(g - e.g) <= 2 && std::abs(b - e.b) <= 2;
        if (!match) ++failures;
        std::printf("%s %-28s (%d,%d,%d) expected (%d,%d,%d)\n", match ? "ok  " : "FAIL", e.what, r, g, b,
                    e.r, e.g, e.b);
    }
    shutdown_ge_gpu_backend();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!ok) std::printf("FAIL: no frame was read back\n");
    return ok && failures == 0 ? 0 : 1;
}

namespace lcs {
void display_window_request_fullscreen(bool) noexcept {}
}
