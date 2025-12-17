#include "ui/ImageRenderer.hpp"
#include "ui/Terminal.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstdlib>
#include <termios.h>
#include <fcntl.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"
#include "util/Logger.hpp"
#include "util/ImageDecoderPool.hpp"
#include "util/ArtworkHasher.hpp"

namespace ouroboros::ui {

ImageRenderer& ImageRenderer::instance() {
    static ImageRenderer instance;
    return instance;
}

ImageRenderer::ImageRenderer() {
    detect_protocol();
    detect_cell_size();
    
}

void ImageRenderer::detect_protocol() {
    ouroboros::util::Logger::info("ImageRenderer: Detecting protocol");

    // Detect terminal type first (sets quirk flags)
    detect_terminal_type();

    // Allow user to force a specific protocol via environment variable
    const char* force_protocol = std::getenv("OUROBOROS_IMAGE_PROTOCOL");
    if (force_protocol) {
        std::string proto_str(force_protocol);
        if (proto_str == "kitty") {
            protocol_ = ImageProtocol::Kitty;
            ouroboros::util::Logger::info("ImageRenderer: Forced Kitty protocol via OUROBOROS_IMAGE_PROTOCOL");
            return;
        } else if (proto_str == "sixel") {
            protocol_ = ImageProtocol::Sixel;
            ouroboros::util::Logger::info("ImageRenderer: Forced Sixel protocol via OUROBOROS_IMAGE_PROTOCOL");
            return;
        } else if (proto_str == "iterm2") {
            protocol_ = ImageProtocol::ITerm2;
            ouroboros::util::Logger::info("ImageRenderer: Forced iTerm2 protocol via OUROBOROS_IMAGE_PROTOCOL");
            return;
        } else if (proto_str == "none") {
            protocol_ = ImageProtocol::None;
            ouroboros::util::Logger::info("ImageRenderer: Forced no images via OUROBOROS_IMAGE_PROTOCOL");
            return;
        }
    }


    if (query_kitty_support()) {
        protocol_ = ImageProtocol::Kitty;
        return;
    }
    
    if (query_sixel_support()) {
        protocol_ = ImageProtocol::Sixel;
        return;
    }
    
    std::string da1 = query_da1();
    
    if (da1.find("iTerm") != std::string::npos) {
        protocol_ = ImageProtocol::ITerm2;
        return;
    }
    
    protocol_ = ImageProtocol::None;
}

void ImageRenderer::detect_terminal_type() {
    // 1. Check TERM_PROGRAM (most reliable for modern terminals)
    const char* term_prog = std::getenv("TERM_PROGRAM");
    if (term_prog) {
        if (std::string(term_prog) == "ghostty") {
            terminal_type_ = TerminalType::Ghostty;
            // Ghostty optimizations (assuming recent version fixes quirks)
            terminal_respects_image_ids_ = false;
            terminal_supports_temp_file_ = false;
            ouroboros::util::Logger::info("ImageRenderer: Detected Ghostty terminal");
            return;
        }
    }

    // 2. Check Kitty-specific environment variables
    if (std::getenv("KITTY_WINDOW_ID") || std::getenv("KITTY_PID")) {
        terminal_type_ = TerminalType::Kitty;
        terminal_respects_image_ids_ = true;   // Kitty: full support
        terminal_supports_temp_file_ = true;   // Kitty: full support
        ouroboros::util::Logger::info("ImageRenderer: Detected Kitty terminal");
        return;
    }

    // 3. Check TERM variable
    const char* term_env = std::getenv("TERM");
    if (term_env) {
        std::string term_str(term_env);

        // Ghostty detection via TERM
        if (term_str.find("ghostty") != std::string::npos || term_str == "xterm-ghostty") {
            terminal_type_ = TerminalType::Ghostty;
            terminal_respects_image_ids_ = false;
            terminal_supports_temp_file_ = false;
            ouroboros::util::Logger::info("ImageRenderer: Detected Ghostty terminal via TERM");
            return;
        }

        // Kitty detection via TERM
        if (term_str.find("kitty") != std::string::npos) {
            terminal_type_ = TerminalType::Kitty;
            terminal_respects_image_ids_ = true;
            terminal_supports_temp_file_ = true;
            ouroboros::util::Logger::info("ImageRenderer: Detected Kitty terminal via TERM");
            return;
        }
    }

    // 4. Default: assume Other terminal with standard behavior
    terminal_type_ = TerminalType::Other;
    terminal_respects_image_ids_ = true;   // Assume standard behavior
    terminal_supports_temp_file_ = true;   // Assume standard behavior
    ouroboros::util::Logger::debug("ImageRenderer: Unknown terminal, assuming standard behavior");
}

bool ImageRenderer::query_kitty_support() {
    ouroboros::util::Logger::debug("ImageRenderer: Querying Kitty support");

    // Check KITTY specific env vars first (most reliable if TERM is overridden)
    if (std::getenv("KITTY_WINDOW_ID") || std::getenv("KITTY_PID")) {
        return true;
    }

    // Fast path: check TERM environment variable
    const char* term_env = std::getenv("TERM");
    if (term_env) {
        std::string term_str(term_env);

        // Check for Ghostty terminal (supports Kitty graphics protocol)
        if (term_str.find("ghostty") != std::string::npos || term_str == "xterm-ghostty") {
            ouroboros::util::Logger::info("ImageRenderer: Ghostty terminal detected, using Kitty graphics protocol");
            return true;
        }

        // Check for Kitty terminal
        if (term_str.find("kitty") != std::string::npos) {
            ouroboros::util::Logger::info("ImageRenderer: Kitty terminal detected via TERM");
            return true;
        }
    }


    struct termios old_term, new_term;
    if (tcgetattr(STDIN_FILENO, &old_term) != 0) return false;
    
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    new_term.c_cc[VMIN] = 0;
    new_term.c_cc[VTIME] = 0;
    
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_term) != 0) return false;
    
    tcflush(STDIN_FILENO, TCIFLUSH);
    
    const char* query = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\";
    if (write(STDOUT_FILENO, query, strlen(query)) < 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        return false;
    }
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    
    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    
    char buf[256] = {0};
    ssize_t n = 0;
    
    if (ret > 0) n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    
    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, "_G") != nullptr) return true;
    }
    
    return false;
}

bool ImageRenderer::query_sixel_support() {
    ouroboros::util::Logger::debug("ImageRenderer: Querying Sixel support");

    
    struct termios old_term, new_term;
    if (tcgetattr(STDIN_FILENO, &old_term) != 0) return false;
    
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    new_term.c_cc[VMIN] = 0;
    new_term.c_cc[VTIME] = 0;
    
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_term) != 0) return false;
    
    tcflush(STDIN_FILENO, TCIFLUSH);
    
    const char* query = "\033[c";
    if (write(STDOUT_FILENO, query, strlen(query)) < 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        return false;
    }
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    
    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    
    char buf[256] = {0};
    ssize_t n = 0;
    
    if (ret > 0) n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    
    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, ";4") != nullptr) return true;
    }
    
    return false;
}

std::string ImageRenderer::query_da1() {
    struct termios old_term, new_term;
    if (tcgetattr(STDIN_FILENO, &old_term) != 0) return "";
    
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    new_term.c_cc[VMIN] = 0;
    new_term.c_cc[VTIME] = 0;
    
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_term) != 0) return "";
    
    tcflush(STDIN_FILENO, TCIFLUSH);
    
    const char* query = "\033[c";
    if (write(STDOUT_FILENO, query, strlen(query)) < 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        return "";
    }
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    
    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    
    char buf[256] = {0};
    ssize_t n = 0;
    
    if (ret > 0) n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf);
    }
    return "";
}

void ImageRenderer::detect_cell_size() {
    // For Kitty terminal, query actual cell size using TIOCGWINSZ ioctl
    if (protocol_ == ImageProtocol::Kitty) {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_xpixel > 0 && w.ws_ypixel > 0) {
            // Calculate pixel size per character cell
            cell_width_ = w.ws_xpixel / w.ws_col;
            cell_height_ = w.ws_ypixel / w.ws_row;
            ouroboros::util::Logger::info("ImageRenderer: Detected Kitty cell size: " +
                                          std::to_string(cell_width_) + "×" +
                                          std::to_string(cell_height_) + " pixels");
            return;
        }
    }

    // Fallback to common terminal defaults
    cell_width_ = 8;
    cell_height_ = 16;
    ouroboros::util::Logger::info("ImageRenderer: Using default cell size: 8×16 pixels");
}

uint32_t ImageRenderer::render_image(
    const std::vector<unsigned char>& image_data,
    int x, int y,
    int width_cols, int height_rows,
    const std::string& content_hash,
    int visible_rows
) {
    if (!album_art_enabled_ || image_data.empty()) return 0;

    // 1. Ensure image is in pipeline (Cache check, Pending check, or Start Decode)
    // preload_image handles all the async logic and hash calculation
    bool is_cached = preload_image(image_data, width_cols, height_rows, content_hash);

    if (!is_cached) return 0;

    // 2. Retrieve from Cache and Render
    // Re-calculate hash to look up in cache (fast)
    size_t data_hash;
    if (!content_hash.empty()) {
        data_hash = std::stoull(content_hash.substr(0, 16), nullptr, 16);
    } else {
        data_hash = util::ArtworkHasher::fast_hash(image_data);
    }

    CacheKey key{data_hash, width_cols, height_rows};
    auto it = cache_.find(key);
    
    if (it != cache_.end()) {
        std::string encoded;
        uint32_t out_id = 0;
        
        // Determine render dimensions (handling partial visibility)
        int render_rows = height_rows;
        size_t render_size = it->second.width * it->second.height * 3; // Default full size

        if (visible_rows > 0 && visible_rows < height_rows) {
            render_rows = visible_rows;
            // Crop the pixel data by reducing the size
            // (RGB data is row-major, so taking the first N bytes crops the bottom)
            int crop_h = visible_rows * cell_height_;
            if (crop_h > it->second.height) crop_h = it->second.height;
            render_size = it->second.width * crop_h * 3;
        }

        if (protocol_ == ImageProtocol::Kitty) {
            // Pass SHA-256-derived data_hash and content_hash for stable, collision-free image_id
            encoded = render_kitty(it->second.rgba.data(), render_size, width_cols, render_rows, data_hash, content_hash, out_id);
        } else if (protocol_ == ImageProtocol::ITerm2) {
            encoded = render_iterm2(it->second.rgba.data(), it->second.width, it->second.height, width_cols, render_rows);
        } else if (protocol_ == ImageProtocol::Sixel) {
            encoded = render_sixel(it->second.rgba.data(), it->second.width, it->second.height, width_cols, render_rows);
        }

        if (!encoded.empty()) {
            auto& term = Terminal::instance();
            term.move_cursor(x, y);
            term.write_raw(encoded);

            // Update LRU
            auto list_it = std::find(lru_list_.begin(), lru_list_.end(), key);
            if (list_it != lru_list_.end()) {
                lru_list_.splice(lru_list_.begin(), lru_list_, list_it);
            }
            return out_id;
        }
    }
    
    return 0;
}

bool ImageRenderer::preload_image(
    const std::vector<unsigned char>& image_data,
    int width_cols, int height_rows,
    const std::string& content_hash
) {
    if (!album_art_enabled_ || image_data.empty()) return false;

    // 1. Calculate Hash
    size_t data_hash;
    if (!content_hash.empty()) {
        data_hash = std::stoull(content_hash.substr(0, 16), nullptr, 16);
    } else {
        data_hash = util::ArtworkHasher::fast_hash(image_data);
    }

    // 1b. Check Negative Cache
    if (failed_hashes_.count(data_hash)) return false;

    CacheKey key{data_hash, width_cols, height_rows};

    // 2. Poll Pending Jobs (Maintenance)
    poll_jobs();

    // 3. Check Cache - if exists, we are good
    if (cache_.count(key)) return true;

    // 4. Check Pending - if exists, we are waiting
    if (pending_jobs_.count(key)) return false;

    // 5. Start Background Job (Reuse logic via internal helper? Or Copy/Paste?)
    // Copy/Paste is safer to avoid refactoring render_image right now, but risky for drift.
    // The submission logic is identical.
    
    // ... Duplicated submission logic ...
    // NOTE: In a real refactor, I'd extract `submit_decode_job` private method.
    // For now, I will inline it to keep changes localized.
    
    std::vector<unsigned char> data_copy = image_data;
    int target_w = width_cols * cell_width_;
    int target_h = height_rows * cell_height_;

    auto promise = std::make_shared<std::promise<CachedPixels>>();
    std::future<CachedPixels> future = promise->get_future();

    auto& pool = ouroboros::util::ImageDecoderPool::instance();
    bool submitted = pool.submit_job([data_copy, target_w, target_h, promise]() {
        int w, h, channels;
        unsigned char* pixels = stbi_load_from_memory(
            data_copy.data(), static_cast<int>(data_copy.size()),
            &w, &h, &channels, 3
        );

        if (!pixels) {
            promise->set_value(CachedPixels{{}, 0, 0, 0, false});
            return;
        }

        std::vector<unsigned char> output(target_w * target_h * 3);
        stbir_resize(pixels, w, h, 0, output.data(), target_w, target_h, 0,
                     STBIR_RGB, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_CATMULLROM);

        stbi_image_free(pixels);
        promise->set_value(CachedPixels{std::move(output), target_w, target_h, 0, true});
    });

    if (submitted) {
        pending_jobs_[key] = std::move(future);
    }

    return false;
}

void ImageRenderer::delete_image(const std::string& content_hash) {
    if (protocol_ != ImageProtocol::Kitty || content_hash.empty()) return;

    size_t data_hash = std::stoull(content_hash.substr(0, 16), nullptr, 16);
    uint32_t image_id = static_cast<uint32_t>(data_hash & 0xFFFFFFFF);

    delete_image_by_id(image_id);
}

void ImageRenderer::delete_image_by_id(uint32_t image_id) {
    if (protocol_ != ImageProtocol::Kitty || image_id == 0) return;

    // Remove from local tracking so we re-transmit if needed later
    transmitted_ids_.erase(image_id);

    auto& term = Terminal::instance();
    // Kitty delete by ID
    std::string cmd = "\033_Ga=d,d=i,i=" + std::to_string(image_id) + "\033\\";
    term.write_raw(cmd);
}

void ImageRenderer::clear_image(int x, int y, int width_cols, int height_rows) {
    ouroboros::util::Logger::debug("ImageRenderer: Clearing image");

    auto& term = Terminal::instance();

    // Protocol-specific clearing
    if (protocol_ == ImageProtocol::Kitty) {
        // Kitty: Delete all images (more reliable than selective delete)
        term.write_raw("\033_Ga=d\033\\");
        transmitted_ids_.clear(); // We wiped the terminal cache
    } else {
        // Sixel/iTerm2: Overwrite with spaces (current approach works)
        for (int row = 0; row < height_rows; ++row) {
            term.move_cursor(x, y + row);
            term.write_raw(std::string(width_cols, ' '));
        }
    }
}

std::string ImageRenderer::render_unicode_blocks(const unsigned char* /*rgba*/, int /*w*/, int /*h*/, int /*cols*/, int /*rows*/) {
    return ""; 
}

std::string ImageRenderer::rgb_to_ansi_truecolor(unsigned char, unsigned char, unsigned char, bool) {
    return ""; // Unused
}

std::string ImageRenderer::rgb_to_sixel(const unsigned char* rgb, int w, int h) {
    std::ostringstream ss;
    ss << "\033Pq"; 
    ss << "\"1;1"; 
    ss << "#0;2;0;0;0"; // Black
    ss << "#1;2;100;100;100"; // White
    
    for (int y = 0; y < h; y += 6) {
        ss << "#1";
        for (int x = 0; x < w; ++x) {
            unsigned char mask = 0;
            for (int dy = 0; dy < 6 && (y + dy) < h; ++dy) {
                int idx = ((y + dy) * w + x) * 3;
                int lum = (rgb[idx] * 299 + rgb[idx+1] * 587 + rgb[idx+2] * 114) / 1000;
                if (lum > 127) mask |= (1 << dy);
            }
            ss << (char)(63 + mask);
        }
        ss << "$-ի";
    }
    ss << "\033\\";
    return ss.str();
}

std::string ImageRenderer::render_sixel(const unsigned char* rgba, int w, int h, int cols, int rows) {
    int target_w = cols * cell_width_;
    int target_h = rows * cell_height_;
    // Fix: pass 4 channels
    auto resized = resize_image(rgba, w, h, target_w, target_h, 4);
    
    std::vector<unsigned char> rgb(target_w * target_h * 3);
    for (int i = 0; i < target_w * target_h; ++i) {
        rgb[i * 3 + 0] = resized[i * 4 + 0];
        rgb[i * 3 + 1] = resized[i * 4 + 1];
        rgb[i * 3 + 2] = resized[i * 4 + 2];
    }
    
    return rgb_to_sixel(rgb.data(), target_w, target_h);
}

// Helper to write data to a temporary file in /dev/shm (RAM)
// Returns the Base64-encoded path
std::string ImageRenderer::write_to_temp_file(const unsigned char* data, size_t len) {
    char template_path[] = "/dev/shm/ouroboros-art-XXXXXX";
    int fd = mkstemp(template_path);
    if (fd == -1) {
        ouroboros::util::Logger::error("ImageRenderer: Failed to create temp file");
        return "";
    }

    // Write data
    size_t written = 0;
    while (written < len) {
        ssize_t ret = write(fd, data + written, len - written);
        if (ret < 0) {
            ouroboros::util::Logger::error("ImageRenderer: Failed to write to temp file");
            close(fd);
            unlink(template_path);
            return "";
        }
        written += ret;
    }
    
    close(fd);
    
    // We do NOT unlink. Kitty 't=t' protocol will read and unlink it.
    
    // Base64 encode the PATH
    std::string path(template_path);
    return encode_base64(
        reinterpret_cast<const unsigned char*>(path.c_str()), 
        path.length()
    );
}

std::string ImageRenderer::render_kitty(const unsigned char* data, size_t len, int cols, int rows, size_t data_hash, const std::string& content_hash, uint32_t& out_id) {
    // Input is Resized RGB Data (3 bytes/pixel)

    int img_w = cols * cell_width_;
    int img_h = rows * cell_height_;

    // Check for TMUX environment
    static bool in_tmux = (std::getenv("TMUX") != nullptr);

    // Generate image ID based on terminal capabilities
    uint32_t image_id;
    if (terminal_respects_image_ids_) {
        // Standard behavior: content-based ID (allows caching/placement optimization)
        image_id = static_cast<uint32_t>(data_hash & 0xFFFFFFFF);
    } else {
        // Workaround for Ghostty Issue #6711: unique ID per position
        static uint32_t unique_id_counter = 1;
        image_id = unique_id_counter++;
    }
    
    out_id = image_id;

    std::ostringstream ss;

    if (in_tmux) {
        ss << "\033Ptmux;\033\033_G";
    } else {
        ss << "\033_G";
    }

    // Choose transmission mode based on terminal capabilities
    if (terminal_supports_temp_file_) {
        // Standard: use temp file for better performance (Kitty, WezTerm, etc.)
        std::string b64_path = write_to_temp_file(data, len);
        if (b64_path.empty()) {
            return ""; // Fail gracefully
        }
        ss << "a=T,t=t,f=24,i=" << image_id
           << ",s=" << img_w << ",v=" << img_h
           << ",c=" << cols << ",r=" << rows
           << ",q=1,z=1,C=1";
        ss << ";";
        ss << b64_path;

        std::string hash_info = content_hash.empty() ? "FNV-1a" : ("SHA-256: " + content_hash.substr(0, 8) + "...");
        ouroboros::util::Logger::debug("ImageRenderer: Uploaded via SHM (t=t), image_id=" +
                                       std::to_string(image_id) +
                                       " (" + hash_info + ")");
    } else {
        // Workaround: use direct transmission (t=d) for terminals with temp file bugs (Ghostty)
        std::string b64_data = encode_base64(data, len);
        ss << "a=T,t=d,f=24,i=" << image_id
           << ",s=" << img_w << ",v=" << img_h
           << ",c=" << cols << ",r=" << rows
           << ",q=1";
        ss << ";";
        ss << b64_data;

        std::string hash_info = content_hash.empty() ? "FNV-1a" : ("SHA-256: " + content_hash.substr(0, 8) + "...");
        ouroboros::util::Logger::debug("ImageRenderer: Uploaded via direct mode (t=d), image_id=" +
                                       std::to_string(image_id) +
                                       " (" + hash_info + ")");
    }

    if (in_tmux) {
        ss << "\033\033\\\033\\";
    } else {
        ss << "\033\\";
    }

    return ss.str();
}

std::string ImageRenderer::render_iterm2(const unsigned char* rgba, int w, int h, int cols, int rows) {
    int target_w = cols * cell_width_;
    int target_h = rows * cell_height_;
    // Fix: pass 4 channels
    auto resized = resize_image(rgba, w, h, target_w, target_h, 4);
    std::string b64 = encode_base64(resized.data(), resized.size());
    
    std::ostringstream ss;
    ss << "\033]1337;File=inline=1;width=" << cols << "cells;height=" << rows << "cells:" << b64 << "\007";
    return ss.str();
}

std::string ImageRenderer::encode_base64(const unsigned char* data, size_t len) {
    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    ret.reserve((len * 4) / 3 + 4);
    int i = 0;
    unsigned char char_array_3[3], char_array_4[4];
    
    while (len--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; i < 4; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for(int j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        for (int j = 0; j < i + 1; j++) ret += base64_chars[char_array_4[j]];
        while(i++ < 3) ret += '=';
    }
    return ret;
}

std::vector<unsigned char> ImageRenderer::resize_image(const unsigned char* pixels, int w, int h, int target_w, int target_h, int channels) {
    // Use Mitchell filter for smooth downscaling (default for downsample, prevents pixelation)
    // Mitchell-Netrevalli (B=1/3, C=1/3) is optimal for reducing aliasing artifacts
    std::vector<unsigned char> output(target_w * target_h * channels);
    stbir_resize(pixels, w, h, 0, output.data(), target_w, target_h, 0,
                 (channels == 4 ? STBIR_RGBA : STBIR_RGB),
                 STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_MITCHELL);
    return output;
}

std::string ImageRenderer::protocol_name(ImageProtocol proto) {
    switch (proto) {
        case ImageProtocol::Kitty: return "Kitty";
        case ImageProtocol::ITerm2: return "iTerm2";
        case ImageProtocol::Sixel: return "Sixel";
        case ImageProtocol::None: return "None";
        default: return "Unknown";
    }
}

bool ImageRenderer::has_pending_updates() {
    poll_jobs(); // Check for completions
    return has_updates_.load();
}

void ImageRenderer::poll_jobs() {
    auto job_it = pending_jobs_.begin();
    while (job_it != pending_jobs_.end()) {
        auto& future = job_it->second;
        if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            CachedPixels result = future.get();
            if (result.valid) {
                if (cache_.size() >= MAX_CACHE_SIZE) {
                    auto last = lru_list_.back();
                    cache_.erase(last);
                    lru_list_.pop_back();
                }
                lru_list_.push_front(job_it->first);
                cache_[job_it->first] = std::move(result);
                has_updates_.store(true);
                ouroboros::util::Logger::debug("ImageRenderer: Async job completed in poll_jobs");
            } else {
                 failed_hashes_.insert(job_it->first.data_hash);
            }
            job_it = pending_jobs_.erase(job_it);
        } else {
            ++job_it;
        }
    }
}

void ImageRenderer::clear_pending_updates() {
    has_updates_.store(false);
}

} // namespace ouroboros::ui
