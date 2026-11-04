// Desktop check of the Switch build's allocator (lcs/switch/switch_malloc.cpp).
// The "real" allocator underneath is a locked bump allocator in a fake heap;
// four threads then allocate, grow, align and free blocks and check that no
// block was handed out twice or overwritten.

#define LCS_SWITCH_MALLOC_TEST 1
#include "../lcs/switch/switch_malloc.cpp"

#include <sys/mman.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <thread>
#include <vector>

namespace lcs_malloc_test {
char *heap_start = nullptr;
char *heap_end = nullptr;
namespace {
std::mutex real_mutex;
char *real_top = nullptr;
std::map<void *, std::size_t> real_blocks;
std::atomic<std::size_t> real_frees{0};
}  // namespace

void *real_memalign(std::size_t alignment, std::size_t size) {
    std::lock_guard<std::mutex> guard(real_mutex);
    if (alignment != kSlabSize && size >= 4096u) {
        // Big blocks: the system allocator, outside the fake heap (a bump
        // heap that never reuses memory would run out).
        void *block = std::aligned_alloc(alignment < 16u ? 16u : alignment,
                                         (size + alignment - 1u) / alignment * alignment);
        if (block != nullptr) real_blocks[block] = size;
        return block;
    }
    auto address = reinterpret_cast<std::uintptr_t>(real_top);
    address = (address + alignment - 1u) & ~(alignment - 1u);
    if (address + size > reinterpret_cast<std::uintptr_t>(heap_end)) return nullptr;
    real_top = reinterpret_cast<char *>(address + size);
    real_blocks[reinterpret_cast<void *>(address)] = size;
    return reinterpret_cast<void *>(address);
}
void *real_malloc(std::size_t size) { return real_memalign(16u, size == 0u ? 1u : size); }
void real_free(void *pointer) {
    std::lock_guard<std::mutex> guard(real_mutex);
    const auto found = real_blocks.find(pointer);
    if (found == real_blocks.end()) {
        std::fprintf(stderr, "FAIL real free of a pointer the real allocator never gave out\n");
        std::abort();
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address < reinterpret_cast<std::uintptr_t>(heap_start) ||
        address >= reinterpret_cast<std::uintptr_t>(heap_end))
        std::free(pointer);
    real_blocks.erase(found);
    ++real_frees;
}
std::size_t real_usable_size(void *pointer) {
    std::lock_guard<std::mutex> guard(real_mutex);
    const auto found = real_blocks.find(pointer);
    return found == real_blocks.end() ? 0u : found->second;
}
}  // namespace lcs_malloc_test

namespace {

int fail(const char *message) {
    std::fprintf(stderr, "FAIL %s\n", message);
    return 1;
}

struct Block {
    unsigned char *pointer{};
    std::size_t size{};
    unsigned char tag{};
};

bool intact(const Block &block) {
    for (std::size_t index = 0; index < block.size; ++index)
        if (block.pointer[index] != block.tag) return false;
    return true;
}

}  // namespace

int main() {
    constexpr std::size_t kHeap = std::size_t{1} << 30;
    void *region = mmap(nullptr, kHeap + kSlabSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) return fail("mmap");
    // Not 1 MiB aligned on purpose: the table works from the aligned-down start.
    lcs_malloc_test::heap_start = static_cast<char *>(region) + 4096u * 3u;
    lcs_malloc_test::heap_end = static_cast<char *>(region) + kHeap;
    lcs_malloc_test::real_top = lcs_malloc_test::heap_start;

    for (std::size_t size = 1u; size <= kMaxSmall; size += (size < 4096u ? 1u : 97u)) {
        const std::size_t index = size_class(size);
        if (index >= kClassCount || class_size(index) < size ||
            (index > 0u && class_size(index - 1u) >= size))
            return fail("size class");
    }

    // A pointer from the real allocator goes back to it.
    void *foreign = lcs_malloc_test::real_malloc(64u);
    test__free_r(nullptr, foreign);
    if (lcs_malloc_test::real_frees.load() != 1u) return fail("foreign free");

    // Calloc zeroes a recycled block.
    void *dirty = test__malloc_r(nullptr, 200u);
    std::memset(dirty, 0xAB, 200u);
    test__free_r(nullptr, dirty);
    auto *zeroed = static_cast<unsigned char *>(test__calloc_r(nullptr, 1u, 200u));
    for (std::size_t index = 0; index < 200u; ++index)
        if (zeroed[index] != 0u) return fail("calloc");
    test__free_r(nullptr, zeroed);

    // devkitA64's malloc asks memalign(32, n): every class block is 32-aligned.
    for (std::size_t size = 32u; size <= 4096u; size += 16u) {
        void *block = test__memalign_r(nullptr, 32u, size);
        if ((reinterpret_cast<std::uintptr_t>(block) & 31u) != 0u || owner_class(block) < 0)
            return fail("32-byte alignment");
        test__free_r(nullptr, block);
    }

    // Aligned blocks inside a class: free finds the block start.
    void *aligned = test__memalign_r(nullptr, 4096u, 1000u);
    if ((reinterpret_cast<std::uintptr_t>(aligned) & 4095u) != 0u) return fail("memalign");
    if (test__malloc_usable_size_r(nullptr, aligned) < 1000u) return fail("aligned usable size");
    test__free_r(nullptr, aligned);
    void *big_aligned = test__memalign_r(nullptr, 65536u, 300000u);
    if ((reinterpret_cast<std::uintptr_t>(big_aligned) & 65535u) != 0u) return fail("big memalign");
    test__free_r(nullptr, big_aligned);

    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < 4u; ++thread) {
        threads.emplace_back([thread, &failed] {
            std::mt19937 random(thread * 7919u + 1u);
            std::vector<Block> live;
            for (int step = 0; step < 200000 && !failed.load(); ++step) {
                const unsigned action = random() % 10u;
                if (live.empty() || (action < 5u && live.size() < 3000u)) {
                    const std::size_t size = random() % 8u == 0u ? random() % 400000u : random() % 2048u;
                    Block block;
                    block.size = size;
                    block.tag = static_cast<unsigned char>(random());
                    if (random() % 16u == 0u)
                        block.pointer = static_cast<unsigned char *>(test__memalign_r(nullptr, 64u << (random() % 6u), size));
                    else
                        block.pointer = static_cast<unsigned char *>(test__malloc_r(nullptr, size));
                    if (block.pointer == nullptr) { failed = true; break; }
                    std::memset(block.pointer, block.tag, size);
                    live.push_back(block);
                } else if (action < 7u) {
                    Block &block = live[random() % live.size()];
                    if (!intact(block)) { failed = true; break; }
                    const std::size_t size = random() % 3000u;
                    auto *grown = static_cast<unsigned char *>(test__realloc_r(nullptr, block.pointer, size));
                    if (size != 0u && grown == nullptr) { failed = true; break; }
                    for (std::size_t index = 0; index < std::min(size, block.size); ++index)
                        if (grown[index] != block.tag) { failed = true; break; }
                    block.pointer = grown;
                    block.size = size;
                    if (size != 0u) std::memset(grown, block.tag, size);
                    if (size == 0u) { block = live.back(); live.pop_back(); }
                } else {
                    const std::size_t at = random() % live.size();
                    if (!intact(live[at])) { failed = true; break; }
                    test__free_r(nullptr, live[at].pointer);
                    live[at] = live.back();
                    live.pop_back();
                }
            }
            for (const Block &block : live) {
                if (!intact(block)) failed = true;
                test__free_r(nullptr, block.pointer);
            }
        });
    }
    for (std::thread &thread : threads) thread.join();
    if (failed.load()) return fail("stress: a block was lost, shared or overwritten");
    std::printf("classes=%zu stress=4x200000 ok\n", kClassCount);
    return 0;
}
