// A size-class allocator in front of newlib's malloc, for the Switch build.
//
// Why: the first console profile put 52% of the GE thread in newlib's
// _malloc_r / _free_r. newlib keeps large free chunks in size-sorted bins and
// walks them on every insert and best-fit search; with the GE's steady churn
// of vertex and index buffers the bins grow long and each call walks them.
//
// What: blocks up to 256 KiB come from 1 MiB slabs, one size class per slab
// (32-byte steps to 128 bytes, then four steps per power of two). Every class
// is a multiple of 32 bytes, so every block is 32-byte aligned: devkitA64's
// malloc asks for memalign(32, n) from 32 bytes up. A freed block
// goes on its class's free list; nothing is returned to newlib. Larger blocks,
// and aligned ones that would not fit a class, stay with newlib.
//
// How it is hooked: switch.cmake links with --wrap for newlib's reentrant
// entry points (_malloc_r, _free_r, _realloc_r, _calloc_r, _memalign_r,
// _malloc_usable_size_r). newlib builds each in its own object file, so the
// plain malloc/free and newlib's own internal calls go through here too. A
// pointer is ours when its 1 MiB region is one of our slabs (a table indexed by
// the region's offset in the heap); anything else is handed back to newlib.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(LCS_SWITCH_MALLOC_TEST)
// Desktop test build: tests/switch_malloc_test.cpp supplies the heap bounds,
// the "real" allocator and a std::mutex based lock.
#include <mutex>
struct _reent;
namespace lcs_malloc_test {
extern char *heap_start;
extern char *heap_end;
void *real_malloc(std::size_t size);
void real_free(void *pointer);
void *real_memalign(std::size_t alignment, std::size_t size);
std::size_t real_usable_size(void *pointer);
}  // namespace lcs_malloc_test
#define LCS_HEAP_START lcs_malloc_test::heap_start
#define LCS_HEAP_END lcs_malloc_test::heap_end
#define LCS_REAL_MALLOC(r, n) lcs_malloc_test::real_malloc(n)
#define LCS_REAL_FREE(r, p) lcs_malloc_test::real_free(p)
#define LCS_REAL_MEMALIGN(r, a, n) lcs_malloc_test::real_memalign(a, n)
#define LCS_REAL_USABLE(r, p) lcs_malloc_test::real_usable_size(p)
#define LCS_WRAP(name) test_##name
namespace {
struct ClassLock {
    std::mutex mutex;
    void lock() { mutex.lock(); }
    void unlock() { mutex.unlock(); }
};
}  // namespace
#else
#include <switch.h>
#include <sys/reent.h>
extern "C" {
extern char *fake_heap_start;
extern char *fake_heap_end;
void *__real__malloc_r(struct _reent *r, std::size_t size);
void __real__free_r(struct _reent *r, void *pointer);
void *__real__memalign_r(struct _reent *r, std::size_t alignment, std::size_t size);
std::size_t __real__malloc_usable_size_r(struct _reent *r, void *pointer);
}
#define LCS_HEAP_START fake_heap_start
#define LCS_HEAP_END fake_heap_end
#define LCS_REAL_MALLOC(r, n) __real__malloc_r(r, n)
#define LCS_REAL_FREE(r, p) __real__free_r(r, p)
#define LCS_REAL_MEMALIGN(r, a, n) __real__memalign_r(r, a, n)
#define LCS_REAL_USABLE(r, p) __real__malloc_usable_size_r(r, p)
#define LCS_WRAP(name) __wrap_##name
namespace {
struct ClassLock {
    Mutex mutex = 0;  // libnx: one exclusive load/store when uncontended
    void lock() { mutexLock(&mutex); }
    void unlock() { mutexUnlock(&mutex); }
};
}  // namespace
#endif

namespace {

constexpr std::size_t kSlabShift = 20u;
constexpr std::size_t kSlabSize = std::size_t{1} << kSlabShift;
constexpr std::size_t kMaxSmall = 256u * 1024u;
constexpr std::size_t kClassCount = 4u + 4u * 11u;  // 32..128, then 160..262144
constexpr std::size_t kNaturalAlignment = 32u;
constexpr std::size_t kTableSlots = 16384u;         // 16 GiB of heap

constexpr std::size_t class_size(std::size_t index) {
    if (index < 4u) return (index + 1u) * 32u;
    const std::size_t group = (index - 4u) / 4u;  // 0 => (128, 256]
    const std::size_t step = (index - 4u) % 4u + 1u;
    const std::size_t base = std::size_t{128} << group;
    return base + step * (base / 4u);
}
static_assert(class_size(0) == 32u && class_size(3) == 128u && class_size(4) == 160u);
static_assert(class_size(7) == 256u && class_size(8) == 320u && class_size(kClassCount - 1u) == kMaxSmall);
static_assert(kSlabSize % kNaturalAlignment == 0u && 160u % kNaturalAlignment == 0u);

inline std::size_t size_class(std::size_t size) {
    if (size <= 128u) return size == 0u ? 0u : (size - 1u) / 32u;
    const unsigned log = 63u - static_cast<unsigned>(__builtin_clzll(size - 1u));  // size in (2^log, 2^(log+1)]
    const std::size_t base = std::size_t{1} << log;
    const std::size_t step = base / 4u;
    const std::size_t k = (size - base + step - 1u) / step;  // 1..4
    return 4u + (log - 7u) * 4u + (k - 1u);
}

struct SizeClass {
    ClassLock lock;
    void *free_list{nullptr};
    char *bump{nullptr};
    char *bump_end{nullptr};
};

SizeClass g_classes[kClassCount];
// Per 1 MiB region of the heap: 0, or 1 + the class of our slab there.
unsigned char g_slab_class[kTableSlots];

inline std::uintptr_t heap_base() {
    return reinterpret_cast<std::uintptr_t>(LCS_HEAP_START) & ~(kSlabSize - 1u);
}

// The class of the slab holding `pointer`, or -1 when newlib owns it.
inline int owner_class(const void *pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (LCS_HEAP_START == nullptr || address < reinterpret_cast<std::uintptr_t>(LCS_HEAP_START) ||
        address >= reinterpret_cast<std::uintptr_t>(LCS_HEAP_END))
        return -1;
    const std::size_t slot = (address - heap_base()) >> kSlabShift;
    if (slot >= kTableSlots) return -1;
    const unsigned char entry = __atomic_load_n(&g_slab_class[slot], __ATOMIC_ACQUIRE);
    return entry == 0u ? -1 : static_cast<int>(entry) - 1;
}

// The start of the block that contains `pointer` (aligned allocations hand
// out a pointer inside their block).
inline void *block_start(void *pointer, std::size_t index) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const std::uintptr_t slab = address & ~(kSlabSize - 1u);
    const std::size_t size = class_size(index);
    return reinterpret_cast<void *>(slab + ((address - slab) / size) * size);
}

void *class_alloc([[maybe_unused]] struct _reent *r, std::size_t index) {
    SizeClass &cls = g_classes[index];
    const std::size_t size = class_size(index);
    cls.lock.lock();
    void *block = cls.free_list;
    if (block != nullptr) {
        cls.free_list = *static_cast<void **>(block);
    } else {
        if (cls.bump == nullptr || static_cast<std::size_t>(cls.bump_end - cls.bump) < size) {
            void *slab = LCS_REAL_MEMALIGN(r, kSlabSize, kSlabSize);
            const std::size_t slot = slab == nullptr ? kTableSlots
                : (reinterpret_cast<std::uintptr_t>(slab) - heap_base()) >> kSlabShift;
            if (slab == nullptr || slot >= kTableSlots) {
                if (slab != nullptr) LCS_REAL_FREE(r, slab);
                cls.lock.unlock();
                return nullptr;
            }
            __atomic_store_n(&g_slab_class[slot], static_cast<unsigned char>(index + 1u), __ATOMIC_RELEASE);
            cls.bump = static_cast<char *>(slab);
            cls.bump_end = cls.bump + (kSlabSize / size) * size;
        }
        block = cls.bump;
        cls.bump += size;
    }
    cls.lock.unlock();
    return block;
}

void class_free(void *pointer, std::size_t index) {
    void *block = block_start(pointer, index);
    SizeClass &cls = g_classes[index];
    cls.lock.lock();
    *static_cast<void **>(block) = cls.free_list;
    cls.free_list = block;
    cls.lock.unlock();
}

void *allocate(struct _reent *r, std::size_t size) {
    if (size <= kMaxSmall && LCS_HEAP_START != nullptr) {
        if (void *block = class_alloc(r, size_class(size))) return block;
    }
    return LCS_REAL_MALLOC(r, size);
}

void release([[maybe_unused]] struct _reent *r, void *pointer) {
    if (pointer == nullptr) return;
    const int index = owner_class(pointer);
    if (index >= 0) class_free(pointer, static_cast<std::size_t>(index));
    else LCS_REAL_FREE(r, pointer);
}

std::size_t usable_size([[maybe_unused]] struct _reent *r, void *pointer) {
    if (pointer == nullptr) return 0u;
    const int index = owner_class(pointer);
    if (index < 0) return LCS_REAL_USABLE(r, pointer);
    const auto block = reinterpret_cast<std::uintptr_t>(block_start(pointer, static_cast<std::size_t>(index)));
    return class_size(static_cast<std::size_t>(index)) - (reinterpret_cast<std::uintptr_t>(pointer) - block);
}

}  // namespace

extern "C" {

void *LCS_WRAP(_malloc_r)(struct _reent *r, std::size_t size) { return allocate(r, size); }

void LCS_WRAP(_free_r)(struct _reent *r, void *pointer) { release(r, pointer); }

std::size_t LCS_WRAP(_malloc_usable_size_r)(struct _reent *r, void *pointer) { return usable_size(r, pointer); }

void *LCS_WRAP(_calloc_r)(struct _reent *r, std::size_t count, std::size_t size) {
    if (size != 0u && count > static_cast<std::size_t>(-1) / size) return nullptr;
    void *block = allocate(r, count * size);
    if (block != nullptr) std::memset(block, 0, count * size);
    return block;
}

void *LCS_WRAP(_realloc_r)(struct _reent *r, void *pointer, std::size_t size) {
    if (pointer == nullptr) return allocate(r, size);
    if (size == 0u) {
        release(r, pointer);
        return nullptr;
    }
    const std::size_t old_size = usable_size(r, pointer);
    // Same class and not shrinking by half: keep the block.
    if (owner_class(pointer) >= 0 && size <= old_size && size > old_size / 2u) return pointer;
    void *block = allocate(r, size);
    if (block == nullptr) return nullptr;
    std::memcpy(block, pointer, old_size < size ? old_size : size);
    release(r, pointer);
    return block;
}

void *LCS_WRAP(_memalign_r)(struct _reent *r, std::size_t alignment, std::size_t size) {
    if (alignment <= kNaturalAlignment) return allocate(r, size);
    if ((alignment & (alignment - 1u)) != 0u) return nullptr;
    if (size + alignment <= kMaxSmall && LCS_HEAP_START != nullptr) {
        // A block with room to align inside it; free() finds the block's
        // start from the slab's class.
        void *block = class_alloc(r, size_class(size + alignment));
        if (block != nullptr) {
            const auto address = reinterpret_cast<std::uintptr_t>(block);
            return reinterpret_cast<void *>((address + alignment - 1u) & ~(alignment - 1u));
        }
    }
    // newlib's own memalign asks _malloc_r for more than size + alignment,
    // which is above kMaxSmall here, so that request stays with newlib too.
    return LCS_REAL_MEMALIGN(r, alignment, size);
}

}  // extern "C"
