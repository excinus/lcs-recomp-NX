#pragma once

// Game archives kept in memory.
//
// While driving, the game streams models and world sectors out of the .IMG
// archives, and the view-distance option reads nine neighbouring sectors
// (several MB) each time the current one changes. On the Switch every read is
// a request to the SD card's file system service on the game thread, which
// held whole frames up. The first time an archive is read it is queued for a
// background thread that reads the whole file; from then on reads are copies.
// Until it is in, reads go to the file as before.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace lcs {

using CachedFile = std::shared_ptr<const std::vector<std::uint8_t>>;

// LCS_FILE_CACHE=0 turns it off; on by default on the Switch only.
[[nodiscard]] bool file_cache_enabled() noexcept;

// The file's bytes when they are in memory, else null (and, for a file worth
// keeping, its load starts in the background). Thread-safe.
[[nodiscard]] CachedFile cached_file(const std::filesystem::path &path);

// Copies `output.size()` bytes from `offset` when the file is in memory;
// returns how many (0 past the end), or -1 when it is not in memory.
[[nodiscard]] std::int64_t read_cached_file(const std::filesystem::path &path, std::uint64_t offset,
                                            std::span<std::uint8_t> output);

// Whether a file is one this cache keeps (an .IMG archive below the size cap).
[[nodiscard]] bool file_cache_wants(const std::filesystem::path &path, std::uint64_t size) noexcept;

// Waits for queued loads (tests).
void file_cache_wait_idle();

}  // namespace lcs
