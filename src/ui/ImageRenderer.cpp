#include "ui/ImageRenderer.hpp"
#include "ui/Terminal.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <vector>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstdlib>
#include <termios.h>
#include <fcntl.h>

// stb_image_resize2 for Sixel/iTerm2 resizing
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

#include "util/Logger.hpp"

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
            terminal_respects_image_ids_ = false;
            terminal_supports_temp_file_ = false;
            ouroboros::util::Logger::info("ImageRenderer: Detected Ghostty terminal");
            return;
        }
    }

    // 2. Check Kitty-specific environment variables
    if (std::getenv("KITTY_WINDOW_ID") || std::getenv("KITTY_PID")) {
        terminal_type_ = TerminalType::Kitty;
        terminal_respects_image_ids_ = true;
        terminal_supports_temp_file_ = true;
        ouroboros::util::Logger::info("ImageRenderer: Detected Kitty terminal");
        return;
    }

    // 3. Check TERM variable
    const char* term_env = std::getenv("TERM");
    if (term_env) {
        std::string term_str(term_env);

        if (term_str.find("ghostty") != std::string::npos || term_str == "xterm-ghostty") {
            terminal_type_ = TerminalType::Ghostty;
            terminal_respects_image_ids_ = false;
            terminal_supports_temp_file_ = false;
            ouroboros::util::Logger::info("ImageRenderer: Detected Ghostty terminal via TERM");
            return;
        }

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
    terminal_respects_image_ids_ = true;
    terminal_supports_temp_file_ = true;
    ouroboros::util::Logger::debug("ImageRenderer: Unknown terminal, assuming standard behavior");
}

bool ImageRenderer::query_kitty_support() {
    ouroboros::util::Logger::debug("ImageRenderer: Querying Kitty support");

    if (std::getenv("KITTY_WINDOW_ID") || std::getenv("KITTY_PID")) {
        return true;
    }

    const char* term_env = std::getenv("TERM");
    if (term_env) {
        std::string term_str(term_env);

        if (term_str.find("ghostty") != std::string::npos || term_str == "xterm-ghostty") {
            ouroboros::util::Logger::info("ImageRenderer: Ghostty terminal detected, using Kitty graphics protocol");
            return true;
        }

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
    if (protocol_ == ImageProtocol::Kitty) {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_xpixel > 0 && w.ws_ypixel > 0) {
            cell_width_ = w.ws_xpixel / w.ws_col;
            cell_height_ = w.ws_ypixel / w.ws_row;
            ouroboros::util::Logger::info("ImageRenderer: Detected Kitty cell size: " +
                                          std::to_string(cell_width_) + "×" +
                                          std::to_string(cell_height_) + " pixels");
            return;
        }
    }

    cell_width_ = 8;
    cell_height_ = 16;
    ouroboros::util::Logger::info("ImageRenderer: Using default cell size: 8×16 pixels");
}

uint32_t ImageRenderer::render_image(
    const uint8_t* data,
    size_t data_size,
    int data_width,
    int data_height,
    CachedFormat format,
    int x, int y,
    int width_cols, int height_rows,
    const std::string& content_hash,
    int visible_rows
) {
    if (!album_art_enabled_ || data == nullptr || data_size == 0) return 0;

    // Calculate image_id from content hash
    size_t data_hash = 0;
    if (!content_hash.empty() && content_hash.size() >= 16) {
        try {
            data_hash = std::stoull(content_hash.substr(0, 16), nullptr, 16);
        } catch (const std::exception&) {
            // Fallback: use data pointer as hash (not ideal but works)
            data_hash = reinterpret_cast<size_t>(data);
        }
    } else {
        data_hash = reinterpret_cast<size_t>(data);
    }

    // Calculate expected image_id for position deduplication
    uint32_t expected_id = 0;
    if (protocol_ == ImageProtocol::Kitty && terminal_respects_image_ids_ && !content_hash.empty()) {
        expected_id = static_cast<uint32_t>(data_hash & 0xFFFFFFFF);

        // Check if this image is already displayed at this position
        uint32_t pos_key = (static_cast<uint32_t>(x) << 16) | static_cast<uint32_t>(y & 0xFFFF);
        auto pos_it = displayed_at_position_.find(pos_key);
        if (pos_it != displayed_at_position_.end() && pos_it->second == expected_id) {
            return expected_id;
        }
    }

    std::string encoded;
    uint32_t out_id = 0;

    // Determine render dimensions (handling partial visibility)
    int render_rows = height_rows;
    size_t render_size = data_size;

    if (visible_rows > 0 && visible_rows < height_rows) {
        render_rows = visible_rows;  // Applies to ALL formats (Kitty uses r= parameter)

        // Only crop raw pixel data for RGB (PNG is already encoded)
        if (format == CachedFormat::RGB) {
            int crop_h = visible_rows * cell_height_;
            if (crop_h > data_height) crop_h = data_height;
            render_size = data_width * crop_h * 3;
        }
    }

    if (protocol_ == ImageProtocol::Kitty) {
        encoded = render_kitty(data, render_size, width_cols, render_rows, data_hash, content_hash, out_id, format);
    } else if (protocol_ == ImageProtocol::ITerm2) {
        encoded = render_iterm2(data, data_width, data_height, width_cols, render_rows);
    } else if (protocol_ == ImageProtocol::Sixel) {
        encoded = render_sixel(data, data_width, data_height, width_cols, render_rows);
    }

    if (!encoded.empty()) {
        auto& term = Terminal::instance();
        term.move_cursor(x, y);
        term.write_raw(encoded);

        // Track what's displayed at this position for future deduplication
        if (protocol_ == ImageProtocol::Kitty && out_id != 0) {
            uint32_t pos_key = (static_cast<uint32_t>(x) << 16) | static_cast<uint32_t>(y & 0xFFFF);
            displayed_at_position_[pos_key] = out_id;
        }

        return out_id;
    }

    return 0;
}

void ImageRenderer::delete_image(const std::string& content_hash) {
    if (protocol_ != ImageProtocol::Kitty || content_hash.empty()) return;
    if (content_hash.size() < 16) return;

    size_t data_hash = 0;
    try {
        data_hash = std::stoull(content_hash.substr(0, 16), nullptr, 16);
    } catch (const std::exception&) {
        return;
    }

    uint32_t image_id = static_cast<uint32_t>(data_hash & 0xFFFFFFFF);
    delete_image_by_id(image_id);
}

void ImageRenderer::delete_image_by_id(uint32_t image_id) {
    if (protocol_ != ImageProtocol::Kitty || image_id == 0) return;

    transmitted_ids_.erase(image_id);

    for (auto it = displayed_at_position_.begin(); it != displayed_at_position_.end(); ) {
        if (it->second == image_id) {
            it = displayed_at_position_.erase(it);
        } else {
            ++it;
        }
    }

    auto& term = Terminal::instance();
    std::string cmd = "\033_Ga=d,d=i,i=" + std::to_string(image_id) + "\033\\";
    term.write_raw(cmd);
}

void ImageRenderer::clear_image(int x, int y, int width_cols, int height_rows) {
    ouroboros::util::Logger::debug("ImageRenderer: Clearing image");

    auto& term = Terminal::instance();

    if (protocol_ == ImageProtocol::Kitty) {
        term.write_raw("\033_Ga=d\033\\");
        transmitted_ids_.clear();
        displayed_at_position_.clear();
    } else {
        for (int row = 0; row < height_rows; ++row) {
            term.move_cursor(x, y + row);
            term.write_raw(std::string(width_cols, ' '));
        }
    }
}

std::string ImageRenderer::rgb_to_sixel(const unsigned char* rgb, int w, int h) {
    std::ostringstream ss;
    ss << "\033Pq";
    ss << "\"1;1";
    ss << "#0;2;0;0;0";
    ss << "#1;2;100;100;100";

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
        ss << "$-";
    }
    ss << "\033\\";
    return ss.str();
}

std::string ImageRenderer::render_sixel(const unsigned char* data, int w, int h, int cols, int rows) {
    int target_w = cols * cell_width_;
    int target_h = rows * cell_height_;

    // Resize if needed
    std::vector<unsigned char> resized;
    const unsigned char* rgb_data = data;

    if (w != target_w || h != target_h) {
        resized = resize_image(data, w, h, target_w, target_h, 3);
        rgb_data = resized.data();
    }

    return rgb_to_sixel(rgb_data, target_w, target_h);
}

std::string ImageRenderer::write_to_temp_file(const unsigned char* data, size_t len) {
    // Track temp files with creation time for delayed cleanup
    // Kitty should delete t=t files, but we clean up any stragglers older than 500ms
    static std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> pending_files;

    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::milliseconds(500);

    // Remove files older than 500ms (Kitty has definitely read them by now)
    auto it = pending_files.begin();
    while (it != pending_files.end()) {
        if (it->second < cutoff) {
            unlink(it->first.c_str());
            it = pending_files.erase(it);
        } else {
            ++it;
        }
    }

    char template_path[] = "/dev/shm/ouroboros-art-XXXXXX";
    int fd = mkstemp(template_path);
    if (fd == -1) {
        ouroboros::util::Logger::error("ImageRenderer: Failed to create temp file");
        return "";
    }

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

    // Track for delayed cleanup (in case Kitty doesn't delete it)
    pending_files.emplace_back(template_path, std::chrono::steady_clock::now());

    std::string path(template_path);
    return encode_base64(
        reinterpret_cast<const unsigned char*>(path.c_str()),
        path.length()
    );
}

std::string ImageRenderer::render_kitty(const unsigned char* data, size_t len, int cols, int rows, size_t data_hash, const std::string& content_hash, uint32_t& out_id, CachedFormat format) {
    int img_w = cols * cell_width_;
    int img_h = rows * cell_height_;

    static bool in_tmux = (std::getenv("TMUX") != nullptr);

    uint32_t image_id;
    if (terminal_respects_image_ids_) {
        image_id = static_cast<uint32_t>(data_hash & 0xFFFFFFFF);
    } else {
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

    if (terminal_supports_temp_file_) {
        std::string b64_path = write_to_temp_file(data, len);
        if (b64_path.empty()) {
            return "";
        }

        if (format == CachedFormat::PNG) {
            ss << "a=T,t=t,f=100,i=" << image_id
               << ",c=" << cols << ",r=" << rows
               << ",q=1,z=1,C=1";
        } else {
            ss << "a=T,t=t,f=24,i=" << image_id
               << ",s=" << img_w << ",v=" << img_h
               << ",c=" << cols << ",r=" << rows
               << ",q=1,z=1,C=1";
        }
        ss << ";";
        ss << b64_path;

        std::string format_str = (format == CachedFormat::PNG) ? "PNG" : "RGB";
        std::string hash_info = content_hash.empty() ? "ptr-hash" : ("SHA-256: " + content_hash.substr(0, 8) + "...");
        ouroboros::util::Logger::debug("ImageRenderer: Uploaded via SHM (t=t), format=" + format_str +
                                       ", image_id=" + std::to_string(image_id) +
                                       " (" + hash_info + ")");
    } else {
        std::string b64_data = encode_base64(data, len);

        if (format == CachedFormat::PNG) {
            ss << "a=T,t=d,f=100,i=" << image_id
               << ",c=" << cols << ",r=" << rows
               << ",q=1";
        } else {
            ss << "a=T,t=d,f=24,i=" << image_id
               << ",s=" << img_w << ",v=" << img_h
               << ",c=" << cols << ",r=" << rows
               << ",q=1";
        }
        ss << ";";
        ss << b64_data;

        std::string format_str = (format == CachedFormat::PNG) ? "PNG" : "RGB";
        std::string hash_info = content_hash.empty() ? "ptr-hash" : ("SHA-256: " + content_hash.substr(0, 8) + "...");
        ouroboros::util::Logger::debug("ImageRenderer: Uploaded via direct mode (t=d), format=" + format_str +
                                       ", image_id=" + std::to_string(image_id) +
                                       " (" + hash_info + ")");
    }

    if (in_tmux) {
        ss << "\033\033\\\033\\";
    } else {
        ss << "\033\\";
    }

    return ss.str();
}

std::string ImageRenderer::render_iterm2(const unsigned char* data, int w, int h, int cols, int rows) {
    int target_w = cols * cell_width_;
    int target_h = rows * cell_height_;

    std::vector<unsigned char> resized;
    const unsigned char* rgb_data = data;

    if (w != target_w || h != target_h) {
        resized = resize_image(data, w, h, target_w, target_h, 3);
        rgb_data = resized.data();
    }

    std::string b64 = encode_base64(rgb_data, target_w * target_h * 3);

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

} // namespace ouroboros::ui
