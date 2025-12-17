#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>

namespace ouroboros::model {

enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
};

enum class RepeatMode {
    Off,
    One,
    All,
};

enum class AudioFormat {
    Unknown,
    MP3,
    FLAC,
    OGG,
    WAV,
};

struct Track {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string date;
    int track_number = 0;
    int duration_ms = 0;

    // Format information for playback
    AudioFormat format = AudioFormat::Unknown;
    int sample_rate = 44100;
    int channels = 2;
    int bit_depth = 16;
    int bitrate = 0;  // kbps, 0 for lossless

    // Artwork cache key (SHA-256 hash of artwork data)
    // Empty string if no artwork or not yet computed
    std::string artwork_hash;

    // Optimization fields for multi-tier caching (CACHE_VERSION 3)
    std::time_t file_mtime = 0;     // File modification time when cached
    uint64_t file_inode = 0;         // Inode for move/rename detection

    // Validation flag
    bool is_valid = true;
    std::string error_message;

    bool operator==(const Track&) const = default;
};

struct PlayerState {
    PlaybackState state = PlaybackState::Stopped;
    int volume_percent = 50;
    int playback_position_ms = 0;
    bool shuffle_enabled = false;
    RepeatMode repeat_mode = RepeatMode::Off;
    std::optional<int> current_track_index;  // Index into LibraryState::tracks, not full Track copy
    int64_t seek_request_ms = -1;

    bool operator==(const PlayerState&) const = default;
};

struct LibraryState {
    std::vector<Track> tracks;
    bool is_scanning = false;
    int scanned_count = 0;
    int total_count = 0;

    // Hierarchical cache: directory browsing support
    std::optional<std::string> current_directory;
    bool is_browsing_by_directory = false;

    bool operator==(const LibraryState&) const = default;
};

struct QueueState {
    std::vector<int> track_indices;  // Indices into LibraryState::tracks, not full Track copies
    size_t current_index = 0;

    bool operator==(const QueueState&) const = default;
};

struct UIState {
    std::string current_layout = "default";
    std::string current_theme = "dark";
    bool show_header = true;
    bool show_controls = true;
    bool show_browser = true;
    bool show_queue = true;
    bool show_spectrogram = false;
    
    bool operator==(const UIState&) const = default;
};

struct Alert {
    std::string level;  // "info", "warn", "crit"
    std::string message;
    std::chrono::steady_clock::time_point timestamp;
    
    bool operator==(const Alert&) const = default;
};

/// Snapshot uses Copy-On-Write (COW) optimization via shared_ptr:
///
/// DESIGN RATIONALE:
/// - LibraryState stores FULL Track objects (single source of truth)
/// - QueueState stores INDICES into LibraryState::tracks (memory efficient)
/// - PlayerState stores INDEX to current track (not full copy)
///
/// COW BENEFITS:
/// - Lock-free reads: UI can read snapshot without blocking updates
/// - Cheap copies: Copying Snapshot only copies shared_ptrs (16 bytes each)
/// - Immutability: Each snapshot is immutable - no partial updates visible
/// - Efficient updates: Only modified state gets copied (Library OR Queue, not both)
///
/// MEMORY FOOTPRINT:
/// - LibraryState (10k tracks): ~2-4 MB
/// - QueueState (100 tracks): ~400 bytes (100 * 4-byte ints)
///
/// WHY INDICES?
/// - Queue can reference same track multiple times (no data duplication)
/// - Metadata updates propagate automatically (all indices see new data)
///
/// THREAD SAFETY:
/// - All updates serialized by SnapshotPublisher::mutex_
/// - Double-buffering in SnapshotBuffers provides wait-free reads

struct Snapshot {
    uint64_t seq = 0;
    
    PlayerState player;
    std::shared_ptr<const LibraryState> library;
    std::shared_ptr<const QueueState> queue;
    UIState ui;
    std::vector<Alert> alerts;
    std::chrono::steady_clock::time_point timestamp;
    
    bool operator==(const Snapshot&) const = default;
};

}  // namespace ouroboros::model
