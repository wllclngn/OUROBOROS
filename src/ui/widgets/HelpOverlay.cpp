#include "ui/widgets/HelpOverlay.hpp"
#include "config/UIConfig.hpp"

namespace ouroboros::ui::widgets {

HelpOverlay::HelpOverlay() {
    build_content();
}

void HelpOverlay::build_content() {
    using L = Line;

    auto heading    = [&](const std::string& t) { lines_.push_back({L::Heading,    t, ""}); };
    auto subheading = [&](const std::string& t) { lines_.push_back({L::SubHeading, t, ""}); };
    auto kv         = [&](const std::string& k, const std::string& v) { lines_.push_back({L::KeyValue, k, v}); };
    auto text       = [&](const std::string& t) { lines_.push_back({L::Text,       t, ""}); };
    auto blank      = [&]()                     { lines_.push_back({L::Blank,      "",  ""}); };
    auto divider    = [&]()                     { lines_.push_back({L::Divider,    "",  ""}); };

    heading("OUROBOROS MUSIC PLAYER");
    text("Offline, metadata-driven music player for modern Linux terminals.");
    text("Built in C++23. Lock-free snapshot architecture. Zero deadlocks.");
    blank();
    divider();

    // --- KEYBINDINGS ---
    heading("KEYBINDINGS");
    blank();
    subheading("Navigation");
    kv("j / k",            "Navigate down / up");
    kv("Up / Down",        "Arrow key navigation");
    kv("h / l",            "Navigate left / right (album grid)");
    kv("Left / Right",     "Arrow key navigation (album grid)");
    kv("Shift+J / Shift+K","Multi-select (select and navigate)");
    kv("Tab",              "Switch focus (Browser <-> Queue)");
    kv("g / G",            "Jump to top / bottom");
    blank();

    subheading("Playback");
    kv("Space",            "Play / Pause");
    kv("n",                "Next track");
    kv("p",                "Previous track");
    kv("Left / Right",     "Seek backward / forward (+/-5s)");
    kv("r",                "Cycle repeat mode (Off -> One -> All)");
    blank();

    subheading("Volume");
    kv("+ / =",            "Volume up (+5%)");
    kv("- / _",            "Volume down (-5%)");
    blank();

    subheading("Library & Queue");
    kv("Enter",            "Add selected track(s) / album to queue");
    kv("Ctrl+f",           "Toggle search / find");
    kv("Ctrl+d",           "Clear the queue");
    kv("Ctrl+a",           "Toggle album grid view");
    blank();

    subheading("Application");
    kv("?",                "Toggle this help view");
    kv("q",                "Quit");
    kv("Ctrl+C",           "Force quit");
    blank();
    divider();

    // --- CONFIGURATION ---
    heading("CONFIGURATION");
    blank();
    text("Config file: ~/.config/ouroboros/config.toml");
    blank();

    subheading("[library]");
    kv("music_directories", "Array of music directory paths");
    blank();

    subheading("[playback]");
    kv("default_volume",   "Volume on startup (0-100). Default: 50");
    kv("shuffle",          "Enable shuffle mode. Default: false");
    kv("repeat",           "\"off\", \"one\", \"all\". Default: \"all\"");
    blank();

    subheading("[ui]");
    kv("layout",           "\"default\", \"queue\", \"browser\". Default: \"default\"");
    kv("theme",            "\"dark\", \"light\", \"monokai\", \"terminal\"");
    kv("enable_album_art", "Enable artwork display. Default: true");
    blank();

    subheading("[performance]");
    kv("artwork_max_workers",     "Decoder threads (0 = auto). Default: 0");
    kv("artwork_prefetch_items",  "Items to prefetch. Default: 100");
    kv("artwork_spawn_threshold", "Queue depth to spawn workers. Default: 10");
    kv("artwork_memory_limit_mb", "Artwork memory limit (MB). Default: 3072");
    blank();
    divider();

    // --- ENVIRONMENT ---
    heading("ENVIRONMENT VARIABLES");
    blank();
    kv("OUROBOROS_IMAGE_PROTOCOL",   "Force image protocol: kitty, sixel, iterm2, none");
    kv("OUROBOROS_GHOSTTY_USE_SHM",  "Enable /dev/shm for Ghostty (experimental). Set to 1");
    kv("XDG_CONFIG_HOME",           "Config base dir. Default: ~/.config");
    kv("XDG_CACHE_HOME",            "Cache base dir. Default: ~/.cache");
    kv("XDG_MUSIC_DIR",             "Default music directory. Default: ~/Music");
    blank();
    divider();

    // --- TERMINAL SUPPORT ---
    heading("TERMINAL SUPPORT");
    blank();
    kv("Kitty",            "Best. Full graphics protocol + shared memory.");
    kv("WezTerm",          "Excellent. Kitty protocol + Sixel fallback. GPU-accelerated.");
    kv("Konsole",          "Good. Kitty protocol in 23.08+.");
    kv("Ghostty",          "Acceptable. Kitty w/ quirks. Try GHOSTTY_USE_SHM=1.");
    kv("foot",             "Fast Wayland-native. Sixel protocol.");
    kv("xterm / mlterm",   "Basic Sixel. Slower rendering.");
    kv("Alacritty",        "No image protocol. Text-only mode.");
    blank();
    divider();

    // --- FILES ---
    heading("FILES");
    blank();
    kv("~/.config/ouroboros/config.toml", "User configuration (TOML)");
    kv("~/.cache/ouroboros/library.bin",  "Library metadata cache");
    kv("~/.cache/ouroboros/dirs/",        "Per-directory metadata caches");
    kv("~/.cache/ouroboros/artwork.cache", "Content-addressed artwork storage");
    kv("/tmp/ouroboros_debug.log",        "Debug log file");
    blank();
    divider();

    // --- TROUBLESHOOTING ---
    heading("TROUBLESHOOTING");
    blank();
    subheading("Album Art Not Displaying");
    text("1. Check terminal support (Kitty/WezTerm/Konsole recommended)");
    text("2. Verify enable_album_art = true in config.toml");
    text("3. Check for embedded artwork or sidecar files (cover.jpg, folder.png)");
    text("4. View logs: grep -i artwork /tmp/ouroboros_debug.log");
    blank();

    subheading("Terminal Doesn't Restore After Crash");
    text("Run `reset` or close/reopen terminal.");
    blank();

    subheading("Build Fails");
    text("1. Check compiler: g++ --version (need GCC 13+ for C++23)");
    text("2. Verify dependencies installed");
    text("3. Clean build: make distclean && cmake -B build");
    text("4. Delete build/CMakeCache.txt if switching compilers");
    blank();

    subheading("Playback Issues");
    text("1. Ensure PipeWire running: systemctl --user status pipewire");
    text("2. Check audio sink: pactl list sinks short");
    text("3. Verify file format: file <audio_file>");
    text("4. Check logs: grep -i playback /tmp/ouroboros_debug.log");
    blank();
    divider();

    // --- AUDIO FORMATS ---
    heading("SUPPORTED FORMATS");
    blank();
    kv("MP3",              "libmpg123");
    kv("FLAC / WAV",       "libsndfile");
    kv("OGG / Vorbis",     "libvorbisfile");
    kv("M4A / AAC",        "FFmpeg (libavformat/libavcodec)");
    kv("DSD / DSF",        "Native DSD-over-PCM decimation");
    blank();
    divider();

    // --- ARCHITECTURE ---
    heading("ARCHITECTURE");
    blank();
    text("Lock-free snapshot system with atomic reads. UI never blocks.");
    text("Double-buffered state: front (atomic, lock-free) + back (mutex).");
    text("Kernel-level getdents64 for 2-3x faster directory scanning.");
    text("Multi-tier cache: O(1) tree hash -> O(dirs) mtime -> O(files) parse.");
    text("Content-addressed artwork via SHA-256 + FNV-1a dual-hash.");
    text("30 FPS rendering with sub-millisecond snapshot reads.");
    blank();

    subheading("Threading Model");
    kv("Main Thread",       "UI rendering (30 FPS), input handling");
    kv("LibraryCollector",  "Background scanning, metadata extraction");
    kv("PlaybackCollector", "Audio decoding, PipeWire output");
    kv("ArtworkLoader",     "Async image decoding with viewport prefetch");
    kv("ImageDecoderPool",  "Hardware-aware thread pool (4-16 threads)");
    blank();

    subheading("Algorithms");
    kv("TimSort",           "Adaptive merge sort. O(n) best, O(n log n) worst.");
    kv("Boyer-Moore",       "Sublinear search O(n/m). <5ms per keystroke.");
    kv("SHA-256",           "FIPS 180-4. Content-addressed artwork storage.");
    kv("FNV-1a",            "Fast hash for lookups. Adaptive sampling >65KB.");
    blank();
    divider();
    blank();
    text("Press ? or Escape to close. Scroll with j/k or arrow keys.");
}

void HelpOverlay::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    (void)snap;
    if (!visible_) return;

    const auto& uc = config::ui_config();

    // Draw border with title
    auto inner = draw_box_border(canvas, rect, "HELP  (? or ESC to close)", true);

    int available = inner.height;
    if (available < 1) return;

    int total = static_cast<int>(lines_.size());

    // Clamp scroll
    int max_scroll = std::max(0, total - available);
    if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll;
    if (scroll_offset_ < 0) scroll_offset_ = 0;

    // Render visible lines
    int end = std::min(total, scroll_offset_ + available);
    int y = inner.y;
    int max_w = inner.width - 2; // padding from left edge

    for (int i = scroll_offset_; i < end; ++i, ++y) {
        const auto& line = lines_[i];
        int x = inner.x + 1;

        switch (line.type) {
        case Line::Blank:
            break;

        case Line::Heading:
            canvas.draw_text(x, y, line.left, uc.heading);
            break;

        case Line::SubHeading:
            canvas.draw_text(x + 1, y, line.left, uc.accent);
            break;

        case Line::KeyValue: {
            int key_width = 28;
            std::string key = line.left;
            if (static_cast<int>(key.size()) > key_width)
                key = key.substr(0, key_width);

            // Draw key in bold/artist style, value in normal text
            int after = canvas.draw_text(x + 2, y, key, Style{uc.artist.fg, Color::Default, Attribute::Bold});
            int pad_to = x + 2 + key_width + 2;
            if (after < pad_to) after = pad_to;

            // Truncate value to fit
            std::string val = line.right;
            int val_max = std::max(0, (inner.x + inner.width - 1) - after);
            if (static_cast<int>(val.size()) > val_max && val_max > 3)
                val = val.substr(0, val_max - 3) + "...";
            else if (static_cast<int>(val.size()) > val_max)
                val = val.substr(0, val_max);

            canvas.draw_text(after, y, val, uc.title);
            break;
        }

        case Line::Text:
            canvas.draw_text(x + 2, y, truncate_text(line.left, max_w - 2), uc.title);
            break;

        case Line::Divider:
            for (int dx = 0; dx < inner.width; ++dx)
                canvas.draw_text(inner.x + dx, y, "\xe2\x94\x80", uc.muted); // ─
            break;
        }
    }

    // Scroll indicator
    if (total > available) {
        int pct = (total > 1) ? (scroll_offset_ * 100) / max_scroll : 0;
        std::string indicator = std::to_string(pct) + "%";
        canvas.draw_text(inner.x + inner.width - static_cast<int>(indicator.size()) - 1,
                        rect.y, " " + indicator + " ", uc.muted);
    }
}

void HelpOverlay::handle_input(const InputEvent& event) {
    // Scrolling
    if (event.key_name == "j" || event.key_name == "down") {
        scroll_offset_++;
    } else if (event.key_name == "k" || event.key_name == "up") {
        scroll_offset_--;
    } else if (event.key_name == "d" || event.key_name == "page_down") {
        scroll_offset_ += 20;
    } else if (event.key_name == "u" || event.key_name == "page_up") {
        scroll_offset_ -= 20;
    } else if (event.key_name == "g" || event.key_name == "home") {
        scroll_offset_ = 0;
    } else if (event.key_name == "G" || event.key_name == "end") {
        scroll_offset_ = static_cast<int>(lines_.size()); // clamped in render
    }
}

SizeConstraints HelpOverlay::get_constraints() const {
    return SizeConstraints{};
}

}  // namespace ouroboros::ui::widgets
