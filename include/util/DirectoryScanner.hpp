#pragma once

#include <filesystem>
#include <vector>
#include <string>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <ctime>

namespace ouroboros::util {

/**
 * DirectoryScanner: High-performance directory scanning using getdents64 syscall.
 *
 * Replaces std::filesystem::recursive_directory_iterator with direct kernel interface
 * for maximum performance. Uses 256KB buffers to batch syscalls and d_type field to
 * avoid unnecessary stat() calls.
 *
 * Performance gains:
 * - 2-3x faster than std::filesystem for large directories
 * - Single syscall fetches 3000+ directory entries
 * - Avoids stat() calls by using d_type from getdents64
 */
class DirectoryScanner {
public:
    /**
     * Result of a complete directory scan.
     */
    struct ScanResult {
        std::vector<std::string> audio_files;                    // All audio file paths (absolute)
        std::unordered_map<std::string, std::time_t> file_mtimes;  // Path → modification time
        std::unordered_map<std::string, std::time_t> dir_mtimes;   // Directory → modification time
        uint64_t tree_hash;                                        // Hash of all file paths (for TIER 0)
    };

    /**
     * Performs a complete directory scan using getdents64.
     *
     * @param root_dir Root directory to scan recursively
     * @return ScanResult containing all audio files, mtimes, and tree hash
     */
    [[nodiscard]] static ScanResult scan_directory(const std::filesystem::path& root_dir);

    /**
     * Scans only directories (not files) for TIER 1 validation.
     * Much faster than full scan - only checks directory structure.
     *
     * @param root_dir Root directory to scan
     * @return Map of directory path → modification time
     */
    [[nodiscard]] static std::unordered_map<std::string, std::time_t> scan_directories_only(
        const std::filesystem::path& root_dir
    );

    /**
     * Checks if a filename has an audio extension.
     *
     * @param filename Filename to check
     * @return true if filename ends with .mp3, .flac, .ogg, .wav, or .m4a
     */
    [[nodiscard]] static bool is_audio_extension(const char* filename);

private:
    static constexpr size_t BUFFER_SIZE = 256 * 1024;  // 256KB buffer for getdents64

    /**
     * Supported audio file extensions.
     */
    static constexpr std::array<std::string_view, 5> AUDIO_EXTENSIONS = {
        ".flac", ".m4a", ".mp3", ".ogg", ".wav"
    };

    /**
     * Recursively scan directory using getdents64 syscall.
     *
     * @param dir_path Directory to scan
     * @param root_path Root path for making relative paths
     * @param result Output ScanResult to populate
     */
    static void scan_directory_recursive(
        const std::string& dir_path,
        const std::string& root_path,
        ScanResult& result
    );

    /**
     * Recursively scan directories only (for TIER 1).
     *
     * @param dir_path Directory to scan
     * @param root_path Root path for making relative paths
     * @param dir_mtimes Output map to populate
     */
    static void scan_directories_recursive(
        const std::string& dir_path,
        const std::string& root_path,
        std::unordered_map<std::string, std::time_t>& dir_mtimes
    );

    /**
     * Compute SHA-256 hash of all file paths (for TIER 0 validation).
     *
     * @param paths Vector of file paths (sorted before hashing)
     * @return 64-bit truncated hash
     */
    static uint64_t compute_tree_hash(const std::vector<std::string>& paths);
};

}  // namespace ouroboros::util
