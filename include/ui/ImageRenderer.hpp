#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace ouroboros::ui {

enum class ImageProtocol {
    Sixel,        // xterm, mlterm, mintty, iTerm2, WezTerm - MOST COMPATIBLE
    Kitty,        // Kitty terminal only
    ITerm2,       // iTerm2 inline images
    None          // Fallback to no images
};

enum class TerminalType {
    Kitty,        // Kitty terminal (gold standard, all features work)
    Ghostty,      // Ghostty terminal (Kitty protocol with quirks)
    Other         // Other terminals (WezTerm, Alacritty, etc.)
};

// Format of image data for rendering
enum class CachedFormat {
    RGB,  // Raw RGB pixels (transmitted with f=24)
    PNG   // PNG-encoded data with transparency (transmitted with f=100)
};

class ImageRenderer {
public:
    static ImageRenderer& instance();

    // Detect what the terminal supports
    void detect_protocol();
    [[nodiscard]] ImageProtocol get_protocol() const { return protocol_; }

    // Control album art display from config
    void set_album_art_enabled(bool enabled) { album_art_enabled_ = enabled; }
    [[nodiscard]] bool is_album_art_enabled() const { return album_art_enabled_; }

    // Render pre-decoded pixels at terminal position
    // data is RGB or PNG pixels from ArtworkWindow
    // Returns image ID if successful, 0 if failed
    [[nodiscard]] uint32_t render_image(
        const uint8_t* data,
        size_t data_size,
        int data_width,
        int data_height,
        CachedFormat format,
        int x, int y,
        int width_cols, int height_rows,
        const std::string& content_hash = "",
        int visible_rows = -1
    );

    // Clear specific image by hash (Kitty)
    void delete_image(const std::string& content_hash);
    void delete_image_by_id(uint32_t id);

    // Clear image area
    void clear_image(int x, int y, int width_cols, int height_rows);

    // Check if images are supported
    [[nodiscard]] bool images_supported() const { return album_art_enabled_; }

    // Debug helper
    [[nodiscard]] std::string protocol_name(ImageProtocol proto);

    // Helper to write data to a temporary file in /dev/shm (RAM)
    [[nodiscard]] std::string write_to_temp_file(const unsigned char* data, size_t len);

    // Cell size accessors
    [[nodiscard]] int get_cell_width() const { return cell_width_; }
    [[nodiscard]] int get_cell_height() const { return cell_height_; }

private:
    ImageRenderer();
    ~ImageRenderer() = default;

    ImageProtocol protocol_ = ImageProtocol::None;
    bool album_art_enabled_ = true;
    int cell_width_ = 8;   // Pixels per column
    int cell_height_ = 16; // Pixels per row

    // Terminal detection and quirk handling
    TerminalType terminal_type_ = TerminalType::Other;
    bool terminal_respects_image_ids_ = true;      // false for Ghostty (Issue #6711)
    bool terminal_supports_temp_file_ = true;      // false for Ghostty (Issue #5774)

    // Detect cell size from terminal
    void detect_cell_size();

    // Terminal detection and capability queries
    void detect_terminal_type();
    bool query_kitty_support();
    bool query_sixel_support();
    std::string query_da1();

    // Render Logic
    std::string render_kitty(const unsigned char* data, size_t len, int cols, int rows, size_t data_hash, const std::string& content_hash, uint32_t& out_id, CachedFormat format);
    std::string render_iterm2(const unsigned char* rgba, int w, int h, int cols, int rows);
    std::string render_sixel(const unsigned char* rgba, int w, int h, int cols, int rows);

    // Utilities
    std::string encode_base64(const unsigned char* data, size_t len);
    std::vector<unsigned char> resize_image(const unsigned char* pixels, int w, int h, int target_w, int target_h, int channels);
    std::string rgb_to_sixel(const unsigned char* rgb, int w, int h);

    // Track IDs transmitted to terminal (for placement optimization)
    std::unordered_set<uint32_t> transmitted_ids_;

    // Track what's currently displayed at each position to skip redundant renders
    // Key: (x << 16 | y), Value: image_id
    std::unordered_map<uint32_t, uint32_t> displayed_at_position_;
};

} // namespace ouroboros::ui
