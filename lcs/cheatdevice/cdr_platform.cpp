/*
 *  Cheat Device Remastered for LCSNative: the PSP SDK on the host.
 *  See cdr_platform.h.
 *
 *  Copyright (C) 2017-2025, Freakler (Cheat Device Remastered)
 *  Copyright (C) 2026, LCSNative contributors (this conversion)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version. See LICENSE in this folder.
 */
// Before cdr_platform.h, which takes the st_*time macros away.
#include <sys/stat.h>
#include <ctime>
static std::time_t cdr_host_mtime(const struct stat &host) { return host.st_mtime; }

#include "cdr_platform.h"
#include "cdr_host_internal.h"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

psprecomp::Runtime *g_runtime = nullptr;
psprecomp::AllegrexContext *g_context = nullptr;
std::string g_plugin_folder;  // "<base>/cheatdevice_remastered/"
std::string g_card_root;      // what ms0:/ maps to

// Guest memory the port keeps for the cheat device (below the game, which
// loads at 0x08804000; nothing else uses 0x08100000-0x08200000): a scratch
// area for strings and buffers handed to game functions, and a stack for
// those calls.
constexpr std::uint32_t kScratchBase = 0x08100000u;
constexpr std::uint32_t kScratchEnd = 0x08180000u;
constexpr std::uint32_t kMirrorBase = 0x08180000u;
constexpr std::uint32_t kMirrorEnd = 0x081C0000u;
constexpr std::uint32_t kCallStackTop = 0x08200000u - 0x40u;  // down to 0x081C0000
std::uint32_t g_scratch_next = kScratchBase;

alignas(16) std::uint8_t g_dummy[64];

SceCtrlData g_pad{0u, 0u, 128u, 128u, {128u, 128u, 0u, 0u, 0u, 0u}};

// The guest RAM in one piece (the scan reads the game's code a word at a
// time, millions of times at start-up).
std::uint8_t *g_ram = nullptr;
std::uint32_t g_ram_size = 0u;

}  // namespace

// ---------------------------------------------------------------------------
// Binding to the port.

void cdr_bind_runtime(psprecomp::Runtime *runtime) noexcept {
    g_runtime = runtime;
    g_ram = nullptr;
    g_ram_size = 0u;
    if (runtime == nullptr) return;
    // As much of the RAM from 0x08000000 as is one contiguous block.
    for (std::uint32_t size = 64u << 20; size >= (8u << 20); size /= 2u) {
        if (std::uint8_t *base = runtime->memory().raw_pointer(0x08000000u, size)) {
            g_ram = base;
            g_ram_size = size;
            break;
        }
    }
}
void cdr_set_call_context(psprecomp::AllegrexContext *context) noexcept { g_context = context; }
void cdr_set_folders(const std::string &plugin_folder, const std::string &card_root) {
    g_plugin_folder = plugin_folder;
    g_card_root = card_root;
}
const std::string &cdr_plugin_folder() noexcept { return g_plugin_folder; }

void cdr_set_pad(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly, std::uint8_t rx, std::uint8_t ry) noexcept {
    g_pad.Buttons = buttons;
    g_pad.Lx = lx;
    g_pad.Ly = ly;
    g_pad.Rsrv[0] = rx;
    g_pad.Rsrv[1] = ry;
    g_pad.TimeStamp = static_cast<unsigned int>(sceKernelGetSystemTimeLow());
}

// ---------------------------------------------------------------------------
// Memory.

void *cdr_guest_pointer(std::uint32_t address, std::size_t bytes) noexcept {
    const std::uint32_t offset = (address & 0x1FFFFFFFu) - 0x08000000u;
    if (g_ram != nullptr && offset < g_ram_size && bytes <= g_ram_size - offset) return g_ram + offset;
    if (g_runtime != nullptr) {
        if (std::uint8_t *pointer = g_runtime->memory().raw_pointer(address, bytes)) return pointer;
    }
    std::memset(g_dummy, 0, sizeof(g_dummy));
    return g_dummy;
}

std::uint32_t cdr_load32(std::uint32_t address) noexcept {
    std::uint32_t value = 0u;
    std::memcpy(&value, cdr_guest_pointer(address, 4u), 4u);
    return value;
}
std::uint16_t cdr_load16(std::uint32_t address) noexcept {
    std::uint16_t value = 0u;
    std::memcpy(&value, cdr_guest_pointer(address, 2u), 2u);
    return value;
}
std::uint8_t cdr_load8(std::uint32_t address) noexcept {
    return *static_cast<std::uint8_t *>(cdr_guest_pointer(address, 1u));
}
void cdr_store32(std::uint32_t address, std::uint32_t value) noexcept {
    std::memcpy(cdr_guest_pointer(address, 4u), &value, 4u);
}
void cdr_store16(std::uint32_t address, std::uint16_t value) noexcept {
    std::memcpy(cdr_guest_pointer(address, 2u), &value, 2u);
}
void cdr_store8(std::uint32_t address, std::uint8_t value) noexcept {
    *static_cast<std::uint8_t *>(cdr_guest_pointer(address, 1u)) = value;
}

std::uint32_t cdr_scratch_copy_in(const void *data, std::size_t bytes) noexcept {
    const std::size_t limit = kScratchEnd - kScratchBase;
    if (bytes > limit) bytes = limit;
    std::size_t reserved = (bytes + 15u) & ~std::size_t{15u};
    if (reserved == 0u) reserved = 16u;
    if (g_scratch_next + reserved > kScratchEnd) g_scratch_next = kScratchBase;
    const std::uint32_t address = g_scratch_next;
    g_scratch_next += static_cast<std::uint32_t>(reserved);
    void *target = cdr_guest_pointer(address, reserved);
    std::memset(target, 0, reserved);
    if (data != nullptr && bytes != 0u) std::memcpy(target, data, bytes);
    return address;
}

void cdr_scratch_copy_out(void *data, std::uint32_t address, std::size_t bytes) noexcept {
    if (data == nullptr) return;
    std::memcpy(data, cdr_guest_pointer(address, bytes), bytes);
}

void cdr_scratch_reset() noexcept { g_scratch_next = kScratchBase; }

namespace {
struct Mirror {
    std::uint32_t guest;
    std::size_t bytes;
};
std::map<const void *, Mirror> g_mirrors;
std::uint32_t g_mirror_next = kMirrorBase;
}  // namespace

int cdr_guest_mirror(const void *host, std::size_t bytes) noexcept {
    if (bytes == 0u) bytes = 4u;
    auto found = g_mirrors.find(host);
    if (found == g_mirrors.end() || found->second.bytes < bytes) {
        const std::uint32_t reserved = static_cast<std::uint32_t>((bytes + 15u) & ~std::size_t{15u});
        if (g_mirror_next + reserved > kMirrorEnd) {
            // Full: start over (a mirror the game still reads would be lost,
            // but the cheat device keeps few, small buffers).
            g_mirrors.clear();
            g_mirror_next = kMirrorBase;
        }
        found = g_mirrors.insert_or_assign(host, Mirror{g_mirror_next, bytes}).first;
        g_mirror_next += reserved;
    }
    std::memcpy(cdr_guest_pointer(found->second.guest, bytes), host, bytes);
    return static_cast<int>(found->second.guest);
}

void cdr_guest_mirror_back(void *host, std::size_t bytes) noexcept {
    const auto found = g_mirrors.find(host);
    if (found == g_mirrors.end()) return;
    std::memcpy(host, cdr_guest_pointer(found->second.guest, bytes), std::min(bytes, found->second.bytes));
}

// ---------------------------------------------------------------------------
// Code patches and calls.

namespace {
std::mutex g_patch_mutex;
std::vector<CdrCodePatch> g_patches;
}  // namespace

void cdr_note_code_patch(const char *kind, std::uint32_t address, const char *target) noexcept {
    std::lock_guard lock(g_patch_mutex);
    g_patches.push_back(CdrCodePatch{kind, address, target});
}

std::vector<CdrCodePatch> cdr_code_patches() {
    std::lock_guard lock(g_patch_mutex);
    return g_patches;
}

std::uint32_t cdr_call_guest(std::uint32_t address, const std::uint32_t *gprs, std::size_t gpr_count,
                             const float *fprs, std::size_t fpr_count, float *float_result) noexcept {
    static int warned = 0;
    if (g_runtime == nullptr || g_context == nullptr) {
        if (warned++ < 8)
            std::cerr << "[cheatdevice] game function 0x" << std::hex << address << std::dec
                      << " called outside the game thread's frame hook\n";
        return 0u;
    }
    // A copy of the game thread's registers, on the cheat device's own stack,
    // with the PSP's argument registers ($a0-$t3, $f12-$f19) filled in.
    psprecomp::AllegrexContext context = *g_context;
    context.gpr[29] = kCallStackTop;
    for (std::size_t index = 0; index < gpr_count && index < 8u; ++index) context.gpr[4u + index] = gprs[index];
    for (std::size_t index = 0; index < fpr_count && index < 8u; ++index) context.fpr[12u + index] = fprs[index];
    if (!g_runtime->call_guest(address, context)) {
        if (warned++ < 8)
            std::cerr << "[cheatdevice] game function 0x" << std::hex << address << std::dec
                      << " did not return\n";
        return 0u;
    }
    if (float_result != nullptr) *float_result = context.fpr[0];
    return context.gpr[2];
}

// ---------------------------------------------------------------------------
// Files.

const char *cdr_host_path(const char *psp_path) noexcept {
    static thread_local std::string path;
    path = psp_path != nullptr ? psp_path : "";
    static const std::string plugin = "ms0:/PSP/PLUGINS/cheatdevice_remastered/";
    const auto starts = [&](const std::string &prefix) {
        return path.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), path.begin(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    };
    if (starts(plugin)) {
        path = g_plugin_folder + path.substr(plugin.size());
    } else if (starts("ms0:/")) {
        path = g_card_root + path.substr(5);
    } else if (starts("ef0:/")) {
        path = g_card_root + path.substr(5);
    }
    return path.c_str();
}

namespace {
struct DirectoryHandle {
    DIR *dir{};
    std::string path;
};
std::mutex g_dir_mutex;
std::map<int, DirectoryHandle> g_dirs;
int g_next_dir = 1;

void fill_stat(const struct stat &host, SceIoStat *stat) {
    std::memset(stat, 0, sizeof(*stat));
    stat->st_mode = S_ISDIR(host.st_mode) ? (FIO_S_IFDIR | 0777) : (FIO_S_IFREG | 0666);
    stat->st_attr = S_ISDIR(host.st_mode) ? FIO_SO_IFDIR : FIO_SO_IFREG;
    stat->st_size = host.st_size;
    std::tm tm{};
    const std::time_t when = cdr_host_mtime(host);
#if defined(_WIN32)
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    ScePspDateTime time{};
    time.year = static_cast<unsigned short>(tm.tm_year + 1900);
    time.month = static_cast<unsigned short>(tm.tm_mon + 1);
    time.day = static_cast<unsigned short>(tm.tm_mday);
    time.hour = static_cast<unsigned short>(tm.tm_hour);
    time.minute = static_cast<unsigned short>(tm.tm_min);
    time.second = static_cast<unsigned short>(tm.tm_sec);
    stat->st_ctime = stat->st_atime = stat->st_mtime = time;
}
}  // namespace

SceUID sceIoOpen(const char *path, int flags, SceMode mode) {
    int host_flags = 0;
    if ((flags & PSP_O_RDWR) == PSP_O_RDWR) host_flags = O_RDWR;
    else if (flags & PSP_O_WRONLY) host_flags = O_WRONLY;
    else host_flags = O_RDONLY;
    if (flags & PSP_O_CREAT) host_flags |= O_CREAT;
    if (flags & PSP_O_TRUNC) host_flags |= O_TRUNC;
    if (flags & PSP_O_APPEND) host_flags |= O_APPEND;
    if (flags & PSP_O_EXCL) host_flags |= O_EXCL;
#ifdef O_BINARY
    host_flags |= O_BINARY;
#endif
    const int fd = ::open(cdr_host_path(path), host_flags, mode != 0 ? mode : 0666);
    return fd < 0 ? static_cast<SceUID>(0x80010002) : fd;
}
int sceIoClose(SceUID fd) { return fd >= 0 ? ::close(fd) : -1; }
int sceIoRead(SceUID fd, void *data, SceSize size) {
    return fd >= 0 ? static_cast<int>(::read(fd, data, size)) : -1;
}
int sceIoWrite(SceUID fd, const void *data, SceSize size) {
    return fd >= 0 ? static_cast<int>(::write(fd, data, size)) : -1;
}
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence) {
    return fd >= 0 ? static_cast<SceOff>(::lseek(fd, static_cast<off_t>(offset), whence)) : -1;
}
int sceIoLseek32(SceUID fd, int offset, int whence) { return static_cast<int>(sceIoLseek(fd, offset, whence)); }
int sceIoRemove(const char *path) { return ::remove(cdr_host_path(path)); }
int sceIoMkdir(const char *path, SceMode mode) {
#if defined(_WIN32)
    (void)mode;
    return ::mkdir(cdr_host_path(path));
#else
    return ::mkdir(cdr_host_path(path), mode != 0 ? mode : 0777);
#endif
}
int sceIoRmdir(const char *path) { return ::rmdir(cdr_host_path(path)); }
int sceIoRename(const char *from, const char *to) {
    const std::string source = cdr_host_path(from);
    return ::rename(source.c_str(), cdr_host_path(to));
}
int sceIoMvdir(const char *from, const char *to) { return sceIoRename(from, to); }
int sceIoGetstat(const char *path, SceIoStat *stat) {
    struct stat host {};
    if (::stat(cdr_host_path(path), &host) != 0) return static_cast<int>(0x80010002);
    fill_stat(host, stat);
    return 0;
}
int sceIoChstat(const char *, SceIoStat *, int) { return 0; }
SceUID sceIoDopen(const char *path) {
    const std::string host = cdr_host_path(path);
    DIR *dir = ::opendir(host.c_str());
    if (dir == nullptr) return static_cast<SceUID>(0x80010002);
    std::lock_guard lock(g_dir_mutex);
    const int handle = g_next_dir++;
    g_dirs[handle] = DirectoryHandle{dir, host};
    return handle;
}
int sceIoDread(SceUID fd, SceIoDirent *entry) {
    std::lock_guard lock(g_dir_mutex);
    const auto found = g_dirs.find(fd);
    if (found == g_dirs.end()) return -1;
    for (;;) {
        const dirent *next = ::readdir(found->second.dir);
        if (next == nullptr) return 0;
        std::memset(entry, 0, sizeof(*entry));
        std::snprintf(entry->d_name, sizeof(entry->d_name), "%s", next->d_name);
        struct stat host {};
        const std::string full = found->second.path + "/" + next->d_name;
        if (::stat(full.c_str(), &host) == 0) fill_stat(host, &entry->d_stat);
        return 1;
    }
}
int sceIoDclose(SceUID fd) {
    std::lock_guard lock(g_dir_mutex);
    const auto found = g_dirs.find(fd);
    if (found == g_dirs.end()) return -1;
    ::closedir(found->second.dir);
    g_dirs.erase(found);
    return 0;
}
// Only used to ask whether the host is an emulator: it is not a PSP.
int sceIoDevctl(const char *, unsigned int, void *, int, void *, int) { return -1; }

// ---------------------------------------------------------------------------
// Time, threads, memory, pad, display.

SceInt64 sceKernelGetSystemTimeWide() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}
unsigned int sceKernelGetSystemTimeLow() { return static_cast<unsigned int>(sceKernelGetSystemTimeWide()); }
int sceKernelDelayThread(unsigned int microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return 0;
}
int sceKernelSleepThread() { return 0; }

namespace {
std::mutex g_thread_mutex;
std::map<SceUID, SceKernelThreadEntry> g_thread_entries;
SceUID g_next_thread = 1;
}  // namespace

SceUID sceKernelCreateThread(const char *, SceKernelThreadEntry entry, int, int, unsigned int, void *) {
    std::lock_guard lock(g_thread_mutex);
    const SceUID id = g_next_thread++;
    g_thread_entries[id] = entry;
    return id;
}
int sceKernelStartThread(SceUID thread, SceSize args, void *argp) {
    SceKernelThreadEntry entry = nullptr;
    {
        std::lock_guard lock(g_thread_mutex);
        const auto found = g_thread_entries.find(thread);
        if (found == g_thread_entries.end()) return -1;
        entry = found->second;
    }
    std::thread([entry, args, argp] { entry(args, argp); }).detach();
    return 0;
}
int sceKernelExitDeleteThread(int) { return 0; }
SceSize sceKernelTotalFreeMemSize() { return 16u * 1024u * 1024u; }
SceSize sceKernelMaxFreeMemSize() { return 16u * 1024u * 1024u; }

namespace {
std::map<SceUID, void *> g_blocks;
SceUID g_next_block = 1;
}  // namespace
SceUID sceKernelAllocPartitionMemory(SceUID, const char *, int, SceSize size, void *) {
    void *block = std::calloc(1u, size);
    if (block == nullptr) return -1;
    std::lock_guard lock(g_thread_mutex);
    g_blocks[g_next_block] = block;
    return g_next_block++;
}
int sceKernelFreePartitionMemory(SceUID block) {
    std::lock_guard lock(g_thread_mutex);
    const auto found = g_blocks.find(block);
    if (found == g_blocks.end()) return -1;
    std::free(found->second);
    g_blocks.erase(found);
    return 0;
}
void *sceKernelGetBlockHeadAddr(SceUID block) {
    std::lock_guard lock(g_thread_mutex);
    const auto found = g_blocks.find(block);
    return found == g_blocks.end() ? nullptr : found->second;
}
int sceKernelPrintf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    std::fputs("[cheatdevice] ", stdout);
    const int written = std::vprintf(format, args);
    std::fputs("\n", stdout);
    va_end(args);
    return written;
}
void sceKernelExitGame() { cdr_request_exit(); }
int sceRtcGetCurrentClockLocalTime(pspTime *time) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    time->year = static_cast<unsigned short>(tm.tm_year + 1900);
    time->month = static_cast<unsigned short>(tm.tm_mon + 1);
    time->day = static_cast<unsigned short>(tm.tm_mday);
    time->hour = static_cast<unsigned short>(tm.tm_hour);
    time->minutes = static_cast<unsigned short>(tm.tm_min);
    time->seconds = static_cast<unsigned short>(tm.tm_sec);
    time->microseconds = 0u;
    return 0;
}
int sceCtrlPeekBufferPositive(SceCtrlData *pad, int count) {
    for (int index = 0; index < count; ++index) pad[index] = g_pad;
    return count;
}
int sceDisplaySetMode(int, int, int) { return 0; }
int sceDisplaySetFrameBuf(void *, int, int, int) { return 0; }
int sceDisplaySetFrameBufferInternal(int, void *, int, int, int) { return 0; }
int sceKernelGetModuleIdList(SceUID *, SceSize, int *count) {
    if (count != nullptr) *count = 0;
    return 0;
}
int sceKernelQueryModuleInfo(SceUID, SceKernelModuleInfo *) { return -1; }
