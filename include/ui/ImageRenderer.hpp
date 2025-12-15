#pragma once
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <future>

namespace ouroboros::ui {

enum class ImageProtocol {
    Sixel,        // xterm, mlterm, mintty, iTerm2, WezTerm - MOST COMPATIBLE
    Kitty,        // Kitty terminal only
    ITerm2,       // iTerm2 inline images
    None          // Fallback to no images
};

struct CachedImage {
    size_t data_hash;
    int cols;
    int rows;
    std::string encoded_data; // The final string sent to terminal (Sixel/Base64/etc)
};

class ImageRenderer {
public:
    static ImageRenderer& instance();
    
    // Detect what the terminal supports
    void detect_protocol();
    ImageProtocol get_protocol() const { return protocol_; }
    
    // Control album art display from config
    void set_album_art_enabled(bool enabled) { album_art_enabled_ = enabled; }
    bool is_album_art_enabled() const { return album_art_enabled_; }
    
    // Render image at terminal position (x, y) with size (cols, rows)
    // Returns true if successful
    bool render_image(
        const std::vector<unsigned char>& image_data,
        int x, int y,
        int width_cols, int height_rows,
        const std::string& content_hash = "",
        int visible_rows = -1 // < 0 means fully visible
    );

    // Pre-decode image into cache without rendering
    // Returns true if already cached
    bool preload_image(
        const std::vector<unsigned char>& image_data,
        int width_cols, int height_rows,
        const std::string& content_hash = ""
    );
    
    // Clear image area
    // Clear specific image by hash (Kitty)
    void delete_image(const std::string& content_hash);

    void clear_image(int x, int y, int width_cols, int height_rows);

    // Check if images are supported (always true with Unicode fallback)
    bool images_supported() const { return album_art_enabled_; }

    // Check if async jobs have completed (for main loop render trigger)
    bool has_pending_updates();
    void clear_pending_updates();

    // Debug helper
    std::string protocol_name(ImageProtocol proto);
    
    // Helper to write data to a temporary file in /dev/shm (RAM)
    std::string write_to_temp_file(const unsigned char* data, size_t len);

private:
    ImageRenderer();
    ~ImageRenderer() = default;
    
    ImageProtocol protocol_ = ImageProtocol::None;
    bool album_art_enabled_ = true;
    int cell_width_ = 8;   // Pixels per column
    int cell_height_ = 16; // Pixels per row
    
    // Detect cell size from terminal
    void detect_cell_size();
    
    // Terminal capability queries (system-agnostic)
    bool query_kitty_support();
    bool query_sixel_support();
    std::string query_da1();
    
    // Render Logic
    std::string render_kitty(const unsigned char* data, size_t len, int cols, int rows, size_t data_hash, const std::string& content_hash);
    std::string render_iterm2(const unsigned char* rgba, int w, int h, int cols, int rows);
    std::string render_sixel(const unsigned char* rgba, int w, int h, int cols, int rows);
    std::string render_unicode_blocks(const unsigned char* rgba, int w, int h, int cols, int rows);

    // Utilities
    std::string encode_base64(const unsigned char* data, size_t len);
    std::vector<unsigned char> resize_image(const unsigned char* pixels, int w, int h, int target_w, int target_h, int channels);
    std::string rgb_to_sixel(const unsigned char* rgb, int w, int h);
    std::string rgb_to_ansi_truecolor(unsigned char r, unsigned char g, unsigned char b, bool bg);
    
    // Internal helper to check futures
    void poll_jobs();

    // LRU Cache
    struct CacheKey {
        size_t data_hash;
        int width;
        int height;
        
        bool operator==(const CacheKey& other) const {
            return data_hash == other.data_hash && width == other.width && height == other.height;
        }
    };
    
    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const {
            return k.data_hash ^ (std::hash<int>()(k.width) << 1) ^ (std::hash<int>()(k.height) << 2);
        }
    };

    // Cache resized RGBA pixel data, not encoded strings (much smaller memory footprint)
    struct CachedPixels {
        std::vector<uint8_t> rgba;
        int width;
        int height;
        uint32_t image_id = 0; // Kitty Image ID for re-placement
        bool valid = false; // To indicate success/failure of async job
    };

    static constexpr size_t MAX_CACHE_SIZE = 100; // Larger cache for fast scrolling (prevents re-decoding)
    std::list<CacheKey> lru_list_;
    std::unordered_map<CacheKey, CachedPixels, CacheKeyHash> cache_;
    
    // Async processing
    // We use a separate map to track pending jobs
    std::unordered_map<CacheKey, std::future<CachedPixels>, CacheKeyHash> pending_jobs_;
    // Store hashes of invalid/corrupt images to prevent infinite retry loops
    std::unordered_set<size_t> failed_hashes_;
    // Flag to notify main loop when async jobs complete
    std::atomic<bool> has_updates_{false};

};

} // namespace ouroboros::ui