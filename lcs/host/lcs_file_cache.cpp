#include "lcs_file_cache.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace lcs {
namespace {

// One archive is at most this big to be kept, and all of them together at
// most kBudget (the Switch build has about 3 GiB of heap).
constexpr std::uint64_t kMaxFileBytes = 384ull << 20;
constexpr std::uint64_t kBudget = 1024ull << 20;

enum class State { Queued, Loading, Ready, Refused };

struct Entry {
    State state{State::Queued};
    CachedFile bytes;
};

class FileCache {
public:
    ~FileCache() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    CachedFile get(const std::filesystem::path &path) {
        const std::string key = path.generic_string();
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end()) return found->second.state == State::Ready ? found->second.bytes : nullptr;
        std::error_code error;
        const std::uint64_t size = std::filesystem::file_size(path, error);
        if (error || !file_cache_wants(path, size) || used_ + size > kBudget) {
            entries_[key].state = State::Refused;
            return nullptr;
        }
        used_ += size;
        entries_[key].state = State::Queued;
        queue_.push_back(key);
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
        wake_.notify_all();
        return nullptr;
    }

    void wait_idle() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && !loading_; });
    }

private:
    void run() {
        for (;;) {
            std::string key;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                key = queue_.front();
                queue_.pop_front();
                entries_[key].state = State::Loading;
                loading_ = true;
            }
            const auto start = std::chrono::steady_clock::now();
            auto bytes = std::make_shared<std::vector<std::uint8_t>>();
            bool ok = false;
            {
                std::ifstream file(std::filesystem::path(key), std::ios::binary);
                std::error_code error;
                const std::uint64_t size = std::filesystem::file_size(std::filesystem::path(key), error);
                if (file && !error) {
                    bytes->resize(static_cast<std::size_t>(size));
                    // In pieces, so the game's own reads get the card between them.
                    constexpr std::size_t kPiece = 4u << 20;
                    std::size_t done = 0u;
                    while (done < bytes->size() && file) {
                        const std::size_t piece = std::min(kPiece, bytes->size() - done);
                        file.read(reinterpret_cast<char *>(bytes->data() + done), static_cast<std::streamsize>(piece));
                        done += static_cast<std::size_t>(file.gcount());
                    }
                    ok = done == bytes->size();
                }
            }
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            {
                std::lock_guard lock(mutex_);
                Entry &entry = entries_[key];
                if (ok) {
                    entry.state = State::Ready;
                    entry.bytes = std::move(bytes);
                } else {
                    entry.state = State::Refused;
                }
                loading_ = false;
            }
            idle_.notify_all();
            std::printf("[file-cache] %s %s (%.1f MiB, %.2f s)\n",
                        std::filesystem::path(key).filename().string().c_str(), ok ? "in memory" : "could not be read",
                        ok ? static_cast<double>(entries_size(key)) / 1048576.0 : 0.0, seconds);
            std::fflush(stdout);
        }
    }

    std::size_t entries_size(const std::string &key) {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        return found != entries_.end() && found->second.bytes ? found->second.bytes->size() : 0u;
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> queue_;
    std::uint64_t used_{};
    bool loading_{};
    bool stop_{};
    std::thread thread_;
};

FileCache &cache() {
    static FileCache instance;
    return instance;
}

}  // namespace

bool file_cache_enabled() noexcept {
    static const bool enabled = [] {
        const char *text = std::getenv("LCS_FILE_CACHE");
        if (text != nullptr && *text != '\0') return std::strcmp(text, "0") != 0;
#if defined(__SWITCH__)
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}

bool file_cache_wants(const std::filesystem::path &path, std::uint64_t size) noexcept {
    if (size == 0u || size > kMaxFileBytes) return false;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return extension == ".IMG";
}

CachedFile cached_file(const std::filesystem::path &path) {
    if (!file_cache_enabled()) return nullptr;
    return cache().get(path);
}

std::int64_t read_cached_file(const std::filesystem::path &path, std::uint64_t offset,
                              std::span<std::uint8_t> output) {
    const CachedFile bytes = cached_file(path);
    if (!bytes) return -1;
    if (offset >= bytes->size()) return 0;
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(output.size(), bytes->size() - offset));
    std::memcpy(output.data(), bytes->data() + offset, count);
    return static_cast<std::int64_t>(count);
}

void file_cache_wait_idle() {
    if (file_cache_enabled()) cache().wait_idle();
}

}  // namespace lcs
