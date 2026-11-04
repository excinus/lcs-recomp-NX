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
//  4. It asks sys-clk (if installed) for a faster CPU and memory while the game
//     is in focus, and gives the clocks back when it is not or when it exits.
//  5. It runs the PC main() on a thread with a 64 MiB stack pinned to core 0.
//
// --wrap=pthread_create puts every other thread on cores 1 and 2.

#if defined(__SWITCH__)

#include <switch.h>

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
#include <vector>

extern "C" {
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
    g_log_flusher = std::thread([] {
        while (!g_log_flusher_stop.load(std::memory_order_relaxed)) {
            svcSleepThread(500'000'000ll);
            std::fflush(stdout);
            std::fflush(stderr);
        }
    });
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
    // Thread layout: the game on core 0, the GE worker on core 1, one raster
    // worker on core 2. hardware_concurrency() is 0 on Horizon (no sysconf),
    // so the pools are sized here.
    setenv("PSPRECOMP_RASTER_THREADS", "2", 0);
    setenv("PSPRECOMP_GE_DECODE_THREADS", "0", 0);
    // Mesa: skip per-call GL error checks.
    setenv("MESA_NO_ERROR", "1", 0);
}

bool env_enabled(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "off") != 0;
}

// ---------------------------------------------------------------------------
// Clocks. sys-clk (the usual overclocking sysmodule, part of most CFW packs)
// accepts temporary overrides over its "sys:clk" service: command 8, input
// {u32 module, u32 hz}; module 0 CPU, 1 GPU, 2 memory; 0 Hz hands control back
// to sys-clk's own profile. Nothing is written to its configuration, and the
// overrides are released whenever the game loses focus and at exit.

constexpr u32 kSysClkCmdSetOverride = 8;
constexpr u32 kSysClkModuleCpu = 0;
constexpr u32 kSysClkModuleMem = 2;

std::mutex g_clock_mutex;
Service g_sysclk{};
bool g_sysclk_open{};
bool g_clocks_applied{};
bool g_clocks_wanted{};
u32 g_cpu_hz{1785000000u};
u32 g_mem_hz{1600000000u};
AppletHookCookie g_focus_cookie{};

void sysclk_set(u32 module, u32 hz) {
    const struct {
        u32 module;
        u32 hz;
    } in{module, hz};
    serviceDispatchIn(&g_sysclk, kSysClkCmdSetOverride, in);
}

void apply_clocks(bool enable) {
    std::lock_guard<std::mutex> guard(g_clock_mutex);
    if (!g_sysclk_open || enable == g_clocks_applied) return;
    sysclk_set(kSysClkModuleCpu, enable ? g_cpu_hz : 0u);
    sysclk_set(kSysClkModuleMem, enable ? g_mem_hz : 0u);
    g_clocks_applied = enable;
}

void on_applet_hook(AppletHookType hook, void *) {
    if (hook != AppletHookType_OnFocusState) return;
    const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
    apply_clocks(focused && g_clocks_wanted);
}

void release_clocks_at_exit() {
    apply_clocks(false);
    std::lock_guard<std::mutex> guard(g_clock_mutex);
    if (g_sysclk_open) {
        serviceClose(&g_sysclk);
        g_sysclk_open = false;
    }
}

void start_clocks() {
    g_clocks_wanted = env_enabled("LCS_NX_OVERCLOCK", true);
    if (const char *text = std::getenv("LCS_NX_CPU_MHZ")) {
        const long mhz = std::strtol(text, nullptr, 10);
        if (mhz >= 612 && mhz <= 2397) g_cpu_hz = static_cast<u32>(mhz) * 1000000u;
    }
    if (const char *text = std::getenv("LCS_NX_MEM_MHZ")) {
        const long mhz = std::strtol(text, nullptr, 10);
        if (mhz >= 665 && mhz <= 3200) g_mem_hz = static_cast<u32>(mhz) * 1000000u;
    }
    appletHook(&g_focus_cookie, on_applet_hook, nullptr);
    if (g_clocks_wanted) {
        if (R_SUCCEEDED(smGetService(&g_sysclk, "sys:clk"))) {
            g_sysclk_open = true;
            apply_clocks(true);
            std::printf("[switch] sys-clk: CPU %u MHz, memory %u MHz while in focus\n",
                        g_cpu_hz / 1000000u, g_mem_hz / 1000000u);
        } else {
            std::printf("[switch] sys-clk not running; stock clocks\n");
        }
    } else {
        std::printf("[switch] overclock off (LCS_NX_OVERCLOCK=0)\n");
    }
    // The console's own boost mode: CPU 1785 MHz but the GPU drops to 76.8 MHz,
    // so it only suits the software renderer. Opt-in.
    if (env_enabled("LCS_NX_CPU_BOOST", false)) {
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
        std::printf("[switch] CPU boost mode on (GPU at 76.8 MHz)\n");
    }
    std::atexit(release_clocks_at_exit);
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
// Threads.

struct GameThreadArgs {
    int argc;
    char **argv;
    int result;
};

void *game_thread_main(void *param) {
    auto *args = static_cast<GameThreadArgs *>(param);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, kGameCoreMask);
    args->result = __real_main(args->argc, args->argv);
    return nullptr;
}

struct ThreadStart {
    void *(*routine)(void *);
    void *arg;
    s32 core;
};

std::atomic<unsigned> g_threads_started{0};

void *worker_thread_main(void *param) {
    ThreadStart start = *static_cast<ThreadStart *>(param);
    delete static_cast<ThreadStart *>(param);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, start.core, kWorkerCoreMask);
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
    auto *start = new ThreadStart{start_routine, arg,
                                  1 + static_cast<s32>(g_threads_started.fetch_add(1) % 2u)};
    const int result = __real_pthread_create(thread, &sized, worker_thread_main, start);
    if (result != 0) delete start;
    return result;
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
    std::printf("[switch] LCSNative for Nintendo Switch\n");
    load_env_file();
    set_default_env();
    log_system();

    const std::string elf_path = in_base("game/EBOOT.ELF");
    if (!file_exists(elf_path)) {
        std::printf("[switch] missing %s\n", elf_path.c_str());
        close_log();
        show_message_and_wait(
            "GTA: Liberty City Stories (LCS Recomp)\n\n"
            "The game files are missing. Put them here on the SD card:\n\n"
            "  " + g_base_dir + "/game/EBOOT.ELF   (decrypted, US v1.05, ULUS-10041)\n"
            "  " + g_base_dir + "/game/PSP_GAME/   (the disc's PSP_GAME folder)\n\n"
            "The PC launcher (LCSLauncher.exe) extracts both from your ISO.");
        return 1;
    }

    start_clocks();

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
    release_clocks_at_exit();
    close_log();
    return args.result;
}

#endif  // __SWITCH__
