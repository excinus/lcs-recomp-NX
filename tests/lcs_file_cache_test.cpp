// lcs_file_cache: an .IMG archive is read once in the background and then
// served from memory; other files are left to the caller.
#include "lcs_file_cache.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lcs_file_cache_test";
    fs::create_directories(dir);
    const fs::path archive = dir / "COMMER.IMG";
    const fs::path other = dir / "GAME.DTZ";
    std::vector<std::uint8_t> bytes(3u * 1024u * 1024u + 123u);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>((index * 2654435761u) >> 13u);
    for (const fs::path &path : {archive, other}) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    int failures = 0;
    if (!lcs::file_cache_enabled()) {
        std::printf("FAIL cache off (run with LCS_FILE_CACHE=1)\n");
        return 1;
    }
    std::vector<std::uint8_t> out(4096u);
    // The first read queues the load and is not served.
    if (lcs::read_cached_file(archive, 0u, out) != -1) ++failures;
    lcs::file_cache_wait_idle();
    for (const std::uint64_t offset : {0ull, 1000ull, 2097152ull, static_cast<unsigned long long>(bytes.size() - 100u)}) {
        const std::int64_t got = lcs::read_cached_file(archive, offset, out);
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), bytes.size() - offset));
        if (got != static_cast<std::int64_t>(want)) { ++failures; continue; }
        for (std::size_t index = 0; index < want; ++index)
            if (out[index] != bytes[offset + index]) { ++failures; break; }
    }
    if (lcs::read_cached_file(archive, bytes.size() + 5u, out) != 0) ++failures;
    if (lcs::read_cached_file(other, 0u, out) != -1) ++failures;
    lcs::file_cache_wait_idle();
    if (lcs::read_cached_file(other, 0u, out) != -1) ++failures;  // not an archive: never kept
    fs::remove_all(dir);
    std::printf("%s file cache (%d bad)\n", failures == 0 ? "ok" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
