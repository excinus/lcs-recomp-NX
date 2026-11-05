// Nintendo Switch (Atmosphere homebrew) platform layer.
//
// Compiled only into the Switch build (see switch.cmake). The PC builds never
// see this file. What it does, in the order it happens:
//
//  1. __wrap_main (the .nro's real entry, through -Wl,--wrap=main) finds the
//     folder the .nro was started from, makes it the working folder, keeps the
//     previous log and sends stdout/stderr to lcs_log.txt there.
//  2. It reads lcs_env.txt (KEY=VALUE lines) into the environment: the Switch
//     has no environment variables, and the runtime is tuned through them.
//  3. It checks that game/EBOOT.ELF is there, and if not shows what to do.
//  4. It runs the PC main() on a thread with a 64 MiB stack pinned to core 0.
//
// --wrap=pthread_create puts every other thread on cores 1 and 2.
//
// Clocks are the player's business (sys-clk, Ultrahand): the game leaves them
// alone.

#if defined(__SWITCH__)

#include <switch.h>

#include "ge_gpu_backend.hpp"
#include "lcs_audio_output.hpp"
#include "lcs_profile.hpp"
#include "lcs_display_menu.hpp"
#include "lcs_switch_hooks.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef LCS_SWITCH_VERSION_STRING
#define LCS_SWITCH_VERSION_STRING "dev"
#endif

extern "C" {
void _start();  // first byte of the program: profiler offsets are relative to it
int __real_main(int argc, char **argv);
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg);
int __wrap_main(int argc, char **argv);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg);
}

namespace {

constexpr const char *kLogName = "lcs_log.txt";
constexpr const char *kPreviousLogName = "lcs_log_prev.txt";
constexpr const char *kEnvName = "lcs_env.txt";
constexpr std::size_t kGameStackBytes = 64u * 1024u * 1024u;

// Cores an application may use. Core 3 belongs to the system.
constexpr u32 kGameCoreMask = 1u << 0;
constexpr u32 kWorkerCoreMask = (1u << 1) | (1u << 2);

std::string g_base_dir;  // "/switch/LCSNative": no "sdmc:" so std::filesystem sees it as absolute

// Set while the log and watch threads start, so the profiler leaves them out.
thread_local bool t_untracked_spawn = false;

bool starts_with(const std::string &text, const char *prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    return text.substr(start);
}

// argv[0] is "sdmc:/switch/LCSNative/LCSNative.nro" from the homebrew menu.
// newlib on libnx resolves a path that starts with '/' on the SD card, and
// std::filesystem treats only those as absolute, so drop the device name.
std::string find_base_dir(int argc, char **argv) {
    std::string path = argc > 0 && argv[0] != nullptr ? argv[0] : "";
    if (starts_with(path, "sdmc:")) path.erase(0, 5);
    const std::size_t slash = path.find_last_of('/');
    if (path.empty() || path[0] != '/' || slash == std::string::npos) return "/switch/LCSNative";
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string in_base(const char *name) {
    return g_base_dir == "/" ? std::string("/") + name : g_base_dir + "/" + name;
}

bool file_exists(const std::string &path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

// ---------------------------------------------------------------------------
// Log file. Every write to the SD card is a request to the file service, and a
// line-by-line stderr costs milliseconds per line, so both streams are fully
// buffered and a thread flushes them twice a second. One handle owns the file:
// a second open of the same file for writing fails on Horizon.

std::atomic<bool> g_log_flusher_stop{false};
std::thread g_log_flusher;
std::vector<char> g_stdout_buffer;
std::vector<char> g_stderr_buffer;

void open_log() {
    const std::string log_path = in_base(kLogName);
    const std::string previous_path = in_base(kPreviousLogName);
    std::remove(previous_path.c_str());
    std::rename(log_path.c_str(), previous_path.c_str());
    const int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return;
    }
    close(fd);
    g_stdout_buffer.resize(64u * 1024u);
    g_stderr_buffer.resize(64u * 1024u);
    std::setvbuf(stdout, g_stdout_buffer.data(), _IOFBF, g_stdout_buffer.size());
    std::setvbuf(stderr, g_stderr_buffer.data(), _IOFBF, g_stderr_buffer.size());
    // std::cerr flushes after every insertion (unitbuf), which would reach the
    // card for each message. The flusher thread does it instead.
    std::cerr.unsetf(std::ios::unitbuf);
    t_untracked_spawn = true;
    g_log_flusher = std::thread([] {
        while (!g_log_flusher_stop.load(std::memory_order_relaxed)) {
            svcSleepThread(500'000'000ll);
            std::fflush(stdout);
            std::fflush(stderr);
        }
    });
    t_untracked_spawn = false;
}

void close_log() {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    g_log_flusher_stop.store(true, std::memory_order_relaxed);
    if (g_log_flusher.joinable()) g_log_flusher.join();
}

// ---------------------------------------------------------------------------
// LCSNative.ini is the player's: the release ships LCSNative.default.ini and
// the game makes LCSNative.ini from it only when there is none, so updating
// by copying a new zip keeps the settings. An LCSNative.ini without a
// [Switch] section comes from the first test build (it had other defaults);
// it is kept as LCSNative.ini.old and replaced.

bool copy_file(const std::string &from, const std::string &to) {
    std::FILE *in = std::fopen(from.c_str(), "rb");
    if (in == nullptr) return false;
    std::FILE *out = std::fopen(to.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    char buffer[16384];
    std::size_t got = 0;
    bool ok = true;
    while ((got = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
        ok = ok && std::fwrite(buffer, 1, got, out) == got;
    std::fclose(in);
    return std::fclose(out) == 0 && ok;
}

bool has_switch_section(const std::string &path) {
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (file == nullptr) return false;
    char line[512];
    bool found = false;
    while (!found && std::fgets(line, sizeof(line), file) != nullptr) {
        std::string text = trim(line);
        for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        found = text == "[switch]";
    }
    std::fclose(file);
    return found;
}

void prepare_settings_file() {
    const std::string ini = in_base("LCSNative.ini");
    const std::string defaults = in_base("LCSNative.default.ini");
    if (!file_exists(defaults)) return;
    if (file_exists(ini)) {
        if (has_switch_section(ini)) return;
        const std::string old = in_base("LCSNative.ini.old");
        std::remove(old.c_str());
        std::rename(ini.c_str(), old.c_str());
        std::printf("[switch] LCSNative.ini from an older build kept as LCSNative.ini.old\n");
    }
    if (copy_file(defaults, ini)) std::printf("[switch] LCSNative.ini made from LCSNative.default.ini\n");
}

// ---------------------------------------------------------------------------
// lcs_env.txt: KEY=VALUE per line, '#' or ';' starts a comment. Values set
// here win over the defaults below.

void load_env_file() {
    std::FILE *file = std::fopen(in_base(kEnvName).c_str(), "r");
    if (file == nullptr) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(text.substr(0, equals));
        const std::string value = trim(text.substr(equals + 1));
        if (key.empty()) continue;
        setenv(key.c_str(), value.c_str(), 1);
        std::printf("[switch] env %s=%s\n", key.c_str(), value.c_str());
    }
    std::fclose(file);
}

void set_default_env() {
    // Thread layout: the game alone on core 0; the GE worker, one raster
    // worker and one draw-decode worker share cores 1 and 2.
    // hardware_concurrency() is 0 on Horizon (no sysconf), so the pools are
    // sized here.
    setenv("PSPRECOMP_RASTER_THREADS", "2", 0);
    setenv("PSPRECOMP_GE_DECODE_THREADS", "1", 0);
    // Mesa: skip per-call GL error checks.
    setenv("MESA_NO_ERROR", "1", 0);
    // Sound is mixed as the game's time advances, once per vblank wait. A slow
    // frame (125 ms at 8 fps) outlasted the PC's 69 ms queue and the sound
    // broke up; 12 blocks of 512 frames is 139 ms.
    setenv("PSPRECOMP_AUDIO_PREBUFFER_BLOCKS", "12", 0);
}

bool env_enabled(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "off") != 0;
}

// ---------------------------------------------------------------------------
// System facts for the log, so a report can be read without asking.

void log_system() {
    const AppletType type = appletGetAppletType();
    const bool application =
        type == AppletType_Application || type == AppletType_SystemApplication;
    std::printf("[switch] base %s, applet type %d%s\n", g_base_dir.c_str(), static_cast<int>(type),
                application ? " (application memory)" : " (APPLET MODE: start through a game, hold R)");
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysProductModel model = SetSysProductModel_Invalid;
        if (R_SUCCEEDED(setsysGetProductModel(&model)))
            std::printf("[switch] product model %d\n", static_cast<int>(model));
        SetSysFirmwareVersion firmware{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware)))
            std::printf("[switch] firmware %s\n", firmware.display_version);
        setsysExit();
    }
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    std::printf("[switch] memory %llu MiB total, %llu MiB used at start\n",
                static_cast<unsigned long long>(total >> 20),
                static_cast<unsigned long long>(used >> 20));
}

// ---------------------------------------------------------------------------
// A text screen for problems found before the game starts (no game files).

void show_message_and_wait(const std::string &message) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    std::printf("%s\n\nPress + to exit.\n", message.c_str());
    consoleUpdate(nullptr);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
        svcSleepThread(16'000'000ll);
    }
    consoleExit(nullptr);
}

// ---------------------------------------------------------------------------
// Performance watch.

using lcs::GeGpuBackendReport;
using lcs::ge_gpu_backend_report;
using lcs::host_fps_format;
//
// Every 5 s, one log line: frame rate, each thread's share of a core, memory,
// and the GPU backend's counters for the interval. A sampling profiler pauses
// each known thread about 100 times a second (svcSetThreadActivity), reads its
// pc and lr (svcGetThreadContext3) and resumes it; the log names the hottest
// addresses, and every sample goes to lcs_samples.bin. Addresses are offsets
// from the program's first byte, so lcs/switch/symbolize.py turns them into
// function names with the matching LCSNative.elf (a CI artifact).
// LCS_NX_PROFILE=0 turns the sampling off; the log line stays.
//
// Between pausing a thread and resuming it the profiler makes only kernel
// calls and writes into a fixed buffer: the paused thread may hold the heap
// or a stdio lock.

constexpr u64 kTicksPerSecond = 19200000ull;
constexpr std::size_t kMaxTracked = 32u;

struct TrackedThread {
    Handle handle{};
    char name[16]{};
    u64 last_ticks{};
    bool alive{true};
};

struct Sample {
    u32 time_ms;
    u8 thread;
    u8 reserved[3];
    u64 pc;
    u64 lr;
};
static_assert(sizeof(Sample) == 24u);

std::mutex g_tracked_mutex;
std::array<TrackedThread, kMaxTracked> g_tracked{};
std::atomic<std::size_t> g_tracked_count{0};
std::atomic<bool> g_watch_stop{false};
std::thread g_watch;
u64 g_image_base{};

void track_current_thread(const char *name) {
    std::lock_guard<std::mutex> guard(g_tracked_mutex);
    const std::size_t index = g_tracked_count.load();
    if (index >= kMaxTracked) return;
    TrackedThread &thread = g_tracked[index];
    thread.handle = threadGetCurHandle();
    std::snprintf(thread.name, sizeof(thread.name), "%s", name);
    thread.alive = true;
    g_tracked_count.store(index + 1u);
}

u64 thread_ticks(Handle handle, bool &ok) {
    u64 ticks = 0;
    ok = R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)) ||
         R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, handle, TickCountInfo_Total));
    return ticks;
}

// Pauses one thread, reads where it is and lets it go. False if it is gone.
bool sample_thread(Handle handle, u64 &pc, u64 &lr) {
    if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) return false;
    ThreadContext context{};
    bool got = false;
    for (int attempt = 0; attempt < 64 && !got; ++attempt) {
        got = R_SUCCEEDED(svcGetThreadContext3(&context, handle));
        if (!got) svcSleepThread(0);
    }
    svcSetThreadActivity(handle, ThreadActivity_Runnable);
    if (!got) return false;
    pc = context.pc.x;
    lr = context.lr;
    return true;
}

std::string hex_offset(u64 address) {
    char text[24];
    if (address >= g_image_base && address - g_image_base < 0x40000000ull)
        std::snprintf(text, sizeof(text), "+%llx", static_cast<unsigned long long>(address - g_image_base));
    else
        std::snprintf(text, sizeof(text), "@%llx", static_cast<unsigned long long>(address));
    return text;
}

void watch_main() {
    const bool sampling = env_enabled("LCS_NX_PROFILE", true);
    const std::string samples_path = in_base("lcs_samples.bin");
    if (sampling) {
        std::FILE *file = std::fopen(samples_path.c_str(), "wb");
        if (file != nullptr) {
            const char magic[16] = "LCSSAMPLES1";
            std::fwrite(magic, 1, sizeof(magic), file);
            std::fclose(file);
        }
    }
    std::vector<Sample> pending;
    pending.reserve(8192u);
    std::array<Sample, 64> burst{};
    std::array<std::unordered_map<u64, u32>, kMaxTracked> hot{};
    const u64 start = armGetSystemTick();
    u64 last_report = start;
    GeGpuBackendReport last_report_gpu{};
    lcs::AudioOutputStats last_audio{};
    lcs::GeWaitStats last_waits{};
    while (!g_watch_stop.load()) {
        svcSleepThread(10'000'000ll);
        const std::size_t count = g_tracked_count.load();
        if (sampling) {
            std::size_t taken = 0u;
            const u32 now_ms = static_cast<u32>((armGetSystemTick() - start) * 1000ull / kTicksPerSecond);
            for (std::size_t index = 0u; index < count && taken < burst.size(); ++index) {
                if (!g_tracked[index].alive) continue;
                u64 pc = 0, lr = 0;
                if (!sample_thread(g_tracked[index].handle, pc, lr)) continue;
                burst[taken++] = Sample{now_ms, static_cast<u8>(index), {}, pc - g_image_base, lr - g_image_base};
            }
            for (std::size_t index = 0u; index < taken; ++index) {
                pending.push_back(burst[index]);
                ++hot[burst[index].thread][(burst[index].pc) & ~u64{0xF}];
            }
        }
        const u64 now = armGetSystemTick();
        if (now - last_report < 5ull * kTicksPerSecond) continue;
        const double seconds = double(now - last_report) / double(kTicksPerSecond);
        last_report = now;

        static std::size_t named_threads = 0u;
        if (count != named_threads) {
            std::string names = "[watch] threads:";
            for (std::size_t index = 0u; index < count; ++index)
                names += " " + std::to_string(index) + "=" + g_tracked[index].name;
            std::printf("%s\n", names.c_str());
            named_threads = count;
        }
        char fps[24];
        host_fps_format(fps, sizeof(fps));
        std::string line = "[watch] " + std::to_string((now - start) / kTicksPerSecond) + "s " + fps + " |";
        for (std::size_t index = 0u; index < count; ++index) {
            TrackedThread &thread = g_tracked[index];
            if (!thread.alive) continue;
            bool ok = false;
            const u64 ticks = thread_ticks(thread.handle, ok);
            if (!ok) {
                thread.alive = false;
                continue;
            }
            const double busy = double(ticks - thread.last_ticks) / double(kTicksPerSecond) / seconds * 100.0;
            thread.last_ticks = ticks;
            char part[48];
            std::snprintf(part, sizeof(part), " %s %.0f%%", thread.name, busy);
            line += part;
        }
        u64 used = 0;
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        const GeGpuBackendReport gpu = ge_gpu_backend_report();
        const double frames = double(gpu.game_frames - last_report_gpu.game_frames);
        char tail[200];
        std::snprintf(tail, sizeof(tail),
                      " | mem %llu MiB | gpu frames %.1f/s draws/frame %.0f tex uploads %.1f/s decodes %.1f/s",
                      static_cast<unsigned long long>(used >> 20), frames / seconds,
                      frames > 0.0 ? double(gpu.game_draw_calls - last_report_gpu.game_draw_calls) / frames : 0.0,
                      double(gpu.texture_image_uploads - last_report_gpu.texture_image_uploads) / seconds,
                      double(gpu.decoded_texture_uploads - last_report_gpu.decoded_texture_uploads) / seconds);
        line += tail;
        last_report_gpu = gpu;
        const lcs::AudioOutputStats audio = lcs::audio_output_stats();
        std::snprintf(tail, sizeof(tail), " | audio gaps %llu skips %llu",
                      static_cast<unsigned long long>(audio.underruns - last_audio.underruns),
                      static_cast<unsigned long long>(audio.dropped_blocks - last_audio.dropped_blocks));
        line += tail;
        last_audio = audio;
        const lcs::GeWaitStats waits = lcs::ge_wait_stats();
        std::snprintf(tail, sizeof(tail), " | game waits GE: enqueue %.0f%% sync %.0f%%",
                      double(waits.enqueue_us - last_waits.enqueue_us) / 1e4 / seconds,
                      double(waits.sync_us - last_waits.sync_us) / 1e4 / seconds);
        line += tail;
        last_waits = waits;
        std::printf("%s\n", line.c_str());

        if (sampling) {
            for (std::size_t index = 0u; index < count; ++index) {
                auto &histogram = hot[index];
                u64 total = 0;
                for (const auto &entry : histogram) total += entry.second;
                if (total < 20u) {
                    histogram.clear();
                    continue;
                }
                std::vector<std::pair<u32, u64>> top;
                for (const auto &entry : histogram) top.emplace_back(entry.second, entry.first);
                std::partial_sort(top.begin(), top.begin() + std::min<std::size_t>(6u, top.size()), top.end(),
                                  [](const auto &a, const auto &b) { return a.first > b.first; });
                std::string hot_line = std::string("[watch]   ") + g_tracked[index].name + " hot:";
                for (std::size_t rank = 0u; rank < std::min<std::size_t>(6u, top.size()); ++rank) {
                    char part[48];
                    std::snprintf(part, sizeof(part), " +%llx %.0f%%",
                                  static_cast<unsigned long long>(top[rank].second),
                                  100.0 * top[rank].first / double(total));
                    hot_line += part;
                }
                std::printf("%s\n", hot_line.c_str());
                histogram.clear();
            }
            std::FILE *file = std::fopen(samples_path.c_str(), "ab");
            if (file != nullptr) {
                std::fwrite(pending.data(), sizeof(Sample), pending.size(), file);
                std::fclose(file);
            }
            pending.clear();
        }
    }
}

void start_watch() {
    g_image_base = reinterpret_cast<u64>(&_start);
    std::printf("[switch] image base %s (offsets in [watch] lines are from it)\n",
                hex_offset(g_image_base).c_str());
    t_untracked_spawn = true;
    g_watch = std::thread(watch_main);
    t_untracked_spawn = false;
}

void stop_watch() {
    g_watch_stop.store(true);
    if (g_watch.joinable()) g_watch.join();
}

// ---------------------------------------------------------------------------
// Threads.

struct GameThreadArgs {
    int argc;
    char **argv;
    int result;
};

void *game_thread_main(void *param) {
    auto *args = static_cast<GameThreadArgs *>(param);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, kGameCoreMask);
    track_current_thread("game");
    args->result = __real_main(args->argc, args->argv);
    return nullptr;
}

struct ThreadStart {
    void *(*routine)(void *);
    void *arg;
    s32 core;
    unsigned index;
    bool tracked;
};


std::atomic<unsigned> g_threads_started{0};

void *worker_thread_main(void *param) {
    ThreadStart start = *static_cast<ThreadStart *>(param);
    delete static_cast<ThreadStart *>(param);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, start.core, kWorkerCoreMask);
    char name[16];
    std::snprintf(name, sizeof(name), "t%u", static_cast<unsigned>(start.index));
    if (start.tracked) track_current_thread(name);
    return start.routine(start.arg);
}

}  // namespace

// Linux gives a thread 8 MiB of stack and the PC code was written against
// that; libnx's default is far smaller. The GE worker runs the rasterizer,
// texture decoding and, through Mesa, shader compiles.
constexpr std::size_t kMinThreadStackBytes = 8u * 1024u * 1024u;

extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                     void *(*start_routine)(void *), void *arg) {
    pthread_attr_t sized{};
    if (attr != nullptr) sized = *attr;
    else pthread_attr_init(&sized);
    std::size_t stack = 0u;
    pthread_attr_getstacksize(&sized, &stack);
    if (stack < kMinThreadStackBytes) pthread_attr_setstacksize(&sized, kMinThreadStackBytes);
    const unsigned index = g_threads_started.fetch_add(1);
    auto *start = new ThreadStart{start_routine, arg, 1 + static_cast<s32>(index % 2u), index,
                                  !t_untracked_spawn};
    const int result = __real_pthread_create(thread, &sized, worker_thread_main, start);
    if (result != 0) delete start;
    return result;
}

extern "C" void lcs_switch_name_current_thread(const char *name) {
    const Handle self = threadGetCurHandle();
    {
        std::lock_guard<std::mutex> guard(g_tracked_mutex);
        for (std::size_t index = 0u; index < g_tracked_count.load(); ++index) {
            if (g_tracked[index].handle != self) continue;
            std::snprintf(g_tracked[index].name, sizeof(g_tracked[index].name), "%s", name);
            return;
        }
    }
    track_current_thread(name);
}

extern "C" void userAppInit() {
    // Sockets are needed by nxlink and by anything that calls sysctl.
    socketInitializeDefault();
}

extern "C" void userAppExit() {
    socketExit();
}

extern "C" int __wrap_main(int argc, char **argv) {
    g_base_dir = find_base_dir(argc, argv);
    chdir(g_base_dir.c_str());
    open_log();
    std::printf("[switch] LCSNative for Nintendo Switch, version %s\n", LCS_SWITCH_VERSION_STRING);
    load_env_file();
    set_default_env();
    prepare_settings_file();
    log_system();
    start_watch();

    const std::string elf_path = in_base("game/EBOOT.ELF");
    if (!file_exists(elf_path)) {
        std::printf("[switch] missing %s\n", elf_path.c_str());
        stop_watch();
        close_log();
        show_message_and_wait(
            "GTA: Liberty City Stories (LCS Recomp)\n\n"
            "The game files are missing. Put them here on the SD card:\n\n"
            "  " + g_base_dir + "/game/EBOOT.ELF   (decrypted, US v1.05, ULUS-10041)\n"
            "  " + g_base_dir + "/game/PSP_GAME/   (the disc's PSP_GAME folder)\n\n"
            "The PC launcher (LCSLauncher.exe) extracts both from your ISO.");
        return 1;
    }

    // The PC main() reads its arguments. Give it absolute, device-free paths.
    static std::string program = in_base("LCSNative.nro");
    static std::string game_root = in_base("game");
    std::vector<char *> arguments;
    arguments.push_back(program.data());
    arguments.push_back(const_cast<char *>("--game"));
    arguments.push_back(game_root.data());
    for (int index = 1; index < argc; ++index) arguments.push_back(argv[index]);
    arguments.push_back(nullptr);

    GameThreadArgs args{static_cast<int>(arguments.size() - 1u), arguments.data(), 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kGameStackBytes);
    pthread_t game_thread{};
    if (__real_pthread_create(&game_thread, &attr, game_thread_main, &args) == 0) {
        pthread_join(game_thread, nullptr);
    } else {
        std::printf("[switch] could not start the game thread with a %zu MiB stack; using main\n",
                    kGameStackBytes >> 20);
        args.result = __real_main(args.argc, args.argv);
    }
    pthread_attr_destroy(&attr);
    std::printf("[switch] exit code %d\n", args.result);
    stop_watch();
    close_log();
    return args.result;
}

#endif  // __SWITCH__
