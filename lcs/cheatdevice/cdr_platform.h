/*
 *  Cheat Device Remastered for LCSNative: the PSP SDK, seen from the port.
 *
 *  Cheat Device Remastered (https://github.com/Freakler/CheatDeviceRemastered)
 *  is a PSP plugin by Freakler, converted here with his permission. Its code
 *  runs natively in the port instead of as MIPS beside the game, so this
 *  header stands in for the PSP SDK it was written against:
 *
 *  - Addresses stay what they were on the PSP: the game's memory is the
 *    guest's, reached through GPTR() (a raw `*(int*)adr` became
 *    `*(int*)GPTR(adr)`) and _lw/_sw.
 *  - The game's functions are GuestFunction objects. Assigning an address
 *    (`GetPPLAYER = (void*)(addr)`) and calling them runs the recompiled
 *    game code (Runtime::call_guest); strings and buffers in the port's own
 *    memory are copied to and from a scratch area in guest memory.
 *  - Patching the game's code (HIJACK_FUNCTION, MAKE_CALL, ...) cannot
 *    change recompiled code: those requests are recorded (cdr_hooks) and the
 *    hooks the menu needs are driven from the port (cdr_host.cpp).
 *  - Files, time, threads and the pad map onto the host.
 *
 *  Copyright (C) 2017-2025, Freakler (Cheat Device Remastered)
 *  Copyright (C) 2026, LCSNative contributors (this conversion)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version. See LICENSE in this folder.
 */
#pragma once

#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <algorithm>
#include <type_traits>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// The PSP's SceIoStat names its times like POSIX's macros do.
#undef st_atime
#undef st_mtime
#undef st_ctime

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

// ---------------------------------------------------------------------------
// Types and constants of the PSP SDK.

typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
typedef std::int8_t s8;
typedef std::int16_t s16;
typedef std::int32_t s32;
typedef std::int64_t s64;
typedef int SceUID;
typedef unsigned int SceSize;
typedef int SceSSize;
typedef std::int64_t SceInt64;
typedef std::uint64_t SceUInt64;
typedef int SceInt32;
typedef unsigned int SceUInt32;
typedef std::int64_t SceOff;
typedef int SceMode;
typedef unsigned short SceUShort16;

#define PSP_MODULE_INFO(name, attributes, major, minor)
#define PSP_MODULE_USER 0
#define PSP_THREAD_ATTR_USER 0x80000000
#define PSP_MEMORY_PARTITION_USER 2
#define PSP_SMEM_Low 0
#define PSP_SMEM_High 1
#define PSP_SMEM_H 1

enum PspCtrlButtons {
    PSP_CTRL_SELECT = 0x000001,
    PSP_CTRL_START = 0x000008,
    PSP_CTRL_UP = 0x000010,
    PSP_CTRL_RIGHT = 0x000020,
    PSP_CTRL_DOWN = 0x000040,
    PSP_CTRL_LEFT = 0x000080,
    PSP_CTRL_LTRIGGER = 0x000100,
    PSP_CTRL_RTRIGGER = 0x000200,
    PSP_CTRL_TRIANGLE = 0x001000,
    PSP_CTRL_CIRCLE = 0x002000,
    PSP_CTRL_CROSS = 0x004000,
    PSP_CTRL_SQUARE = 0x008000,
    PSP_CTRL_HOME = 0x010000,
    PSP_CTRL_HOLD = 0x020000,
    PSP_CTRL_NOTE = 0x800000,
    PSP_CTRL_SCREEN = 0x400000,
    PSP_CTRL_VOLUP = 0x100000,
    PSP_CTRL_VOLDOWN = 0x200000,
};

struct SceCtrlData {
    unsigned int TimeStamp;
    unsigned int Buttons;
    unsigned char Lx;
    unsigned char Ly;
    unsigned char Rsrv[6];
};

struct ScePspFVector2 { float x, y; };
struct ScePspFVector3 { float x, y, z; };
struct ScePspIVector2 { int x, y; };

#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR (PSP_O_RDONLY | PSP_O_WRONLY)
#define PSP_O_NBLOCK 0x0004
#define PSP_O_DIROPEN 0x0008
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT 0x0200
#define PSP_O_TRUNC 0x0400
#define PSP_O_EXCL 0x0800
#define PSP_SEEK_SET 0
#define PSP_SEEK_CUR 1
#define PSP_SEEK_END 2

#define FIO_S_IFMT 0xF000
#define FIO_S_IFLNK 0x4000
#define FIO_S_IFDIR 0x1000
#define FIO_S_IFREG 0x2000
#define FIO_S_ISDIR(m) (((m) & FIO_S_IFMT) == FIO_S_IFDIR)
#define FIO_S_ISREG(m) (((m) & FIO_S_IFMT) == FIO_S_IFREG)
#define FIO_SO_IFDIR 0x0010
#define FIO_SO_IFREG 0x0020
#define FIO_SO_ISDIR(m) (((m) & 0x0038) == FIO_SO_IFDIR)
#define FIO_SO_ISREG(m) (((m) & 0x0038) == FIO_SO_IFREG)

struct ScePspDateTime {
    unsigned short year, month, day, hour, minute, second;
    unsigned int microsecond;
};

struct SceIoStat {
    SceMode st_mode;
    unsigned int st_attr;
    SceOff st_size;
    ScePspDateTime st_ctime;
    ScePspDateTime st_atime;
    ScePspDateTime st_mtime;
    unsigned int st_private[6];
};

struct SceIoDirent {
    SceIoStat d_stat;
    char d_name[256];
    void *d_private;
    int dummy;
};

struct SceKernelModuleInfo {
    SceSize size;
    char nsegment;
    char reserved[3];
    int segmentaddr[4];
    int segmentsize[4];
    unsigned int entry_addr;
    unsigned int gp_value;
    unsigned int text_addr;
    unsigned int text_size;
    unsigned int data_size;
    unsigned int bss_size;
    unsigned short attribute;
    unsigned char version[2];
    char name[28];
};

struct pspTime {
    unsigned short year, month, day, hour, minutes, seconds;
    unsigned int microseconds;
};

// ---------------------------------------------------------------------------
// The game's memory.

// The guest RAM as host memory (canonical addresses and their mirrors). An
// address outside it gives a harmless dummy so a stray read cannot crash.
void *cdr_guest_pointer(std::uint32_t address, std::size_t bytes) noexcept;
#define GPTR(address) cdr_guest_pointer(static_cast<std::uint32_t>(address), 4u)

std::uint32_t cdr_load32(std::uint32_t address) noexcept;
std::uint16_t cdr_load16(std::uint32_t address) noexcept;
std::uint8_t cdr_load8(std::uint32_t address) noexcept;
void cdr_store32(std::uint32_t address, std::uint32_t value) noexcept;
void cdr_store16(std::uint32_t address, std::uint16_t value) noexcept;
void cdr_store8(std::uint32_t address, std::uint8_t value) noexcept;

#define _lw(address) cdr_load32(static_cast<std::uint32_t>(address))
#define _lh(address) cdr_load16(static_cast<std::uint32_t>(address))
#define _lb(address) cdr_load8(static_cast<std::uint32_t>(address))
#define _sw(value, address) cdr_store32(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(value))
#define _sh(value, address) cdr_store16(static_cast<std::uint32_t>(address), static_cast<std::uint16_t>(value))
#define _sb(value, address) cdr_store8(static_cast<std::uint32_t>(address), static_cast<std::uint8_t>(value))

// Guest address of the port's own C string / buffer copy (see GuestFunction).
std::uint32_t cdr_scratch_copy_in(const void *data, std::size_t bytes) noexcept;
void cdr_scratch_copy_out(void *data, std::uint32_t address, std::size_t bytes) noexcept;
void cdr_scratch_reset() noexcept;

// A guest copy of a buffer in the port's own memory, at an address that stays
// the same for that buffer: the game may read it frames later (a script it
// runs, a vehicle name it prints). Where the PSP code wrote `(int)&buffer`.
int cdr_guest_mirror(const void *host, std::size_t bytes) noexcept;
// Copies the guest copy back into the buffer (the game wrote into it).
void cdr_guest_mirror_back(void *host, std::size_t bytes) noexcept;

// A value that is a guest address passed around as a pointer.
inline bool cdr_is_guest_address(std::uintptr_t value) noexcept {
    const std::uintptr_t canonical = value & 0x1FFFFFFFu;
    return value < 0x100000000ull && canonical >= 0x08000000u && canonical < 0x0C000000u;
}

// ---------------------------------------------------------------------------
// Code patches: recorded, not applied (see the top of this file).

void cdr_note_code_patch(const char *kind, std::uint32_t address, const char *target) noexcept;

#define MAKE_CALL(a, f) cdr_note_code_patch("call", static_cast<std::uint32_t>(a), #f);
#define MAKE_JUMP(a, f) cdr_note_code_patch("jump", static_cast<std::uint32_t>(a), #f);
#define HIJACK_FUNCTION(a, f, ptr) { \
    cdr_note_code_patch("hijack", static_cast<std::uint32_t>(a), #f); \
    ptr = (void *)(std::uintptr_t)(static_cast<std::uint32_t>(a)); \
}
#define REDIRECT_FUNCTION(a, f) cdr_note_code_patch("redirect", static_cast<std::uint32_t>(a), #f);
#define MAKE_DUMMY_FUNCTION(a, r) cdr_note_code_patch("dummy", static_cast<std::uint32_t>(a), #r);

inline void clearICacheFor(u32) {}
inline void sceKernelDcacheWritebackAll() {}
inline void sceKernelDcacheWritebackInvalidateAll() {}
inline void sceKernelIcacheInvalidateAll() {}

// ---------------------------------------------------------------------------
// Calling the game.

std::uint32_t cdr_call_guest(std::uint32_t address, const std::uint32_t *gprs, std::size_t gpr_count,
                             const float *fprs, std::size_t fpr_count, float *float_result) noexcept;

namespace cdr_detail {

struct CallBuilder {
    std::uint32_t gprs[8]{};
    std::size_t gpr_count{};
    float fprs[8]{};
    std::size_t fpr_count{};
    struct Writeback {
        void *host;
        std::uint32_t guest;
        std::size_t bytes;
    } writebacks[8]{};
    std::size_t writeback_count{};

    void gpr(std::uint32_t value) noexcept {
        if (gpr_count < 8u) gprs[gpr_count++] = value;
    }
    void fpr(float value) noexcept {
        if (fpr_count < 8u) fprs[fpr_count++] = value;
    }

    template <class T>
    void add(T value) noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            fpr(static_cast<float>(value));
        } else if constexpr (std::is_pointer_v<T>) {
            const auto raw = reinterpret_cast<std::uintptr_t>(value);
            if (value == nullptr || cdr_is_guest_address(raw)) {
                gpr(static_cast<std::uint32_t>(raw));
                return;
            }
            using Pointee = std::remove_cv_t<std::remove_pointer_t<T>>;
            if constexpr (std::is_same_v<Pointee, char>) {
                // A string, or a buffer the game fills (copied back).
                const std::size_t length = std::strlen(reinterpret_cast<const char *>(value)) + 1u;
                const std::size_t bytes = std::max<std::size_t>(length, 256u);
                const std::uint32_t guest = cdr_scratch_copy_in(value, length);
                gpr(guest);
                if constexpr (!std::is_const_v<std::remove_pointer_t<T>>)
                    if (writeback_count < 8u) writebacks[writeback_count++] = {(void *)value, guest, bytes};
            } else {
                // A small array (colors, coordinates): 16 elements each way.
                constexpr std::size_t bytes = 16u * sizeof(std::conditional_t<std::is_void_v<Pointee>, char, Pointee>);
                const std::uint32_t guest = cdr_scratch_copy_in(value, bytes);
                gpr(guest);
                if constexpr (!std::is_const_v<std::remove_pointer_t<T>>)
                    if (writeback_count < 8u) writebacks[writeback_count++] = {(void *)value, guest, bytes};
            }
        } else {
            gpr(static_cast<std::uint32_t>(value));
        }
    }

    void finish() noexcept {
        for (std::size_t index = 0; index < writeback_count; ++index)
            cdr_scratch_copy_out(writebacks[index].host, writebacks[index].guest, writebacks[index].bytes);
    }
};

}  // namespace cdr_detail

template <class Signature>
class GuestFunction;

// A game function at a guest address; calling it runs the recompiled code.
template <class Return, class... Args>
class GuestFunction<Return(Args...)> {
public:
    GuestFunction() = default;
    GuestFunction &operator=(void *address) noexcept {
        address_ = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address));
        return *this;
    }
    GuestFunction &operator=(std::nullptr_t) noexcept {
        address_ = 0u;
        return *this;
    }
    explicit operator bool() const noexcept { return address_ != 0u; }
    bool operator==(std::nullptr_t) const noexcept { return address_ == 0u; }
    bool operator!=(std::nullptr_t) const noexcept { return address_ != 0u; }
    [[nodiscard]] std::uint32_t address() const noexcept { return address_; }
    operator void *() const noexcept { return (void *)(std::uintptr_t)address_; }

    Return operator()(Args... args) const noexcept {
        cdr_detail::CallBuilder call;
        (call.add(args), ...);
        float float_result = 0.0f;
        const std::uint32_t result =
            address_ == 0u ? 0u
                           : cdr_call_guest(address_, call.gprs, call.gpr_count, call.fprs, call.fpr_count,
                                            &float_result);
        call.finish();
        if constexpr (std::is_void_v<Return>) {
            (void)result;
        } else if constexpr (std::is_floating_point_v<Return>) {
            return static_cast<Return>(float_result);
        } else if constexpr (std::is_pointer_v<Return>) {
            return reinterpret_cast<Return>(static_cast<std::uintptr_t>(result));
        } else {
            return static_cast<Return>(result);
        }
    }

private:
    std::uint32_t address_{};
};

// printf-like game logging (only a hook used it): never called.
struct GuestPrintf {
    GuestPrintf &operator=(void *) noexcept { return *this; }
    void operator()(const char *, ...) const noexcept {}
};

// ---------------------------------------------------------------------------
// Files (the PSP's ms0:/ and the plugin folder map onto the host).

const char *cdr_host_path(const char *psp_path) noexcept;

SceUID sceIoOpen(const char *path, int flags, SceMode mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void *data, SceSize size);
int sceIoWrite(SceUID fd, const void *data, SceSize size);
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence);
int sceIoLseek32(SceUID fd, int offset, int whence);
int sceIoRemove(const char *path);
int sceIoMkdir(const char *path, SceMode mode);
int sceIoRmdir(const char *path);
int sceIoRename(const char *from, const char *to);
int sceIoMvdir(const char *from, const char *to);
int sceIoGetstat(const char *path, SceIoStat *stat);
int sceIoChstat(const char *path, SceIoStat *stat, int bits);
SceUID sceIoDopen(const char *path);
int sceIoDread(SceUID fd, SceIoDirent *entry);
int sceIoDclose(SceUID fd);
int sceIoDevctl(const char *device, unsigned int command, void *input, int input_length, void *output,
                int output_length);

// ---------------------------------------------------------------------------
// Time, threads, memory, pad, display.

SceInt64 sceKernelGetSystemTimeWide();
unsigned int sceKernelGetSystemTimeLow();
int sceKernelDelayThread(unsigned int microseconds);
int sceKernelSleepThread();
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority, int stack_size,
                             unsigned int attributes, void *options);
int sceKernelStartThread(SceUID thread, SceSize args, void *argp);
int sceKernelExitDeleteThread(int status);
SceSize sceKernelTotalFreeMemSize();
SceSize sceKernelMaxFreeMemSize();
SceUID sceKernelAllocPartitionMemory(SceUID partition, const char *name, int type, SceSize size, void *address);
int sceKernelFreePartitionMemory(SceUID block);
void *sceKernelGetBlockHeadAddr(SceUID block);
int sceKernelPrintf(const char *format, ...);
void sceKernelExitGame();
int sceRtcGetCurrentClockLocalTime(pspTime *time);
int sceCtrlPeekBufferPositive(SceCtrlData *pad, int count);
int sceDisplaySetMode(int mode, int width, int height);
int sceDisplaySetFrameBuf(void *top, int buffer_width, int pixel_format, int sync);
int sceDisplaySetFrameBufferInternal(int priority, void *top, int buffer_width, int pixel_format, int sync);
int sceKernelGetModuleIdList(SceUID *list, SceSize size, int *count);
int sceKernelQueryModuleInfo(SceUID module, SceKernelModuleInfo *info);

// ---------------------------------------------------------------------------
// C library bits the PSP toolchain had.

inline void *memalign(std::size_t alignment, std::size_t bytes) {
    void *block = nullptr;
    if (posix_memalign(&block, alignment < sizeof(void *) ? sizeof(void *) : alignment, bytes) != 0) return nullptr;
    return block;
}
