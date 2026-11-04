// OpenGL GE backend. A port of ge_gpu_backend_vulkan.cpp to OpenGL 3.3 core,
// written for the Nintendo Switch (Mesa/nouveau through SDL2's EGL), where
// there is no Vulkan for homebrew. It builds on Linux too (LCS_GE_OPENGL=ON)
// so it can be checked on a desktop.
//
// Same model as the Vulkan backend: draws are recorded during a GE list, and
// ge_gpu_backend_finish_color_frame replays them into one render target per
// PSP framebuffer address, then shows the display target in the window.
//
// Orientation: like the Vulkan backend the vertex shader negates clip Y, so in
// every render target row 0 is the PSP's top line. Scissors and readbacks
// therefore use the PSP's top-down rows directly; only the final present to
// the window flips. The winding test is the mirror of Vulkan's (Vulkan's
// polygon area has the opposite sign), so front faces are swapped below.
//
// Threads: the GE worker records and finishes frames, and another thread may
// show a CPU frame (videos). The context is made current for each GL scope
// and released at its end, under the backend mutex.

#include "ge_gpu_backend.hpp"

#if defined(LCS_GL_GE_BACKEND)

#include "lcs_display_menu.hpp"
#include "lcs_prompt_textures.hpp"
#include "lcs_render_config.hpp"
#include "lcs_runtime_log.hpp"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

namespace lcs {
namespace {

// ---------------------------------------------------------------------------
// GL entry points, loaded through SDL so nothing links against libGL.

#define LCS_GL_FUNCTIONS(X)                                                   \
    X(void, Enable, (GLenum))                                                 \
    X(void, Disable, (GLenum))                                                \
    X(GLenum, GetError, ())                                                   \
    X(void, GetIntegerv, (GLenum, GLint *))                                   \
    X(void, GetFloatv, (GLenum, GLfloat *))                                   \
    X(const GLubyte *, GetString, (GLenum))                                   \
    X(const GLubyte *, GetStringi, (GLenum, GLuint))                          \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                       \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                        \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                 \
    X(void, ClearDepth, (GLdouble))                                           \
    X(void, Clear, (GLbitfield))                                              \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))          \
    X(void, DepthMask, (GLboolean))                                           \
    X(void, DepthFunc, (GLenum))                                              \
    X(void, CullFace, (GLenum))                                               \
    X(void, FrontFace, (GLenum))                                              \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))              \
    X(void, BlendEquation, (GLenum))                                          \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))                 \
    X(void, PixelStorei, (GLenum, GLint))                                     \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, Flush, ())                                                        \
    X(void, Finish, ())                                                       \
    X(void, GenTextures, (GLsizei, GLuint *))                                 \
    X(void, DeleteTextures, (GLsizei, const GLuint *))                        \
    X(void, BindTexture, (GLenum, GLuint))                                    \
    X(void, ActiveTexture, (GLenum))                                          \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                           \
    X(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(void, GenSamplers, (GLsizei, GLuint *))                                 \
    X(void, DeleteSamplers, (GLsizei, const GLuint *))                        \
    X(void, BindSampler, (GLuint, GLuint))                                    \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint))                       \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat))                     \
    X(void, GenBuffers, (GLsizei, GLuint *))                                  \
    X(void, DeleteBuffers, (GLsizei, const GLuint *))                         \
    X(void, BindBuffer, (GLenum, GLuint))                                     \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))  \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum))           \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))      \
    X(void, GenVertexArrays, (GLsizei, GLuint *))                             \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint *))                    \
    X(void, BindVertexArray, (GLuint))                                        \
    X(void, EnableVertexAttribArray, (GLuint))                                \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                             \
    X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *))                             \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *))                    \
    X(void, BindFramebuffer, (GLenum, GLuint))                                \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))    \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))        \
    X(GLenum, CheckFramebufferStatus, (GLenum))                               \
    X(void, GenRenderbuffers, (GLsizei, GLuint *))                            \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint *))                   \
    X(void, BindRenderbuffer, (GLenum, GLuint))                               \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))          \
    X(GLuint, CreateShader, (GLenum))                                         \
    X(void, DeleteShader, (GLuint))                                           \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint))                                          \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *))                           \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))         \
    X(GLuint, CreateProgram, ())                                              \
    X(void, DeleteProgram, (GLuint))                                          \
    X(void, AttachShader, (GLuint, GLuint))                                   \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *))             \
    X(void, BindFragDataLocation, (GLuint, GLuint, const GLchar *))           \
    X(void, LinkProgram, (GLuint))                                            \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *))                          \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))        \
    X(void, UseProgram, (GLuint))                                             \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *))                    \
    X(void, Uniform1i, (GLint, GLint))                                        \
    X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar *))                 \
    X(void, UniformBlockBinding, (GLuint, GLuint, GLuint))

struct GlApi {
#define LCS_GL_DECLARE(ret, name, args) ret(APIENTRY *name) args = nullptr;
    LCS_GL_FUNCTIONS(LCS_GL_DECLARE)
#undef LCS_GL_DECLARE
};

GlApi gl;

bool load_gl(std::string &error) {
    bool ok = true;
#define LCS_GL_LOAD(ret, name, args)                                                     \
    gl.name = reinterpret_cast<ret(APIENTRY *) args>(SDL_GL_GetProcAddress("gl" #name)); \
    if (gl.name == nullptr) {                                                            \
        if (ok) error = "missing GL entry point gl" #name;                               \
        ok = false;                                                                      \
    }
    LCS_GL_FUNCTIONS(LCS_GL_LOAD)
#undef LCS_GL_LOAD
    return ok;
}

// ---------------------------------------------------------------------------

constexpr std::uint32_t kReferenceWidth = 480u;
constexpr std::uint32_t kReferenceHeight = 272u;
constexpr std::size_t kGeometryUploadCapacity = 64u * 1024u * 1024u;
constexpr std::uint32_t kMaxDraws = 32768u;
constexpr std::size_t kMaxTextures = 4096u;

struct UploadVertex {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
    std::uint32_t rgba{0xFFFFFFFFu};
    float u{};
    float v{};
    float fog_factor{1.0f};
    float q{1.0f};
};
static_assert(sizeof(UploadVertex) == 36u);

struct DrawUniforms {
    float row0[4]{};
    float row1[4]{};
    float row2[4]{};
    float row3[4]{};
    float view_z[4]{};
    float uv[4]{1.0f, 1.0f, 0.0f, 0.0f};
    float fog[4]{};
    std::uint32_t control[4]{};
    float color_mul[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float color_add[4]{};
    std::uint32_t pixel0[4]{};
    std::uint32_t pixel1[4]{};
};
static_assert(sizeof(DrawUniforms) == 192u);

#pragma pack(push, 1)
struct Packed0115 {
    std::uint8_t u{};
    std::uint8_t v{};
    std::uint16_t color{};
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
};
#pragma pack(pop)
static_assert(sizeof(Packed0115) == 10u);

struct Batch {
    GeGpuDrawDescriptor draw{};
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    bool indexed{};
    std::uint32_t logical_draw_count{1u};
    bool hardware_transform{};
    GeGpuHardwareTransform transform{};
    bool framebuffer_feedback{};
    std::uint32_t feedback_address{};
};

struct Target {
    std::uint32_t address{};
    std::uint32_t logical_width{kReferenceWidth};
    std::uint32_t logical_height{kReferenceHeight};
    GLuint framebuffer{};
    GLuint color{};
    GLuint depth{};
    GLuint snapshot{};
    std::uint64_t last_render_epoch{};
};

struct Texture {
    GeGpuDrawDescriptor descriptor{};
    GLuint texture{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t allocated_width{};
    std::uint32_t allocated_height{};
    std::uint32_t allocated_mips{};
    std::vector<std::byte> rgba8;
    std::uint32_t mip_levels{1u};
    bool dirty{};
    std::uint64_t signature_epoch{};
    std::uint64_t last_used_epoch{};
    std::uint32_t prompt_variant{};
};

// What a batch draws with, decided before any GL call of the frame so all the
// uniform blocks go up in one upload.
enum class SourceKind : std::uint8_t { White, Texture, TargetColor, TargetSnapshot };

struct PlannedDraw {
    Target *target{};
    SourceKind source{SourceKind::White};
    GLuint texture{};
    Target *source_target{};
    bool textured{};
};

struct CachedState {
    bool valid{};
    bool depth_test{};
    bool depth_write{};
    GLenum depth_func{};
    std::size_t blend{~std::size_t{0}};
    std::uint32_t color_mask{~0u};
    bool cull{};
    GLenum front_face{};
    std::array<float, 4> blend_color{-1.0f, -1.0f, -1.0f, -1.0f};
    GLuint texture{~0u};
    GLuint sampler{~0u};
    std::array<GLint, 4> scissor{-1, -1, -1, -1};
};

struct GlState {
    std::recursive_mutex mutex;
    GeGpuBackendReport report{};
    bool enabled{};
    SDL_Window *window{};
    SDL_GLContext context{};
    GLuint program{};
    GLuint present_program{};
    GLint present_opaque_location{-1};
    GLint present_flip_location{-1};
    GLuint vao{};
    GLuint present_vao{};
    GLuint vertex_buffer{};
    GLuint index_buffer{};
    GLuint uniform_buffer{};
    GLuint white{};
    GLuint white_sampler{};
    GLuint present_linear{};
    GLuint present_nearest{};
    GLuint fps_overlay{};
    GLuint settings_overlay{};
    GLuint guest_image{};
    std::uint32_t guest_width{};
    std::uint32_t guest_height{};
    std::vector<std::uint8_t> overlay_pixels;
    std::uint32_t uniform_align{256u};
    float max_lod_bias{2.0f};
    float max_anisotropy{1.0f};
    bool anisotropy{};
    bool invert_winding{};
    bool readback_mode{};
    bool vsync{};
    std::unordered_map<std::uint64_t, GLuint> samplers{};
    std::unordered_map<std::uint64_t, Texture> textures{};
    std::unordered_map<std::uint32_t, Target> targets{};
    std::vector<UploadVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<PlannedDraw> plan;
    std::vector<std::byte> uniform_bytes;
    std::vector<std::byte> frame_rgba;
    std::vector<std::byte> last_texture_rgba;
    std::uint32_t target_width{kReferenceWidth};
    std::uint32_t target_height{kReferenceHeight};
    std::uint32_t display_framebuffer{};
    std::uint32_t display_logical_width{kReferenceWidth};
    std::uint32_t display_logical_height{kReferenceHeight};
    std::uint64_t frame_epoch{1u};
    Target *bound_target{};
    CachedState cache{};
    bool direct_present_ok{};
    std::uint64_t gl_errors_logged{};
};

GlState &state() {
    static GlState s;
    return s;
}

// Makes the context current on this thread for one scope.
class ContextScope {
public:
    explicit ContextScope(GlState &s) noexcept : s_(s) {
        ok_ = s.window != nullptr && s.context != nullptr &&
              SDL_GL_MakeCurrent(s.window, s.context) == 0;
    }
    ~ContextScope() {
        if (ok_) SDL_GL_MakeCurrent(s_.window, nullptr);
        s_.bound_target = nullptr;
        s_.cache = CachedState{};
    }
    ContextScope(const ContextScope &) = delete;
    ContextScope &operator=(const ContextScope &) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    GlState &s_;
    bool ok_{};
};

void check_gl(GlState &s, const char *where) {
    const GLenum code = gl.GetError();
    if (code == GL_NO_ERROR) return;
    if (s.gl_errors_logged < 32u) {
        ++s.gl_errors_logged;
        std::cerr << "[gl] error 0x" << std::hex << code << std::dec << " at " << where << "\n";
    }
}

// ---------------------------------------------------------------------------
// Shaders. Same as host/vulkan/ge.vert and ge.frag, in GLSL 3.30. The only
// change in the math: GL clip depth runs -w..w, so z becomes 2z - w at the end
// (the Vulkan path clips and stores z in 0..w).

constexpr const char *kUniformBlock = R"(
layout(std140) uniform DrawUniforms {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 row3;
    vec4 viewZ;
    vec4 uvScaleOffset;
    vec4 fogParameters;
    uvec4 control;
    vec4 colorMul;
    vec4 colorAdd;
    uvec4 pixel0;
    uvec4 pixel1;
} u;
)";

constexpr const char *kVertexShader = R"(
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inQ;
layout(location = 4) in float inFog;
out vec4 vColor;
out vec2 vUv;
out float vQ;
out float vFog;

void main() {
    vec4 position;
    vec2 uv;
    float fog;
    if (u.control.x == 1u) {
        vec4 p = inPosition;
        float clipW = dot(u.row3, p);
        if (abs(clipW) < 1.0e-12) clipW = 1.0;
        float clipZ = dot(u.row2, p);
        if (u.control.y == 0u) clipZ = clamp(clipZ, 0.0, clipW);
        position = vec4(dot(u.row0, p), dot(u.row1, p), clipZ, clipW);
        uv = inUv * u.uvScaleOffset.xy + u.uvScaleOffset.zw;
        float viewZ = dot(u.viewZ, p);
        fog = clamp((viewZ + u.fogParameters.x) * u.fogParameters.y, 0.0, 1.0);
    } else if (u.control.x == 2u) {
        float clipW = inPosition.w;
        if (abs(clipW) < 1.0e-12) clipW = 1.0;
        position = vec4(
            (inPosition.x * u.uvScaleOffset.x - 1.0) * clipW,
            (1.0 - inPosition.y * u.uvScaleOffset.y) * clipW,
            clamp(inPosition.z * u.uvScaleOffset.z, 0.0, 1.0) * clipW,
            clipW);
        uv = inUv;
        fog = inFog;
    } else {
        position = inPosition;
        uv = inUv;
        fog = inFog;
    }
    vec4 color = inColor;
    if (u.control.z != 0u) {
        vec4 lit = clamp(color * u.colorMul + u.colorAdd, 0.0, 1.0);
        color = floor(lit * 255.0) * (1.0 / 255.0);
    }
    position.y = -position.y;
    position.z = position.z * 2.0 - position.w;
    gl_Position = position;
    vColor = color;
    vUv = uv;
    vQ = inQ;
    vFog = fog;
}
)";

constexpr const char *kFragmentShader = R"(
in vec4 vColor;
in vec2 vUv;
in float vQ;
in float vFog;
out vec4 outColor;
uniform sampler2D sourceTexture;

bool alphaPass(uint fn, uint lhs, uint rhs) {
    switch (fn & 7u) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return lhs == rhs;
    case 3u: return lhs != rhs;
    case 4u: return lhs < rhs;
    case 5u: return lhs <= rhs;
    case 6u: return lhs > rhs;
    case 7u: return lhs >= rhs;
    }
    return true;
}

vec4 applyTextureFunction(vec4 vertex, vec4 texel, uvec4 control, uvec4 envBytes) {
    uint fn = control.x & 7u;
    bool useAlpha = control.y != 0u;
    bool doubleColor = control.z != 0u;
    vec4 result = vertex;
    vec3 env = vec3(envBytes.xyz) / 255.0;
    if (fn == 0u) {
        result.rgb = vertex.rgb * texel.rgb;
        result.a = useAlpha ? vertex.a * texel.a : vertex.a;
    } else if (fn == 1u) {
        float a = useAlpha ? texel.a : 1.0;
        result.rgb = mix(vertex.rgb, texel.rgb, a);
        result.a = vertex.a;
    } else if (fn == 2u) {
        result.rgb = mix(vertex.rgb, env, texel.rgb);
        result.a = useAlpha ? vertex.a * texel.a : vertex.a;
    } else if (fn == 3u) {
        result = texel;
        if (!useAlpha) result.a = vertex.a;
    } else if (fn == 4u) {
        result.rgb = clamp(vertex.rgb + texel.rgb, 0.0, 1.0);
        result.a = useAlpha ? vertex.a * texel.a : vertex.a;
    }
    if (doubleColor) result.rgb = clamp(result.rgb * 2.0, 0.0, 1.0);
    return result;
}

float quantize(float value, float levels) {
    return floor(clamp(value, 0.0, 1.0) * levels + 0.5) / levels;
}

vec4 quantizeFramebuffer(vec4 color, uint format) {
    color = clamp(color, 0.0, 1.0);
    uint kind = format & 3u;
    if (kind == 0u) {
        color.r = quantize(color.r, 31.0);
        color.g = quantize(color.g, 63.0);
        color.b = quantize(color.b, 31.0);
        color.a = 1.0;
    } else if (kind == 1u) {
        color.r = quantize(color.r, 31.0);
        color.g = quantize(color.g, 31.0);
        color.b = quantize(color.b, 31.0);
        color.a = color.a >= 0.5 ? 1.0 : 0.0;
    } else if (kind == 2u) {
        color.r = quantize(color.r, 15.0);
        color.g = quantize(color.g, 15.0);
        color.b = quantize(color.b, 15.0);
        color.a = quantize(color.a, 15.0);
    }
    return color;
}

void main() {
    uvec4 textureControl = uvec4(u.pixel0.y & 0xFFu, (u.pixel0.y >> 8u) & 0xFFu,
                                 (u.pixel0.y >> 16u) & 0xFFu, (u.pixel0.y >> 24u) & 0xFFu);
    uvec4 textureEnv = uvec4(u.pixel0.z & 0xFFu, (u.pixel0.z >> 8u) & 0xFFu,
                             (u.pixel0.z >> 16u) & 0xFFu, 0u);
    uvec4 fogControl = uvec4(u.pixel0.w & 0xFFu, (u.pixel0.w >> 8u) & 0xFFu,
                             (u.pixel0.w >> 16u) & 0xFFu, (u.pixel0.w >> 24u) & 0xFFu);
    uvec4 alphaControl = uvec4(u.pixel0.x & 0xFFu, (u.pixel0.x >> 8u) & 0xFFu,
                               (u.pixel0.x >> 16u) & 0xFFu, (u.pixel0.x >> 24u) & 0xFFu);
    vec4 color = clamp(vColor, 0.0, 1.0);
    if (textureControl.w != 0u) {
        float q = abs(vQ) < 1.0e-20 ? 1.0 : vQ;
        vec4 texel = texture(sourceTexture, vUv / q);
        color = applyTextureFunction(color, texel, textureControl, textureEnv);
    }
    if (fogControl.w != 0u) {
        vec3 fog = vec3(fogControl.xyz) / 255.0;
        color.rgb = mix(fog, color.rgb, clamp(vFog, 0.0, 1.0));
    }
    if (alphaControl.x != 0u) {
        uint a = uint(floor(clamp(color.a, 0.0, 1.0) * 255.0 + 0.5));
        uint mask = alphaControl.w;
        if (!alphaPass(alphaControl.y, a & mask, alphaControl.z & mask)) discard;
    }
    outColor = quantizeFramebuffer(color, u.pixel1.x);
}
)";

// A full-screen triangle; v = 0 is the top of the image (row 0 of a target).
constexpr const char *kPresentVertexShader = R"(
out vec2 uv;
uniform int flipRows;
void main() {
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = pos;
    gl_Position = vec4(pos.x * 2.0 - 1.0, 1.0 - pos.y * 2.0, 0.0, 1.0);
    if (flipRows != 0) uv.y = 1.0 - uv.y;
}
)";

constexpr const char *kPresentFragmentShader = R"(
in vec2 uv;
out vec4 color;
uniform sampler2D image;
uniform int opaque;
void main() {
    vec4 texel = texture(image, uv);
    color = opaque != 0 ? vec4(texel.rgb, 1.0) : texel;
}
)";

GLuint compile_shader(GLenum stage, const std::string &source, std::string &error) {
    const GLuint shader = gl.CreateShader(stage);
    const char *text = source.c_str();
    gl.ShaderSource(shader, 1, &text, nullptr);
    gl.CompileShader(shader);
    GLint status = GL_FALSE;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048]{};
        gl.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        error = std::string(stage == GL_VERTEX_SHADER ? "vertex" : "fragment") +
                " shader failed to compile: " + log;
        gl.DeleteShader(shader);
        return 0u;
    }
    return shader;
}

GLuint link_program(const std::string &vertex_source, const std::string &fragment_source,
                    bool ge_attributes, std::string &error) {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source, error);
    if (vertex == 0u) return 0u;
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source, error);
    if (fragment == 0u) {
        gl.DeleteShader(vertex);
        return 0u;
    }
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vertex);
    gl.AttachShader(program, fragment);
    if (ge_attributes) {
        gl.BindAttribLocation(program, 0u, "inPosition");
        gl.BindAttribLocation(program, 1u, "inColor");
        gl.BindAttribLocation(program, 2u, "inUv");
        gl.BindAttribLocation(program, 3u, "inQ");
        gl.BindAttribLocation(program, 4u, "inFog");
        gl.BindFragDataLocation(program, 0u, "outColor");
    } else {
        gl.BindFragDataLocation(program, 0u, "color");
    }
    gl.LinkProgram(program);
    gl.DeleteShader(vertex);
    gl.DeleteShader(fragment);
    GLint status = GL_FALSE;
    gl.GetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048]{};
        gl.GetProgramInfoLog(program, sizeof(log), nullptr, log);
        error = std::string("program failed to link: ") + log;
        gl.DeleteProgram(program);
        return 0u;
    }
    return program;
}

// ---------------------------------------------------------------------------
// Keys (identical to the Vulkan backend, so texture caching behaves the same).

std::uint64_t hash_mix(std::uint64_t hash, std::uint64_t value) noexcept {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}

std::uint64_t texture_key(const GeGpuDrawDescriptor &draw) noexcept {
    if (draw.texture_cache_key_hint != 0u) return draw.texture_cache_key_hint;
    std::uint64_t key = 0xCBF29CE484222325ull;
    const std::uint32_t levels = draw.texture_level_addresses[0] != 0u && draw.texture_mipmap_enabled
        ? std::min<std::uint32_t>(8u, draw.texture_max_level + 1u) : 1u;
    key = hash_mix(key, levels);
    for (std::uint32_t level = 0u; level < levels; ++level) {
        key = hash_mix(key, draw.texture_level_addresses[level] != 0u
            ? draw.texture_level_addresses[level] : draw.texture_address);
        key = hash_mix(key, draw.texture_level_buffer_widths[level] != 0u
            ? draw.texture_level_buffer_widths[level] : draw.texture_buffer_width);
        key = hash_mix(key, draw.texture_level_widths[level] != 0u
            ? draw.texture_level_widths[level] : draw.texture_width);
        key = hash_mix(key, draw.texture_level_heights[level] != 0u
            ? draw.texture_level_heights[level] : draw.texture_height);
    }
    key = hash_mix(key, draw.texture_format);
    key = hash_mix(key, draw.clut_address);
    key = hash_mix(key, draw.clut_format);
    key = hash_mix(key, draw.clut_shift);
    key = hash_mix(key, draw.clut_mask);
    key = hash_mix(key, draw.clut_start);
    key = hash_mix(key, draw.clut_checksum);
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_swizzled));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_min_linear));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_mag_linear));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_mipmap_enabled));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_mipmap_linear));
    key = hash_mix(key, draw.texture_max_level);
    key = hash_mix(key, draw.texture_level_mode);
    key = hash_mix(key, static_cast<std::uint32_t>(draw.texture_level_offset16));
    key = hash_mix(key, draw.texture_selected_level);
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_clamp_u));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_clamp_v));
    return key;
}

std::uint64_t draw_sampler_key(const GeGpuDrawDescriptor &draw) noexcept {
    const std::uint32_t anisotropy =
        std::clamp(lcs_render_configuration().rendering.anisotropic_filtering, 1u, 16u);
    std::uint64_t key = draw.texture_min_linear ? 1u : 0u;
    key = hash_mix(key, draw.texture_mag_linear ? 1u : 0u);
    key = hash_mix(key, (draw.texture_mipmap_enabled && draw.texture_mipmap_linear) ? 1u : 0u);
    key = hash_mix(key, draw.texture_clamp_u ? 1u : 0u);
    key = hash_mix(key, draw.texture_clamp_v ? 1u : 0u);
    key = hash_mix(key, draw.texture_level_mode);
    key = hash_mix(key, static_cast<std::uint32_t>(draw.texture_level_offset16));
    key = hash_mix(key, draw.texture_selected_level);
    key = hash_mix(key, draw.texture_max_level);
    key = hash_mix(key, anisotropy);
    return key;
}

float sampler_lod_bias(const GlState &s, const GeGpuDrawDescriptor &draw) noexcept {
    const float bias = static_cast<float>(draw.texture_level_offset16) / 16.0f + rendering_texture_lod_bias();
    const float limit = s.max_lod_bias > 0.0f ? s.max_lod_bias : 2.0f;
    return std::clamp(bias, -limit, limit);
}

GLuint sampler_for(GlState &s, const GeGpuDrawDescriptor &draw) {
    const std::uint64_t key = draw_sampler_key(draw);
    if (const auto found = s.samplers.find(key); found != s.samplers.end()) return found->second;
    const std::uint32_t anisotropy =
        std::clamp(lcs_render_configuration().rendering.anisotropic_filtering, 1u, 16u);
    const bool mip_linear = draw.texture_mipmap_enabled && draw.texture_mipmap_linear;
    GLuint sampler = 0u;
    gl.GenSamplers(1, &sampler);
    // A texture without mipmaps has GL_TEXTURE_MAX_LEVEL 0, so a mipmap filter
    // on it reads level 0 only, as a Vulkan image with one level does.
    const GLint min_filter = draw.texture_min_linear
        ? (mip_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
        : (mip_linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, draw.texture_mag_linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, draw.texture_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, draw.texture_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    gl.SamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, sampler_lod_bias(s, draw));
    float min_lod = 0.0f;
    float max_lod = static_cast<float>(std::max<std::uint32_t>(1u, draw.texture_max_level + 1u));
    if (draw.texture_level_mode == 1u) {
        min_lod = static_cast<float>(draw.texture_selected_level);
        max_lod = min_lod;
    }
    gl.SamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, min_lod);
    gl.SamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, max_lod);
    if (s.anisotropy && anisotropy > 1u && draw.texture_mipmap_enabled)
        gl.SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                             std::min(static_cast<float>(anisotropy), s.max_anisotropy));
    s.samplers.emplace(key, sampler);
    ++s.report.texture_samplers_created;
    return sampler;
}

GLuint make_plain_sampler(bool linear) {
    GLuint sampler = 0u;
    gl.GenSamplers(1, &sampler);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return sampler;
}

GLuint make_texture_2d(std::uint32_t width, std::uint32_t height, const void *pixels) {
    GLuint texture = 0u;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return texture;
}

// ---------------------------------------------------------------------------
// Targets.

Target *find_target(GlState &s, std::uint32_t address) {
    address &= 0x001FFFF0u;
    const auto found = s.targets.find(address);
    return found == s.targets.end() ? nullptr : &found->second;
}

void destroy_target(Target &target) noexcept {
    if (target.framebuffer) gl.DeleteFramebuffers(1, &target.framebuffer);
    if (target.color) gl.DeleteTextures(1, &target.color);
    if (target.snapshot) gl.DeleteTextures(1, &target.snapshot);
    if (target.depth) gl.DeleteRenderbuffers(1, &target.depth);
    target.framebuffer = target.color = target.snapshot = target.depth = 0u;
}

// Records a target; its GL objects are made at the next frame finish, where
// the context is current.
void ensure_target(GlState &s, std::uint32_t address) {
    address &= 0x001FFFF0u;
    if (find_target(s, address) != nullptr) return;
    Target target{};
    target.address = address;
    s.targets.emplace(address, target);
    ++s.report.framebuffer_targets_observed;
    ++s.report.native_framebuffer_targets;
}

bool realize_target(GlState &s, Target &target, std::string &error) {
    if (target.framebuffer != 0u) return true;
    target.color = make_texture_2d(s.target_width, s.target_height, nullptr);
    gl.GenRenderbuffers(1, &target.depth);
    gl.BindRenderbuffer(GL_RENDERBUFFER, target.depth);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, static_cast<GLsizei>(s.target_width),
                           static_cast<GLsizei>(s.target_height));
    gl.GenFramebuffers(1, &target.framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.color, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth);
    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    s.bound_target = nullptr;
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        error = "framebuffer incomplete (status 0x" + std::to_string(status) + ")";
        destroy_target(target);
        return false;
    }
    s.report.depth_image_created = true;
    s.report.depth_attachment_active = true;
    s.report.offscreen_image_created = true;
    s.report.framebuffer_created = true;
    return true;
}

void note_logical_size(GlState &s, std::uint32_t address, std::uint32_t width, std::uint32_t height) {
    Target *target = find_target(s, address);
    if (target == nullptr) return;
    if (width != 0u) target->logical_width = width;
    if (height != 0u) target->logical_height = height;
}

void clear_target_full(GlState &s, bool color, bool depth) {
    gl.Disable(GL_SCISSOR_TEST);
    GLbitfield bits = 0u;
    if (color) {
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) {
        gl.DepthMask(GL_TRUE);
        gl.ClearDepth(0.0);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (bits != 0u) gl.Clear(bits);
    gl.Enable(GL_SCISSOR_TEST);
    s.cache = CachedState{};
}

void begin_target(GlState &s, Target &target) {
    if (s.bound_target == &target) return;
    gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    gl.Viewport(0, 0, static_cast<GLsizei>(s.target_width), static_cast<GLsizei>(s.target_height));
    const bool first_ever = target.last_render_epoch == 0u;
    const bool first_this_frame = target.last_render_epoch != s.frame_epoch;
    const bool clear_color = first_ever || (first_this_frame && target.address == s.display_framebuffer);
    if (clear_color || first_this_frame) clear_target_full(s, clear_color, first_this_frame);
    s.bound_target = &target;
    target.last_render_epoch = s.frame_epoch;
}

bool snapshot_target(GlState &s, Target &target) {
    if (target.framebuffer == 0u) return false;
    if (target.snapshot == 0u) target.snapshot = make_texture_2d(s.target_width, s.target_height, nullptr);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, target.framebuffer);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, target.snapshot);
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, static_cast<GLsizei>(s.target_width),
                         static_cast<GLsizei>(s.target_height));
    if (s.bound_target != nullptr)
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s.bound_target->framebuffer);
    return true;
}

// ---------------------------------------------------------------------------
// Textures.

void evict_stale_textures(GlState &s) {
    if (s.textures.size() <= kMaxTextures) return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> stale;
    stale.reserve(s.textures.size());
    for (const auto &entry : s.textures) {
        if (entry.second.last_used_epoch >= s.frame_epoch) continue;
        stale.emplace_back(entry.second.last_used_epoch, entry.first);
    }
    std::sort(stale.begin(), stale.end());
    for (const auto &item : stale) {
        if (s.textures.size() <= kMaxTextures * 3u / 4u) break;
        const auto found = s.textures.find(item.second);
        if (found == s.textures.end()) continue;
        if (found->second.texture != 0u) gl.DeleteTextures(1, &found->second.texture);
        s.textures.erase(found);
        ++s.report.evicted_textures;
    }
}

bool upload_texture(GlState &s, Texture &texture) {
    if (!texture.dirty) return true;
    const std::uint32_t width = texture.width;
    const std::uint32_t height = texture.height;
    if (width == 0u || height == 0u || texture.rgba8.empty()) return false;
    const bool reallocate = texture.texture == 0u || texture.allocated_width != width ||
                            texture.allocated_height != height || texture.allocated_mips != texture.mip_levels;
    if (texture.texture == 0u) {
        gl.GenTextures(1, &texture.texture);
        ++s.report.texture_images_created;
    }
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, texture.texture);
    std::uint32_t mip_width = width;
    std::uint32_t mip_height = height;
    std::size_t offset = 0u;
    std::uint32_t uploaded = 0u;
    for (std::uint32_t level = 0u; level < texture.mip_levels; ++level) {
        const std::size_t level_bytes = static_cast<std::size_t>(mip_width) * mip_height * 4u;
        if (offset + level_bytes > texture.rgba8.size()) break;
        const void *pixels = texture.rgba8.data() + offset;
        if (reallocate)
            gl.TexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8, static_cast<GLsizei>(mip_width),
                          static_cast<GLsizei>(mip_height), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        else
            gl.TexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0, static_cast<GLsizei>(mip_width),
                             static_cast<GLsizei>(mip_height), GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        offset += level_bytes;
        ++uploaded;
        mip_width = std::max(1u, mip_width >> 1u);
        mip_height = std::max(1u, mip_height >> 1u);
    }
    if (uploaded == 0u) return false;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(uploaded - 1u));
    texture.allocated_width = width;
    texture.allocated_height = height;
    texture.allocated_mips = texture.mip_levels;
    texture.dirty = false;
    // The CPU copy is no longer needed once the GPU has it.
    texture.rgba8.clear();
    texture.rgba8.shrink_to_fit();
    ++s.report.texture_image_uploads;
    s.report.texture_image_upload_bytes += offset;
    s.report.uploaded_mip_levels += uploaded;
    return true;
}

bool prepare_texture(GlState &s, const GeGpuDrawDescriptor &draw, std::uint32_t width, std::uint32_t height,
                     std::uint32_t mips, std::vector<std::byte> rgba) {
    if (width == 0u) width = draw.texture_width;
    if (height == 0u) height = draw.texture_height;
    if (width == 0u || height == 0u || rgba.empty() || mips == 0u) return false;
    const std::uint64_t key = texture_key(draw);
    Texture &texture = s.textures[key];
    texture.descriptor = draw;
    texture.rgba8 = std::move(rgba);
    texture.mip_levels = mips;
    texture.width = width;
    texture.height = height;
    texture.dirty = true;
    texture.signature_epoch = s.frame_epoch;
    texture.last_used_epoch = s.frame_epoch;
    texture.prompt_variant = prompt_texture_variant(draw.texture_content_signature);
    s.last_texture_rgba = texture.rgba8;
    if (draw.texture_format <= 10u) s.report.base_texture_formats_active = true;
    ++s.report.decoded_texture_uploads;
    s.report.decoded_texture_bytes += texture.rgba8.size();
    s.report.unique_texture_keys = s.textures.size();
    return true;
}

// ---------------------------------------------------------------------------
// Draw state.

UploadVertex make_upload_vertex(const GeGpuVertex &source) noexcept {
    return {source.x, source.y, source.z, source.w, source.rgba, source.u, source.v,
            source.fog_factor, source.q};
}

float expand5(std::uint32_t value) noexcept {
    value &= 31u;
    const std::uint32_t expanded = (value << 3u) | (value >> 2u);
    return static_cast<float>(expanded) * (1.0f / 255.0f);
}

UploadVertex decode_packed(const Packed0115 &packed) noexcept {
    UploadVertex vertex{};
    vertex.x = static_cast<float>(packed.x) * (1.0f / 32768.0f);
    vertex.y = static_cast<float>(packed.y) * (1.0f / 32768.0f);
    vertex.z = static_cast<float>(packed.z) * (1.0f / 32768.0f);
    vertex.w = 1.0f;
    vertex.u = static_cast<float>(packed.u) * (1.0f / 128.0f);
    vertex.v = static_cast<float>(packed.v) * (1.0f / 128.0f);
    vertex.q = 1.0f;
    const std::uint32_t r = static_cast<std::uint32_t>(std::lround(expand5(packed.color) * 255.0f));
    const std::uint32_t g = static_cast<std::uint32_t>(std::lround(expand5(packed.color >> 5u) * 255.0f));
    const std::uint32_t b = static_cast<std::uint32_t>(std::lround(expand5(packed.color >> 10u) * 255.0f));
    const std::uint32_t a = (packed.color & 0x8000u) != 0u ? 255u : 0u;
    vertex.rgba = r | (g << 8u) | (b << 16u) | (a << 24u);
    return vertex;
}

std::array<float, 4> scaled(const std::array<float, 4> &a, float sa,
                            const std::array<float, 4> &b, float sb) noexcept {
    return {a[0] * sa + b[0] * sb, a[1] * sa + b[1] * sb, a[2] * sa + b[2] * sb, a[3] * sa + b[3] * sb};
}

DrawUniforms make_uniforms(const Batch &batch, std::uint32_t logical_width, std::uint32_t logical_height,
                           bool sampled) noexcept {
    DrawUniforms constants{};
    logical_width = std::max<std::uint32_t>(1u, logical_width);
    logical_height = std::max<std::uint32_t>(1u, logical_height);
    if (!batch.hardware_transform) {
        constants.uv[0] = 2.0f / static_cast<float>(logical_width);
        constants.uv[1] = 2.0f / static_cast<float>(logical_height);
        constants.uv[2] = 1.0f / 65535.0f;
        constants.control[0] = 2u;
    } else {
        const GeGpuHardwareTransform &hw = batch.transform;
        const auto row = [&](std::size_t r) {
            return std::array<float, 4>{hw.model_to_clip[r], hw.model_to_clip[4u + r],
                                        hw.model_to_clip[8u + r], hw.model_to_clip[12u + r]};
        };
        const auto clip_x = row(0u);
        const auto clip_y = row(1u);
        const auto clip_z = row(2u);
        const auto clip_w = row(3u);
        const float x_a = hw.viewport_scale_x * (2.0f / static_cast<float>(logical_width));
        const float x_b = (hw.viewport_center_x - hw.viewport_offset_x) *
                          (2.0f / static_cast<float>(logical_width)) - 1.0f;
        const float y_a = hw.viewport_scale_y * (2.0f / static_cast<float>(logical_height));
        const float y_b = (hw.viewport_center_y - hw.viewport_offset_y) *
                          (2.0f / static_cast<float>(logical_height)) - 1.0f;
        constexpr float inv_depth = 1.0f / 65535.0f;
        const auto row_x = scaled(clip_x, x_a, clip_w, x_b);
        const auto row_y = scaled(clip_y, -y_a, clip_w, -y_b);
        const auto row_z = scaled(clip_z, hw.viewport_scale_z * inv_depth, clip_w,
                                  hw.viewport_center_z * inv_depth);
        std::copy(row_x.begin(), row_x.end(), constants.row0);
        std::copy(row_y.begin(), row_y.end(), constants.row1);
        std::copy(row_z.begin(), row_z.end(), constants.row2);
        std::copy(clip_w.begin(), clip_w.end(), constants.row3);
        std::copy(hw.model_to_view_z.begin(), hw.model_to_view_z.end(), constants.view_z);
        constants.uv[0] = hw.uv_scale_u;
        constants.uv[1] = hw.uv_scale_v;
        constants.uv[2] = hw.uv_offset_u;
        constants.uv[3] = hw.uv_offset_v;
        constants.fog[0] = hw.fog_end;
        constants.fog[1] = hw.fog_slope;
        constants.control[0] = 1u;
        constants.control[1] = hw.depth_clip_enabled ? 1u : 0u;
        constants.control[2] = hw.vertex_color_affine ? 1u : 0u;
        std::copy(hw.vertex_color_mul.begin(), hw.vertex_color_mul.end(), constants.color_mul);
        std::copy(hw.vertex_color_add.begin(), hw.vertex_color_add.end(), constants.color_add);
    }
    const GeGpuDrawDescriptor &draw = batch.draw;
    constants.pixel0[0] = static_cast<std::uint32_t>(draw.alpha_test_enabled ? 1u : 0u) |
                          ((draw.alpha_function & 7u) << 8u) |
                          ((draw.alpha_reference & 0xFFu) << 16u) |
                          ((draw.alpha_mask & 0xFFu) << 24u);
    constants.pixel0[1] = (draw.texture_function & 0xFFu) |
                          (static_cast<std::uint32_t>(draw.texture_use_alpha ? 1u : 0u) << 8u) |
                          (static_cast<std::uint32_t>(draw.texture_double_color ? 1u : 0u) << 16u) |
                          (static_cast<std::uint32_t>(sampled ? 1u : 0u) << 24u);
    constants.pixel0[2] = draw.texture_env & 0x00FFFFFFu;
    constants.pixel0[3] = (draw.fog_color & 0x00FFFFFFu) |
                          (static_cast<std::uint32_t>(draw.fog_enabled ? 0xFFu : 0u) << 24u);
    constants.pixel1[0] = draw.framebuffer_format & 3u;
    return constants;
}

std::size_t blend_variant(const GeGpuDrawDescriptor &draw) noexcept {
    if (!draw.blend_enabled || draw.clear_mode) return 0u;
    const std::uint32_t eq = draw.blend_equation & 7u;
    const std::uint32_t src = draw.blend_source_factor & 0xFu;
    const std::uint32_t dst = draw.blend_dest_factor & 0xFu;
    if (eq == 0u && src == 2u && dst == 3u) return 1u;
    if (eq == 0u && src == 10u && dst == 10u) {
        const std::uint32_t fs = draw.blend_fix_source & 0x00FFFFFFu;
        const std::uint32_t fd = draw.blend_fix_dest & 0x00FFFFFFu;
        if (fs == 0x00FFFFFFu && fd == 0u) return 2u;
        if (fs == 0x00FFFFFFu && fd == 0x00FFFFFFu) return 3u;
        bool complements = true;
        for (std::uint32_t shift = 0u; shift < 24u; shift += 8u)
            complements &= (((fs >> shift) & 0xFFu) + ((fd >> shift) & 0xFFu)) == 0xFFu;
        if (complements) return 4u;
    }
    if (eq == 0u && src == 2u && dst == 10u &&
        (draw.blend_fix_dest & 0x00FFFFFFu) == 0x00FFFFFFu) return 5u;
    return 0u;
}

// Bit n set = channel n is written (PSP mask bytes of 0xFF block a channel).
std::uint32_t color_write_bits(const GeGpuDrawDescriptor &draw) noexcept {
    std::uint32_t mask = 0u;
    for (std::uint32_t channel = 0u; channel < 4u; ++channel) {
        const std::uint32_t byte = (draw.color_write_mask >> (channel * 8u)) & 0xFFu;
        if (byte != 0xFFu) mask |= 1u << channel;
    }
    return mask;
}

GLenum depth_compare(std::uint32_t function) noexcept {
    switch (function & 7u) {
    case 0u: return GL_NEVER;
    case 1u: return GL_ALWAYS;
    case 2u: return GL_EQUAL;
    case 3u: return GL_NOTEQUAL;
    case 4u: return GL_LESS;
    case 5u: return GL_LEQUAL;
    case 6u: return GL_GREATER;
    case 7u: return GL_GEQUAL;
    }
    return GL_ALWAYS;
}

void apply_draw_state(GlState &s, const GeGpuDrawDescriptor &draw, bool cull, bool ccw) {
    CachedState &c = s.cache;
    const bool depth_test = draw.depth_test_enabled;
    const bool depth_write = draw.depth_write_enabled;
    const GLenum depth_func = depth_test ? depth_compare(draw.depth_function) : GL_ALWAYS;
    // As in Vulkan (depthWriteEnable only counts while depthTestEnable is set),
    // a disabled depth test also stops depth writes.
    if (!c.valid || c.depth_test != depth_test) {
        if (depth_test) gl.Enable(GL_DEPTH_TEST); else gl.Disable(GL_DEPTH_TEST);
        c.depth_test = depth_test;
    }
    if (!c.valid || c.depth_func != depth_func) {
        gl.DepthFunc(depth_func);
        c.depth_func = depth_func;
    }
    if (!c.valid || c.depth_write != depth_write) {
        gl.DepthMask(depth_write ? GL_TRUE : GL_FALSE);
        c.depth_write = depth_write;
    }
    const std::uint32_t mask = color_write_bits(draw);
    if (!c.valid || c.color_mask != mask) {
        gl.ColorMask((mask & 1u) ? GL_TRUE : GL_FALSE, (mask & 2u) ? GL_TRUE : GL_FALSE,
                     (mask & 4u) ? GL_TRUE : GL_FALSE, (mask & 8u) ? GL_TRUE : GL_FALSE);
        c.color_mask = mask;
    }
    const std::size_t variant = blend_variant(draw);
    if (!c.valid || c.blend != variant) {
        if (variant == 0u || variant == 2u) {
            gl.Disable(GL_BLEND);
        } else {
            gl.Enable(GL_BLEND);
            gl.BlendEquation(GL_FUNC_ADD);
            switch (variant) {
            case 1u: gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case 3u: gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE); break;
            case 4u: gl.BlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_ONE, GL_ZERO); break;
            case 5u: gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE); break;
            default: break;
            }
        }
        c.blend = variant;
    }
    if (variant == 4u) {
        const std::uint32_t fix = draw.blend_fix_source & 0x00FFFFFFu;
        const std::array<float, 4> color{static_cast<float>(fix & 0xFFu) / 255.0f,
                                         static_cast<float>((fix >> 8u) & 0xFFu) / 255.0f,
                                         static_cast<float>((fix >> 16u) & 0xFFu) / 255.0f, 1.0f};
        if (!c.valid || c.blend_color != color) {
            gl.BlendColor(color[0], color[1], color[2], color[3]);
            c.blend_color = color;
        }
    }
    if (!c.valid || c.cull != cull) {
        if (cull) gl.Enable(GL_CULL_FACE); else gl.Disable(GL_CULL_FACE);
        c.cull = cull;
    }
    if (cull) {
        // Vulkan's front_face = CCW is GL's CW for the same clip coordinates.
        GLenum face = ccw ? GL_CW : GL_CCW;
        if (s.invert_winding) face = face == GL_CW ? GL_CCW : GL_CW;
        if (!c.valid || c.front_face != face) {
            gl.FrontFace(face);
            c.front_face = face;
        }
    }
    c.valid = true;
}

void count_draw(GlState &s, const Batch &batch, bool textured) {
    const std::uint32_t n = batch.logical_draw_count;
    if (batch.draw.depth_test_enabled) s.report.depth_tested_game_draw_calls += n;
    if (batch.draw.depth_write_enabled) s.report.depth_writing_game_draw_calls += n;
    if (batch.draw.alpha_test_enabled) {
        s.report.alpha_tested_game_draw_calls += n;
        s.report.alpha_test_shader_active = true;
    }
    switch (blend_variant(batch.draw)) {
    case 1u:
        s.report.standard_alpha_blended_game_draw_calls += n;
        s.report.standard_alpha_blend_pipeline_active = true;
        break;
    case 2u: s.report.fixed_replace_blended_game_draw_calls += n; break;
    case 3u: s.report.additive_blended_game_draw_calls += n; break;
    default: break;
    }
    if (batch.draw.fog_enabled) {
        s.report.fogged_game_draw_calls += n;
        s.report.fog_shader_active = true;
    }
    if (textured) {
        switch (batch.draw.texture_function & 7u) {
        case 0u: s.report.modulate_texture_game_draw_calls += n; break;
        case 1u: s.report.decal_texture_game_draw_calls += n; break;
        case 2u: s.report.blend_texture_game_draw_calls += n; break;
        case 3u: s.report.replace_texture_game_draw_calls += n; break;
        case 4u: s.report.add_texture_game_draw_calls += n; break;
        default: ++s.report.unsupported_texture_function_game_draw_calls; break;
        }
    }
}

// ---------------------------------------------------------------------------
// Presentation to the window.

bool window_pixels(GlState &s, std::uint32_t &width, std::uint32_t &height) noexcept {
    if (s.window == nullptr) return false;
    int w = 0;
    int h = 0;
    SDL_GL_GetDrawableSize(s.window, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetWindowSize(s.window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

// x/y are top-left based, as in the Vulkan present pass.
void draw_present_layer(GlState &s, GLuint texture, GLuint sampler, std::int32_t x, std::int32_t y,
                        std::int32_t width, std::int32_t height, std::uint32_t screen_width,
                        std::uint32_t screen_height, bool opaque) {
    if (texture == 0u || width <= 0 || height <= 0) return;
    const std::int32_t gl_y = static_cast<std::int32_t>(screen_height) - y - height;
    const std::int32_t left = std::max(0, x);
    const std::int32_t bottom = std::max(0, gl_y);
    const std::int32_t right = std::min(x + width, static_cast<std::int32_t>(screen_width));
    const std::int32_t top = std::min(gl_y + height, static_cast<std::int32_t>(screen_height));
    if (right <= left || top <= bottom) return;
    gl.Viewport(x, gl_y, width, height);
    gl.Scissor(left, bottom, right - left, top - bottom);
    gl.Uniform1i(s.present_opaque_location, opaque ? 1 : 0);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.BindSampler(0u, sampler);
    if (opaque) gl.Disable(GL_BLEND);
    else {
        gl.Enable(GL_BLEND);
        gl.BlendEquation(GL_FUNC_ADD);
        gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void upload_overlay(GlState &s, GLuint &texture, std::uint32_t width, std::uint32_t height) {
    if (texture == 0u) texture = make_texture_2d(width, height, nullptr);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                     GL_UNSIGNED_BYTE, s.overlay_pixels.data());
}

// Shows `source` (rows top-down) fitted into the window, plus the overlays.
bool present_to_window(GlState &s, GLuint source, std::uint32_t source_width, std::uint32_t source_height) {
    std::uint32_t screen_width = 0u;
    std::uint32_t screen_height = 0u;
    if (!window_pixels(s, screen_width, screen_height)) return false;
    const Dx12FramePresentDecision decision = decide_dx12_frame_present(true);
    host_fps_note_presented_frame();
    bool draw_fps = false;
    bool draw_settings = false;
    if (decision.composite_fps) {
        s.overlay_pixels.assign(static_cast<std::size_t>(kFpsOverlayWidth) * kFpsOverlayHeight * 4u, 0u);
        rasterize_fps_overlay(s.overlay_pixels.data(), kFpsOverlayWidth, kFpsOverlayHeight);
        upload_overlay(s, s.fps_overlay, kFpsOverlayWidth, kFpsOverlayHeight);
        draw_fps = true;
    }
    if (decision.composite_settings) {
        s.overlay_pixels.assign(static_cast<std::size_t>(kSettingsOverlayWidth) * kSettingsOverlayHeight * 4u, 0u);
        rasterize_settings_overlay(decision.settings, s.overlay_pixels.data(), kSettingsOverlayWidth,
                                   kSettingsOverlayHeight);
        upload_overlay(s, s.settings_overlay, kSettingsOverlayWidth, kSettingsOverlayHeight);
        draw_settings = true;
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0u);
    s.bound_target = nullptr;
    s.cache = CachedState{};
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.DepthMask(GL_FALSE);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Viewport(0, 0, static_cast<GLsizei>(screen_width), static_cast<GLsizei>(screen_height));
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Enable(GL_SCISSOR_TEST);
    gl.UseProgram(s.present_program);
    gl.BindVertexArray(s.present_vao);
    const DisplayConfiguration &display = lcs_render_configuration().display;
    const PresentationRectangle fitted = calculate_presentation_rectangle(
        screen_width, screen_height, source_width, source_height, display.aspect_mode, display.integer_scale);
    const bool nearest = display.upscale_filter == DisplayUpscaleFilter::Nearest;
    draw_present_layer(s, source, nearest ? s.present_nearest : s.present_linear, fitted.x, fitted.y,
                       fitted.width, fitted.height, screen_width, screen_height, true);
    if (draw_fps) {
        const std::int32_t panel_h = std::max(28, static_cast<std::int32_t>(screen_height / 28u));
        const std::int32_t panel_w = panel_h * static_cast<std::int32_t>(kFpsOverlayWidth / kFpsOverlayHeight);
        draw_present_layer(s, s.fps_overlay, s.present_linear, 16, 16, panel_w, panel_h, screen_width,
                           screen_height, false);
    }
    if (draw_settings) {
        const bool centered = decision.settings.centered;
        const float share = centered ? 0.62f : 0.46f;
        const std::int32_t panel_w = std::min(static_cast<std::int32_t>(static_cast<float>(screen_width) * share),
                                              static_cast<std::int32_t>(kSettingsOverlayWidth) * (centered ? 2 : 1));
        const std::int32_t panel_h = panel_w / 2;
        const std::int32_t x = centered ? (static_cast<std::int32_t>(screen_width) - panel_w) / 2
                                        : static_cast<std::int32_t>(screen_width) - panel_w - 24;
        const std::int32_t y = centered ? (static_cast<std::int32_t>(screen_height) - panel_h) / 2 : 24;
        draw_present_layer(s, s.settings_overlay, s.present_linear, x, y, panel_w, panel_h,
                           screen_width, screen_height, false);
    }
    gl.BindSampler(0u, 0u);
    check_gl(s, "present");
    SDL_GL_SwapWindow(s.window);
    return true;
}

// ---------------------------------------------------------------------------
// Creation.

void destroy_backend(GlState &s) noexcept {
    if (s.context != nullptr && s.window != nullptr && SDL_GL_MakeCurrent(s.window, s.context) == 0) {
        for (auto &entry : s.targets) destroy_target(entry.second);
        for (auto &entry : s.textures)
            if (entry.second.texture != 0u) gl.DeleteTextures(1, &entry.second.texture);
        for (auto &entry : s.samplers) gl.DeleteSamplers(1, &entry.second);
        const GLuint textures[]{s.white, s.fps_overlay, s.settings_overlay, s.guest_image};
        for (GLuint texture : textures)
            if (texture != 0u) gl.DeleteTextures(1, &texture);
        const GLuint samplers[]{s.white_sampler, s.present_linear, s.present_nearest};
        for (GLuint sampler : samplers)
            if (sampler != 0u) gl.DeleteSamplers(1, &sampler);
        const GLuint buffers[]{s.vertex_buffer, s.index_buffer, s.uniform_buffer};
        for (GLuint buffer : buffers)
            if (buffer != 0u) gl.DeleteBuffers(1, &buffer);
        if (s.vao != 0u) gl.DeleteVertexArrays(1, &s.vao);
        if (s.present_vao != 0u) gl.DeleteVertexArrays(1, &s.present_vao);
        if (s.program != 0u) gl.DeleteProgram(s.program);
        if (s.present_program != 0u) gl.DeleteProgram(s.present_program);
        SDL_GL_MakeCurrent(s.window, nullptr);
    }
    if (s.context != nullptr) SDL_GL_DeleteContext(s.context);
    s.context = nullptr;
    s.targets.clear();
    s.textures.clear();
    s.samplers.clear();
    s.white = s.fps_overlay = s.settings_overlay = s.guest_image = 0u;
    s.white_sampler = s.present_linear = s.present_nearest = 0u;
    s.vertex_buffer = s.index_buffer = s.uniform_buffer = 0u;
    s.vao = s.present_vao = 0u;
    s.program = s.present_program = 0u;
    s.enabled = false;
    s.bound_target = nullptr;
    s.direct_present_ok = false;
}

bool env_flag(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return *value != '0';
}

bool create_backend(GlState &s, std::string &error) {
    if (s.window == nullptr) {
        error = "no window for the OpenGL GE";
        return false;
    }
    const InternalResolutionDimensions dims = resolve_internal_resolution(lcs_render_configuration().rendering);
    s.target_width = std::max<std::uint32_t>(1u, dims.width);
    s.target_height = std::max<std::uint32_t>(1u, dims.height);
    s.invert_winding = env_flag("LCS_GL_INVERT_WINDING", false);
    s.readback_mode = env_flag("LCS_GL_READBACK", false);
    s.vsync = env_flag("LCS_GL_VSYNC", lcs_render_configuration().display.vsync);

    // Core 4.3 first (what the Switch's Mesa offers), then 3.3.
    const std::array<std::pair<int, int>, 2> versions{{{4, 3}, {3, 3}}};
    for (const auto &[major, minor] : versions) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        s.context = SDL_GL_CreateContext(s.window);
        if (s.context != nullptr) break;
    }
    if (s.context == nullptr) {
        error = std::string("SDL_GL_CreateContext failed: ") + SDL_GetError();
        return false;
    }
    if (SDL_GL_MakeCurrent(s.window, s.context) != 0) {
        error = std::string("SDL_GL_MakeCurrent failed: ") + SDL_GetError();
        return false;
    }
    if (!load_gl(error)) return false;
    SDL_GL_SetSwapInterval(s.vsync ? 1 : 0);
    const auto text = [](GLenum name) {
        const GLubyte *value = gl.GetString(name);
        return value != nullptr ? std::string(reinterpret_cast<const char *>(value)) : std::string("?");
    };
    std::cerr << "[gl] " << text(GL_VERSION) << " | " << text(GL_RENDERER) << " | target " << s.target_width
              << "x" << s.target_height << (s.vsync ? " vsync" : "") << (s.readback_mode ? " readback" : "")
              << "\n";

    GLint extensions = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (GLint index = 0; index < extensions; ++index) {
        const GLubyte *name = gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(index));
        if (name == nullptr) continue;
        const char *ext = reinterpret_cast<const char *>(name);
        if (std::strcmp(ext, "GL_EXT_texture_filter_anisotropic") == 0 ||
            std::strcmp(ext, "GL_ARB_texture_filter_anisotropic") == 0)
            s.anisotropy = true;
    }
    if (s.anisotropy) gl.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &s.max_anisotropy);
    gl.GetFloatv(GL_MAX_TEXTURE_LOD_BIAS, &s.max_lod_bias);
    GLint align = 256;
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    s.uniform_align = static_cast<std::uint32_t>(std::max<GLint>(align, 16));
    while (s.uniform_align < sizeof(DrawUniforms)) s.uniform_align *= 2u;

    const std::string header = "#version 330 core\n";
    s.program = link_program(header + kUniformBlock + kVertexShader, header + kUniformBlock + kFragmentShader,
                             true, error);
    if (s.program == 0u) return false;
    const GLuint block = gl.GetUniformBlockIndex(s.program, "DrawUniforms");
    if (block == GL_INVALID_INDEX) {
        error = "DrawUniforms block not found";
        return false;
    }
    gl.UniformBlockBinding(s.program, block, 0u);
    gl.UseProgram(s.program);
    gl.Uniform1i(gl.GetUniformLocation(s.program, "sourceTexture"), 0);
    s.report.shader_modules_created = true;
    s.report.textured_shader_modules_created = true;

    s.present_program = link_program(header + kPresentVertexShader, header + kPresentFragmentShader, false, error);
    if (s.present_program == 0u) return false;
    gl.UseProgram(s.present_program);
    gl.Uniform1i(gl.GetUniformLocation(s.present_program, "image"), 0);
    s.present_opaque_location = gl.GetUniformLocation(s.present_program, "opaque");
    s.present_flip_location = gl.GetUniformLocation(s.present_program, "flipRows");
    gl.Uniform1i(s.present_flip_location, 0);
    gl.UseProgram(0u);

    gl.GenBuffers(1, &s.vertex_buffer);
    gl.GenBuffers(1, &s.index_buffer);
    gl.GenBuffers(1, &s.uniform_buffer);
    gl.GenVertexArrays(1, &s.vao);
    gl.BindVertexArray(s.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, s.vertex_buffer);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.index_buffer);
    const auto stride = static_cast<GLsizei>(sizeof(UploadVertex));
    const auto at = [](std::size_t offset) { return reinterpret_cast<const void *>(offset); };
    gl.EnableVertexAttribArray(0u);
    gl.VertexAttribPointer(0u, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(UploadVertex, x)));
    gl.EnableVertexAttribArray(1u);
    gl.VertexAttribPointer(1u, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(offsetof(UploadVertex, rgba)));
    gl.EnableVertexAttribArray(2u);
    gl.VertexAttribPointer(2u, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(UploadVertex, u)));
    gl.EnableVertexAttribArray(3u);
    gl.VertexAttribPointer(3u, 1, GL_FLOAT, GL_FALSE, stride, at(offsetof(UploadVertex, q)));
    gl.EnableVertexAttribArray(4u);
    gl.VertexAttribPointer(4u, 1, GL_FLOAT, GL_FALSE, stride, at(offsetof(UploadVertex, fog_factor)));
    gl.BindVertexArray(0u);
    gl.GenVertexArrays(1, &s.present_vao);

    const std::uint32_t white_pixel = 0xFFFFFFFFu;
    s.white = make_texture_2d(1u, 1u, &white_pixel);
    s.white_sampler = make_plain_sampler(false);
    s.present_linear = make_plain_sampler(true);
    s.present_nearest = make_plain_sampler(false);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.Enable(GL_SCISSOR_TEST);
    check_gl(s, "create");

    // A first black frame, so the window does not show stale memory.
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0u);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(s.window);
    SDL_GL_MakeCurrent(s.window, nullptr);

    s.report.instance_created = true;
    s.report.device_created = true;
    s.report.graphics_pipeline_created = true;
    s.report.textured_pipeline_created = true;
    s.report.frames_in_flight_capacity = 1u;
    s.report.upload_capacity_bytes = kGeometryUploadCapacity;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The backend interface (ge_gpu_backend.hpp).

bool initialize_ge_gpu_backend(std::string &error) {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    destroy_backend(s);
    s.report = {};
    s.display_framebuffer = 0u;
    const RenderingConfiguration &rendering = lcs_render_configuration().rendering;
    const bool requested_gpu = rendering.backend == RenderingBackend::DirectX12 ||
                               rendering.backend == RenderingBackend::Vulkan;
    s.report.requested = requested_gpu ? GeGpuBackendKind::OpenGL : GeGpuBackendKind::Software;
    s.report.active = GeGpuBackendKind::Software;
    if (!requested_gpu) {
        s.report.message = "Software GE backend active";
        error.clear();
        return true;
    }
    if (!rendering.dx12_ge_color) {
        s.report.message = "OpenGL GE is built, but NativeGE=false, so the software rasterizer stays active";
        error.clear();
        return true;
    }
    if (!create_backend(s, error)) {
        const std::string native_error = error;
        runtime_log_error("opengl ge initialize", native_error);
        std::cerr << "[gl] " << native_error << "; using the software GE\n";
        destroy_backend(s);
        s.report.active = GeGpuBackendKind::Software;
        s.report.message = "OpenGL GE failed; using the software GE: " + native_error;
        const char *strict = std::getenv("PSPRECOMP_GE_STRICT");
        const bool strict_mode = strict != nullptr && *strict != '\0' && *strict != '0';
        error = native_error;
        return !strict_mode;
    }
    s.enabled = true;
    s.report.active = GeGpuBackendKind::OpenGL;
    s.report.loader_opened = true;
    s.report.message = "OpenGL GE backend active";
    s.report.offscreen_width = s.target_width;
    s.report.offscreen_height = s.target_height;
    error.clear();
    return true;
}

void shutdown_ge_gpu_backend() noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    destroy_backend(s);
    s.report = {};
    s.vertices.clear();
    s.indices.clear();
    s.batches.clear();
    s.frame_rgba.clear();
}

bool ge_gpu_backend_active() noexcept { return state().enabled; }
bool ge_gpu_backend_transfer_ready() noexcept { return state().enabled; }
bool ge_gpu_backend_graphics_ready() noexcept { return state().enabled; }

void ge_gpu_backend_record_draw(const GeGpuDrawDescriptor &draw) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled) return;
    ++s.report.draw_calls;
    s.report.vertices += draw.vertex_count;
    if (draw.texture_enabled) ++s.report.textured_draw_calls;
    const std::uint32_t target = draw.framebuffer_address & 0x001FFFF0u;
    if (draw.framebuffer_stride != 0u || target == s.display_framebuffer) {
        ensure_target(s, target);
        const std::uint32_t logical_width = target == s.display_framebuffer
            ? s.display_logical_width
            : std::max<std::uint32_t>(1u, draw.framebuffer_stride != 0u
                  ? draw.framebuffer_stride
                  : static_cast<std::uint32_t>(std::max(1, draw.scissor_x1 + 1)));
        const std::uint32_t logical_height = target == s.display_framebuffer
            ? s.display_logical_height
            : static_cast<std::uint32_t>(std::max(1, draw.scissor_y1 + 1));
        note_logical_size(s, target, logical_width, logical_height);
    }
}

void ge_gpu_backend_observe_camera(const std::array<float, 12> &, const std::array<float, 16> &,
                                   const std::array<float, 6> &, const std::array<float, 3> &,
                                   const GeGpuDrawDescriptor &, std::uint32_t) noexcept {}

bool ge_gpu_backend_stage_vertices(const GeGpuDrawDescriptor &, std::span<const GeGpuVertex> vertices) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled) return false;
    ++s.report.staged_draw_calls;
    s.report.staged_vertices += vertices.size();
    s.report.staged_bytes += vertices.size_bytes();
    return true;
}

bool ge_gpu_backend_texture_needed(const GeGpuDrawDescriptor &draw) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled || !draw.texture_enabled || draw.texture_format > 10u ||
        draw.texture_width == 0u || draw.texture_height == 0u) return false;
    if (find_target(s, draw.texture_address) != nullptr) {
        ++s.report.texture_cache_hits;
        return false;
    }
    ++s.report.texture_decode_requests;
    const auto found = s.textures.find(texture_key(draw));
    if (found == s.textures.end()) return true;
    found->second.signature_epoch = s.frame_epoch;
    found->second.last_used_epoch = s.frame_epoch;
    if (draw.texture_content_signature != 0u &&
        found->second.descriptor.texture_content_signature != draw.texture_content_signature)
        return true;
    // A repainted button prompt whose picture changed with a setting.
    if (draw.texture_content_signature != 0u &&
        found->second.prompt_variant != prompt_texture_variant(draw.texture_content_signature))
        return true;
    ++s.report.texture_cache_hits;
    return false;
}

void ge_gpu_backend_prepare_texture_keys(GeGpuDrawDescriptor &draw) noexcept {
    if (!draw.texture_enabled) {
        draw.texture_cache_key_hint = 0u;
        draw.texture_image_key_hint = 0u;
        return;
    }
    const std::uint64_t key = texture_key(draw);
    draw.texture_cache_key_hint = key;
    draw.texture_image_key_hint = key;
}

bool ge_gpu_backend_texture_signature_needed(const GeGpuDrawDescriptor &draw) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled || !draw.texture_enabled || draw.texture_width == 0u || draw.texture_height == 0u)
        return false;
    if (find_target(s, draw.texture_address) != nullptr) return false;
    const auto found = s.textures.find(texture_key(draw));
    return found == s.textures.end() || found->second.signature_epoch != s.frame_epoch;
}

bool ge_gpu_backend_is_framebuffer_feedback_texture(const GeGpuDrawDescriptor &draw) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    return s.enabled && draw.texture_enabled && find_target(s, draw.texture_address) != nullptr;
}

GeGpuWidescreenHud ge_gpu_backend_widescreen_hud(const GeGpuDrawDescriptor &draw) noexcept {
    GeGpuWidescreenHud hud{};
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled) return hud;
    const LcsConfiguration &config = lcs_render_configuration();
    if (!config.initialized || !config.widescreen.enabled) return hud;
    const float shrink = widescreen_render_stretch();
    if (!std::isfinite(shrink) || shrink <= 0.0f || std::abs(shrink - 1.0f) < 1.0e-5f) return hud;
    std::uint32_t logical_width = kReferenceWidth;
    if (const Target *target = find_target(s, draw.framebuffer_address))
        logical_width = std::max<std::uint32_t>(1u, target->logical_width);
    hud.shrink = shrink;
    hud.display_scale_x = static_cast<float>(kReferenceWidth) / static_cast<float>(logical_width);
    hud.source_center = static_cast<float>(logical_width) * 0.5f;
    return hud;
}

void ge_gpu_backend_note_through_extent(const GeGpuDrawDescriptor &, float, float) noexcept {}
bool ge_gpu_backend_adopt_shared_texture(const GeGpuDrawDescriptor &) noexcept { return false; }

bool ge_gpu_backend_texture_available(const GeGpuDrawDescriptor &draw) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled || !draw.texture_enabled) return false;
    if (find_target(s, draw.texture_address) != nullptr) return true;
    const auto found = s.textures.find(texture_key(draw));
    if (found == s.textures.end()) return false;
    found->second.last_used_epoch = s.frame_epoch;
    // Uploaded at the next finish if it is still dirty.
    return found->second.texture != 0u || !found->second.rgba8.empty();
}

bool ge_gpu_backend_upload_decoded_texture(const GeGpuDrawDescriptor &draw, std::uint32_t width,
                                           std::uint32_t height, std::span<const std::byte> rgba) noexcept {
    if (rgba.empty()) return false;
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled) return false;
    return prepare_texture(s, draw, width, height, 1u, std::vector<std::byte>(rgba.begin(), rgba.end()));
}

bool ge_gpu_backend_upload_decoded_texture_chain(const GeGpuDrawDescriptor &draw,
                                                 std::span<const GeGpuDecodedMipLevel> levels) noexcept {
    if (levels.empty() || levels.size() > 8u || levels.front().width == 0u || levels.front().height == 0u)
        return false;
    std::vector<std::byte> packed;
    const std::uint32_t width = levels.front().width;
    const std::uint32_t height = levels.front().height;
    for (const GeGpuDecodedMipLevel &level : levels) {
        const std::size_t bytes = static_cast<std::size_t>(level.width) * level.height * 4u;
        if (level.rgba8.size() != bytes) return false;
        packed.insert(packed.end(), level.rgba8.begin(), level.rgba8.end());
    }
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled) return false;
    return prepare_texture(s, draw, width, height, static_cast<std::uint32_t>(levels.size()), std::move(packed));
}

bool ge_gpu_backend_upload_decoded_texture_chain_packed(const GeGpuDrawDescriptor &draw,
                                                        std::uint32_t width, std::uint32_t height,
                                                        std::uint32_t mip_levels,
                                                        std::vector<std::byte> packed) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled) return false;
    return prepare_texture(s, draw, width, height, mip_levels, std::move(packed));
}

bool ge_gpu_backend_copy_last_texture_rgba(std::span<std::byte> destination) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (s.last_texture_rgba.empty() || destination.size() < s.last_texture_rgba.size()) return false;
    std::memcpy(destination.data(), s.last_texture_rgba.data(), s.last_texture_rgba.size());
    return true;
}

void ge_gpu_backend_accumulate_color_triangles(const GeGpuDrawDescriptor &draw,
                                               std::span<const GeGpuVertex> triangle_vertices) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled || triangle_vertices.empty() || triangle_vertices.size() % 3u != 0u) return;
    const std::size_t used = s.vertices.size() * sizeof(UploadVertex) + s.indices.size() * sizeof(std::uint32_t);
    if (used + triangle_vertices.size() * sizeof(UploadVertex) > kGeometryUploadCapacity) {
        ++s.report.game_vertex_overflows;
        return;
    }
    const bool sampled = draw.texture_enabled && ge_gpu_backend_texture_available(draw);
    const std::uint32_t first = static_cast<std::uint32_t>(s.vertices.size());
    for (const GeGpuVertex &source : triangle_vertices) {
        UploadVertex vertex = make_upload_vertex(source);
        if (sampled && draw.texture_width != 0u && draw.texture_height != 0u) {
            vertex.u /= static_cast<float>(draw.texture_width);
            vertex.v /= static_cast<float>(draw.texture_height);
        }
        s.vertices.push_back(vertex);
    }
    Batch batch{};
    batch.draw = draw;
    batch.first_vertex = first;
    batch.vertex_count = static_cast<std::uint32_t>(triangle_vertices.size());
    batch.feedback_address = draw.texture_address & 0x001FFFF0u;
    batch.framebuffer_feedback = draw.texture_enabled && find_target(s, batch.feedback_address) != nullptr;
    s.batches.push_back(batch);
    ++s.report.game_draw_calls;
    s.report.game_triangles += triangle_vertices.size() / 3u;
    s.report.game_vertices += triangle_vertices.size();
    if (draw.texture_enabled && sampled) ++s.report.textured_game_draw_calls;
    else if (draw.texture_enabled) ++s.report.game_textured_draws_without_texture;
}

void ge_gpu_backend_accumulate_hardware_triangles(
    const GeGpuDrawDescriptor &draw, const GeGpuHardwareTransform &transform,
    std::span<const GeGpuVertex> vertices, std::span<const std::uint32_t> triangle_indices) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled || vertices.empty()) return;
    const bool indexed = !triangle_indices.empty();
    const std::size_t emitted = indexed ? triangle_indices.size() : vertices.size();
    if (emitted == 0u || (transform.primitive == 4u ? emitted < 3u : (emitted % 3u) != 0u)) return;
    const std::size_t append = indexed ? vertices.size() : emitted;
    const std::size_t extra_indices = indexed ? triangle_indices.size() : 0u;
    const std::size_t used = s.vertices.size() * sizeof(UploadVertex) + s.indices.size() * sizeof(std::uint32_t);
    if (used + append * sizeof(UploadVertex) + extra_indices * sizeof(std::uint32_t) > kGeometryUploadCapacity) {
        ++s.report.game_vertex_overflows;
        return;
    }
    const std::uint32_t first_vertex = static_cast<std::uint32_t>(s.vertices.size());
    const std::uint32_t first_index = static_cast<std::uint32_t>(s.indices.size());
    for (const GeGpuVertex &source : vertices) s.vertices.push_back(make_upload_vertex(source));
    if (indexed) {
        for (std::uint32_t index : triangle_indices) {
            if (index >= vertices.size()) {
                s.vertices.resize(first_vertex);
                s.indices.resize(first_index);
                ++s.report.game_vertex_overflows;
                return;
            }
            s.indices.push_back(index);
        }
    }
    const std::uint32_t logical = std::max<std::uint32_t>(1u, transform.logical_prim_batches);
    Batch batch{};
    batch.draw = draw;
    batch.first_vertex = first_vertex;
    batch.vertex_count = static_cast<std::uint32_t>(append);
    batch.first_index = first_index;
    batch.index_count = indexed ? static_cast<std::uint32_t>(triangle_indices.size()) : 0u;
    batch.indexed = indexed;
    batch.logical_draw_count = logical;
    batch.hardware_transform = true;
    batch.transform = transform;
    batch.feedback_address = draw.texture_address & 0x001FFFF0u;
    batch.framebuffer_feedback = draw.texture_enabled && find_target(s, batch.feedback_address) != nullptr;
    s.batches.push_back(batch);
    s.report.game_draw_calls += logical;
    s.report.game_triangles += transform.primitive == 4u
        ? (emitted > 2u ? emitted - 2u : 0u) : emitted / 3u;
    s.report.game_vertices += emitted;
    s.report.hw_transform_draw_calls += logical;
    s.report.hw_transform_vertices += vertices.size();
    const bool sampled = draw.texture_enabled && ge_gpu_backend_texture_available(draw);
    if (draw.texture_enabled && sampled) s.report.textured_game_draw_calls += logical;
    else if (draw.texture_enabled) s.report.game_textured_draws_without_texture += logical;
}

bool ge_gpu_backend_accumulate_hardware_packed_0115(
    const GeGpuDrawDescriptor &draw, const GeGpuHardwareTransform &transform,
    std::span<const std::byte> packed_vertices, std::uint32_t vertex_count,
    std::span<const std::uint32_t> triangle_indices) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled || vertex_count == 0u ||
        packed_vertices.size() != static_cast<std::size_t>(vertex_count) * sizeof(Packed0115))
        return false;
    // Reused between draws: a fresh vector per draw was a heap round trip each time.
    static thread_local std::vector<GeGpuVertex> decoded;
    decoded.resize(vertex_count);
    for (std::uint32_t index = 0u; index < vertex_count; ++index) {
        Packed0115 packed{};
        std::memcpy(&packed, packed_vertices.data() + static_cast<std::size_t>(index) * sizeof(Packed0115),
                    sizeof(Packed0115));
        const UploadVertex upload = decode_packed(packed);
        GeGpuVertex vertex{};
        vertex.x = upload.x;
        vertex.y = upload.y;
        vertex.z = upload.z;
        vertex.w = upload.w;
        vertex.rgba = upload.rgba;
        vertex.u = upload.u;
        vertex.v = upload.v;
        vertex.q = upload.q;
        vertex.fog_factor = upload.fog_factor;
        decoded[index] = vertex;
    }
    ge_gpu_backend_accumulate_hardware_triangles(draw, transform, decoded, triangle_indices);
    return true;
}

void ge_gpu_backend_set_native_window(void *native) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    auto *window = static_cast<SDL_Window *>(native);
    if (window == s.window) return;
    if (s.context != nullptr) {
        // The context belongs to the first window; a new window means a new backend.
        std::cerr << "[gl] window changed; the OpenGL GE keeps its first window\n";
        return;
    }
    s.window = window;
}

void ge_gpu_backend_set_display_framebuffer(std::uint32_t address, std::uint32_t logical_width,
                                            std::uint32_t logical_height) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    s.display_framebuffer = address & 0x001FFFF0u;
    s.display_logical_width = logical_width != 0u ? logical_width : kReferenceWidth;
    s.display_logical_height = logical_height != 0u ? logical_height : kReferenceHeight;
    if (!s.enabled) return;
    ensure_target(s, s.display_framebuffer);
    note_logical_size(s, s.display_framebuffer, s.display_logical_width, s.display_logical_height);
}

void ge_gpu_backend_display_logical_size(std::uint32_t &width, std::uint32_t &height) noexcept {
    const GlState &s = state();
    width = s.display_logical_width;
    height = s.display_logical_height;
}

bool ge_gpu_backend_finish_color_frame(std::uint64_t vblank) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    const bool publish = ge_finish_shows_this_frame();
    const auto reset_frame = [&]() {
        s.vertices.clear();
        s.indices.clear();
        s.batches.clear();
        s.plan.clear();
        ++s.frame_epoch;
    };
    if (!s.enabled || s.batches.empty()) {
        reset_frame();
        return false;
    }
    const std::size_t vertex_bytes = s.vertices.size() * sizeof(UploadVertex);
    const std::size_t index_bytes = s.indices.size() * sizeof(std::uint32_t);
    if (vertex_bytes + index_bytes > kGeometryUploadCapacity) {
        ++s.report.game_vertex_overflows;
        reset_frame();
        return false;
    }
    ContextScope scope(s);
    if (!scope.ok()) {
        static std::uint64_t failures = 0u;
        if (++failures <= 5u) std::cerr << "[gl] could not make the context current: " << SDL_GetError() << "\n";
        reset_frame();
        return false;
    }

    // Uploads: textures decoded this frame, geometry.
    evict_stale_textures(s);
    for (auto &entry : s.textures) upload_texture(s, entry.second);
    gl.BindVertexArray(s.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, s.vertex_buffer);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertex_bytes), nullptr, GL_STREAM_DRAW);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertex_bytes), s.vertices.data());
    if (index_bytes != 0u) {
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.index_buffer);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(index_bytes), nullptr, GL_STREAM_DRAW);
        gl.BufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(index_bytes), s.indices.data());
    }

    // Plan every batch and lay out its uniforms, so they go up at once.
    std::string error;
    const std::size_t draws = std::min<std::size_t>(s.batches.size(), kMaxDraws);
    s.plan.assign(draws, PlannedDraw{});
    s.uniform_bytes.assign(draws * s.uniform_align, std::byte{0});
    for (std::size_t index = 0u; index < draws; ++index) {
        const Batch &batch = s.batches[index];
        PlannedDraw &plan = s.plan[index];
        ensure_target(s, batch.draw.framebuffer_address);
        Target *target = find_target(s, batch.draw.framebuffer_address);
        if (target == nullptr || !realize_target(s, *target, error)) {
            if (!error.empty()) runtime_log_error("opengl framebuffer target", error);
            error.clear();
            continue;
        }
        plan.target = target;
        if (batch.draw.texture_enabled) {
            if (batch.framebuffer_feedback && batch.feedback_address == target->address) {
                plan.source = SourceKind::TargetSnapshot;
                plan.source_target = target;
                plan.textured = true;
            } else if (batch.framebuffer_feedback) {
                Target *feedback = find_target(s, batch.feedback_address);
                if (feedback != nullptr && realize_target(s, *feedback, error)) {
                    plan.source = SourceKind::TargetColor;
                    plan.source_target = feedback;
                    plan.textured = true;
                }
                error.clear();
            } else if (const auto found = s.textures.find(texture_key(batch.draw));
                       found != s.textures.end() && found->second.texture != 0u && !found->second.dirty) {
                plan.source = SourceKind::Texture;
                plan.texture = found->second.texture;
                plan.textured = true;
            }
        }
        const DrawUniforms uniforms =
            make_uniforms(batch, target->logical_width, target->logical_height, plan.textured);
        std::memcpy(s.uniform_bytes.data() + index * s.uniform_align, &uniforms, sizeof(uniforms));
    }
    gl.BindBuffer(GL_UNIFORM_BUFFER, s.uniform_buffer);
    gl.BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(s.uniform_bytes.size()), s.uniform_bytes.data(),
                  GL_STREAM_DRAW);
    check_gl(s, "frame uploads");

    gl.UseProgram(s.program);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.Enable(GL_SCISSOR_TEST);
    s.cache = CachedState{};
    s.bound_target = nullptr;
    bool touched_display = false;
    for (std::size_t index = 0u; index < draws; ++index) {
        const Batch &batch = s.batches[index];
        const PlannedDraw &plan = s.plan[index];
        Target *target = plan.target;
        if (target == nullptr) continue;
        begin_target(s, *target);
        if (target->address == s.display_framebuffer) touched_display = true;
        const auto scale = [&](std::int32_t value, std::uint32_t logical, std::uint32_t target_size) {
            return static_cast<std::int32_t>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(value) * target_size / std::max<std::uint32_t>(1u, logical),
                0, target_size));
        };
        const std::int32_t left = scale(batch.draw.scissor_x0, target->logical_width, s.target_width);
        const std::int32_t top = scale(batch.draw.scissor_y0, target->logical_height, s.target_height);
        const std::int32_t right = scale(batch.draw.scissor_x1 + 1, target->logical_width, s.target_width);
        const std::int32_t bottom = scale(batch.draw.scissor_y1 + 1, target->logical_height, s.target_height);
        if (right <= left || bottom <= top) continue;
        // Row 0 is the PSP's top line (see the top of this file).
        const std::array<GLint, 4> scissor{left, top, right - left, bottom - top};
        if (!s.cache.valid || s.cache.scissor != scissor) {
            gl.Scissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            s.cache.scissor = scissor;
        }
        const bool strip = batch.hardware_transform && batch.transform.primitive == 4u;
        const bool cull = batch.hardware_transform && batch.transform.cull_enabled;
        const bool ccw = cull && batch.transform.accept_counter_clockwise;
        apply_draw_state(s, batch.draw, cull, ccw);

        GLuint texture = s.white;
        GLuint sampler = s.white_sampler;
        switch (plan.source) {
        case SourceKind::Texture:
            texture = plan.texture;
            sampler = sampler_for(s, batch.draw);
            break;
        case SourceKind::TargetColor:
            texture = plan.source_target->color;
            sampler = sampler_for(s, batch.draw);
            ++s.report.vram_feedback_refreshes;
            ++s.report.gpu_feedback_draws;
            break;
        case SourceKind::TargetSnapshot:
            if (snapshot_target(s, *plan.source_target)) {
                texture = plan.source_target->snapshot;
                sampler = sampler_for(s, batch.draw);
                ++s.report.vram_feedback_refreshes;
                ++s.report.gpu_feedback_draws;
                ++s.report.self_feedback_snapshots;
            }
            break;
        case SourceKind::White:
            break;
        }
        // A snapshot rebinds unit 0's texture, so it always rebinds here.
        if (s.cache.texture != texture || plan.source == SourceKind::TargetSnapshot) {
            gl.BindTexture(GL_TEXTURE_2D, texture);
            s.cache.texture = texture;
        }
        if (s.cache.sampler != sampler) {
            gl.BindSampler(0u, sampler);
            s.cache.sampler = sampler;
        }
        gl.BindBufferRange(GL_UNIFORM_BUFFER, 0u, s.uniform_buffer,
                           static_cast<GLintptr>(index * s.uniform_align), sizeof(DrawUniforms));
        const GLenum mode = strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
        if (batch.indexed)
            gl.DrawElementsBaseVertex(mode, static_cast<GLsizei>(batch.index_count), GL_UNSIGNED_INT,
                                      reinterpret_cast<const void *>(
                                          static_cast<std::size_t>(batch.first_index) * sizeof(std::uint32_t)),
                                      static_cast<GLint>(batch.first_vertex));
        else
            gl.DrawArrays(mode, static_cast<GLint>(batch.first_vertex), static_cast<GLsizei>(batch.vertex_count));
        count_draw(s, batch, plan.textured);
    }
    gl.BindSampler(0u, 0u);
    gl.BindVertexArray(0u);
    check_gl(s, "frame draws");

    Target *display = find_target(s, s.display_framebuffer);
    const bool display_ready = touched_display && display != nullptr && display->framebuffer != 0u;
    bool presented_now = false;
    bool read_back = false;
    if (publish && display_ready) {
        if (!s.readback_mode) {
            presented_now = present_to_window(s, display->color, s.target_width, s.target_height);
            s.direct_present_ok = s.direct_present_ok || presented_now;
        }
        if (!presented_now) {
            s.frame_rgba.resize(static_cast<std::size_t>(s.target_width) * s.target_height * 4u);
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, display->framebuffer);
            gl.ReadPixels(0, 0, static_cast<GLsizei>(s.target_width), static_cast<GLsizei>(s.target_height),
                          GL_RGBA, GL_UNSIGNED_BYTE, s.frame_rgba.data());
            read_back = true;
            s.report.game_frame_readback_bytes = s.frame_rgba.size();
        }
    }
    if (!display_ready) ++s.report.frames_without_displayed_target;
    ++s.report.game_frames;
    s.report.game_frame_vblank = vblank;
    s.report.offscreen_width = s.target_width;
    s.report.offscreen_height = s.target_height;
    s.report.presented_framebuffer_target = display_ready ? s.display_framebuffer : 0u;
    s.report.gpu_frame_presented_to_window = presented_now;
    s.report.swapchain_active = !s.readback_mode;
    reset_frame();
    return presented_now || read_back;
}

bool ge_gpu_backend_copy_game_frame_rgba(std::span<std::byte> destination) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (s.frame_rgba.empty() || destination.size() < s.frame_rgba.size()) return false;
    std::memcpy(destination.data(), s.frame_rgba.data(), s.frame_rgba.size());
    return true;
}

bool ge_gpu_backend_presents_directly() noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    return s.enabled && !s.readback_mode && s.direct_present_ok;
}

static bool gl_guest_present_available() noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    return s.enabled && !s.readback_mode;
}

// A CPU-made frame (videos, the software path's picture) shown in the window.
static bool gl_present_guest_rgba(std::span<const std::byte> rgba, std::uint32_t width,
                                  std::uint32_t height) noexcept {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if (!s.enabled || s.readback_mode) return false;
    if (width == 0u || height == 0u || width > 8192u || height > 8192u) return false;
    if (rgba.size() < static_cast<std::size_t>(width) * height * 4u) return false;
    ContextScope scope(s);
    if (!scope.ok()) return false;
    gl.ActiveTexture(GL_TEXTURE0);
    if (s.guest_image == 0u || s.guest_width != width || s.guest_height != height) {
        if (s.guest_image != 0u) gl.DeleteTextures(1, &s.guest_image);
        s.guest_image = make_texture_2d(width, height, rgba.data());
        s.guest_width = width;
        s.guest_height = height;
    } else {
        gl.BindTexture(GL_TEXTURE_2D, s.guest_image);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    return present_to_window(s, s.guest_image, width, height);
}

namespace {
struct RegisterGlGuestPresent {
    RegisterGlGuestPresent() noexcept {
        ge_guest_present().available = gl_guest_present_available;
        ge_guest_present().present = gl_present_guest_rgba;
    }
};
const RegisterGlGuestPresent register_gl_guest_present{};
}  // namespace

std::uint32_t ge_gpu_backend_owned_framebuffer() noexcept {
    const GlState &s = state();
    return s.enabled ? s.display_framebuffer : 0u;
}
std::uint32_t ge_gpu_backend_display_framebuffer() noexcept { return state().display_framebuffer; }
std::span<const std::byte> ge_gpu_backend_game_frame_rgba() noexcept {
    const GlState &s = state();
    return s.frame_rgba.empty() ? std::span<const std::byte>{}
                                : std::span<const std::byte>(s.frame_rgba.data(), s.frame_rgba.size());
}
bool ge_gpu_backend_copy_offscreen_rgba(std::span<std::byte> destination) noexcept {
    return ge_gpu_backend_copy_game_frame_rgba(destination);
}
void ge_gpu_backend_mark_window_presented() noexcept {
    state().report.gpu_frame_presented_to_window = true;
}
GeGpuBackendReport ge_gpu_backend_report() {
    GlState &s = state();
    std::lock_guard<std::recursive_mutex> guard(s.mutex);
    return s.report;
}

}  // namespace lcs

#endif  // LCS_GL_GE_BACKEND
