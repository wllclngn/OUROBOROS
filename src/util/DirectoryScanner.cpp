#include "util/DirectoryScanner.hpp"
#include "util/ArtworkHasher.hpp"
#include "util/Logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>

namespace ouroboros::util {

// Linux dirent64 structure for getdents64 syscall
struct linux_dirent64 {
    uint64_t d_ino;           // Inode number
    int64_t  d_off;           // Offset to next structure
    uint16_t d_reclen;        // Size of this dirent
    uint8_t  d_type;          // File type
    char     d_name[];        // Filename (null-terminated)
};

// File type constants from dirent.h
constexpr uint8_t DT_UNKNOWN = 0;
constexpr uint8_t DT_REG = 8;     // Regular file
constexpr uint8_t DT_DIR = 4;     // Directory

bool DirectoryScanner::is_audio_extension(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;

    std::string_view sv(ext);
    for (const auto& ae : AUDIO_EXTENSIONS) {
        if (ae == sv) return true;
    }
    return false;
}

DirectoryScanner::ScanResult DirectoryScanner::scan_directory(
    const std::filesystem::path& root_dir
) {
    ScanResult result;

    // Normalize: strip trailing slashes to prevent // in paths
    std::string root_str = root_dir.string();
    while (root_str.length() > 1 && root_str.back() == '/') {
        root_str.pop_back();
    }
    util::Logger::info("DirectoryScanner: Starting getdents64 scan of " + root_str);

    scan_directory_recursive(root_str, root_str, result);

    // Compute tree hash for TIER 0 validation
    result.tree_hash = compute_tree_hash(result.audio_files);

    util::Logger::info("DirectoryScanner: Found " + std::to_string(result.audio_files.size()) +
                      " audio files in " + std::to_string(result.dir_mtimes.size()) + " directories");

    return result;
}

std::unordered_map<std::string, std::time_t> DirectoryScanner::scan_directories_only(
    const std::filesystem::path& root_dir
) {
    std::unordered_map<std::string, std::time_t> dir_mtimes;

    // Normalize: strip trailing slashes to prevent // in paths
    std::string root_str = root_dir.string();
    while (root_str.length() > 1 && root_str.back() == '/') {
        root_str.pop_back();
    }
    util::Logger::info("DirectoryScanner: Scanning directories only (TIER 1)");

    scan_directories_recursive(root_str, root_str, dir_mtimes);

    util::Logger::info("DirectoryScanner: Found " + std::to_string(dir_mtimes.size()) + " directories");

    return dir_mtimes;
}

void DirectoryScanner::scan_directory_recursive(
    const std::string& dir_path,
    const std::string& root_path,
    ScanResult& result
) {
    // Open directory for reading
    int fd = open(dir_path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        util::Logger::debug("DirectoryScanner: Failed to open directory: " + dir_path);
        return;
    }

    // Get directory mtime
    struct stat dir_stat;
    if (fstat(fd, &dir_stat) == 0) {
        std::string rel_path = dir_path;
        if (rel_path.starts_with(root_path)) {
            rel_path = rel_path.substr(root_path.length());
            if (rel_path.empty()) rel_path = "/";
        }
        result.dir_mtimes[rel_path] = dir_stat.st_mtime;
    }

    // Allocate large buffer for getdents64
    char buffer[BUFFER_SIZE];

    while (true) {
        // Call getdents64 syscall directly
        long nread = syscall(SYS_getdents64, fd, buffer, BUFFER_SIZE);

        if (nread == -1) {
            util::Logger::error("DirectoryScanner: getdents64 failed for " + dir_path);
            break;
        }

        if (nread == 0) {
            // End of directory
            break;
        }

        // Process all entries in buffer
        for (long pos = 0; pos < nread;) {
            auto* d = reinterpret_cast<linux_dirent64*>(buffer + pos);

            // Skip . and ..
            if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0) {
                pos += d->d_reclen;
                continue;
            }

            std::string full_path = dir_path + "/" + d->d_name;

            if (d->d_type == DT_REG) {
                // Regular file - check if audio
                if (is_audio_extension(d->d_name)) {
                    result.audio_files.push_back(full_path);

                    // Get file mtime using fstatat (efficient - no path lookup)
                    struct stat file_stat;
                    if (fstatat(fd, d->d_name, &file_stat, 0) == 0) {
                        result.file_mtimes[full_path] = file_stat.st_mtime;
                    }
                }
            } else if (d->d_type == DT_DIR) {
                // Recurse into subdirectory
                scan_directory_recursive(full_path, root_path, result);
            } else if (d->d_type == DT_UNKNOWN) {
                // Filesystem doesn't support d_type, fall back to stat
                struct stat entry_stat;
                if (fstatat(fd, d->d_name, &entry_stat, 0) == 0) {
                    if (S_ISREG(entry_stat.st_mode)) {
                        if (is_audio_extension(d->d_name)) {
                            result.audio_files.push_back(full_path);
                            result.file_mtimes[full_path] = entry_stat.st_mtime;
                        }
                    } else if (S_ISDIR(entry_stat.st_mode)) {
                        scan_directory_recursive(full_path, root_path, result);
                    }
                }
            }

            pos += d->d_reclen;
        }
    }

    close(fd);
}

void DirectoryScanner::scan_directories_recursive(
    const std::string& dir_path,
    const std::string& root_path,
    std::unordered_map<std::string, std::time_t>& dir_mtimes
) {
    // Open directory
    int fd = open(dir_path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return;
    }

    // Get directory mtime
    struct stat dir_stat;
    if (fstat(fd, &dir_stat) == 0) {
        std::string rel_path = dir_path;
        if (rel_path.starts_with(root_path)) {
            rel_path = rel_path.substr(root_path.length());
            if (rel_path.empty()) rel_path = "/";
        }
        dir_mtimes[rel_path] = dir_stat.st_mtime;
    }

    // Scan for subdirectories only
    char buffer[BUFFER_SIZE];

    while (true) {
        long nread = syscall(SYS_getdents64, fd, buffer, BUFFER_SIZE);

        if (nread <= 0) break;

        for (long pos = 0; pos < nread;) {
            auto* d = reinterpret_cast<linux_dirent64*>(buffer + pos);

            if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0) {
                if (d->d_type == DT_DIR) {
                    std::string full_path = dir_path + "/" + d->d_name;
                    scan_directories_recursive(full_path, root_path, dir_mtimes);
                } else if (d->d_type == DT_UNKNOWN) {
                    // Fall back to stat
                    struct stat entry_stat;
                    if (fstatat(fd, d->d_name, &entry_stat, 0) == 0) {
                        if (S_ISDIR(entry_stat.st_mode)) {
                            std::string full_path = dir_path + "/" + d->d_name;
                            scan_directories_recursive(full_path, root_path, dir_mtimes);
                        }
                    }
                }
            }

            pos += d->d_reclen;
        }
    }

    close(fd);
}

uint64_t DirectoryScanner::compute_tree_hash(const std::vector<std::string>& paths) {
    // Sort paths for deterministic hashing
    std::vector<std::string> sorted_paths = paths;
    std::sort(sorted_paths.begin(), sorted_paths.end());

    // Concatenate all paths with newlines
    std::string concatenated;
    for (const auto& path : sorted_paths) {
        concatenated += path;
        concatenated += '\n';
    }

    // Compute SHA-256 hash using our custom implementation
    auto hash = ArtworkHasher::sha256(
        reinterpret_cast<const uint8_t*>(concatenated.data()),
        concatenated.size()
    );

    // Truncate to uint64_t (first 8 bytes)
    uint64_t result = 0;
    std::memcpy(&result, hash.data(), sizeof(result));

    return result;
}

}  // namespace ouroboros::util
