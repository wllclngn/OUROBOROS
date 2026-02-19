# OUROBOROS Architecture & Technical Documentation

This document provides a comprehensive technical deep-dive into OUROBOROS's architecture, algorithms, and implementation details. For user-facing documentation, see [README.md](README.md).

---

## Table of Contents

1. [Concurrency Architecture](#concurrency-architecture)
2. [Audio Pipeline](#audio-pipeline)
3. [Library Management](#library-management)
4. [Advanced Algorithms](#advanced-algorithms)
5. [Kernel-Level Optimizations](#kernel-level-optimizations)
6. [Artwork System](#artwork-system)
7. [5-Phase Rendering Pipeline](#5-phase-rendering-pipeline)
8. [Smart Scroll Optimization](#smart-scroll-optimization)
9. [Atomic Slot System](#atomic-slot-system)
10. [ArtworkWindow Coordinator](#artworkwindow-coordinator)
11. [Scattered Album Detection & Merging](#scattered-album-detection--merging)
12. [Unicode Support](#unicode-support)
13. [Performance Characteristics](#performance-characteristics)
14. [Security](#security)
15. [Code Quality & Engineering Patterns](#code-quality--engineering-patterns)
16. [Project Structure](#project-structure)

---

## Concurrency Architecture

### Lock-Free Snapshot System

**File**: `src/backend/SnapshotBuffers.cpp`

OUROBOROS achieves **zero deadlocks** through an immutable snapshot architecture using atomic double-buffering.

#### Ping-Pong Double-Buffer Pattern

Two buffers (`a_` and `b_`) swap roles via an atomic pointer:

- **Front buffer**: Atomically readable by UI thread (lock-free)
- **Back buffer**: Writeable by collector threads (mutex-protected via `SnapshotPublisher`)

```cpp
model::Snapshot a_;
model::Snapshot b_;
std::atomic<model::Snapshot*> front_;  // Points to a_ or b_
model::Snapshot* back_;                // Points to the other
```

#### Atomic Read Path (Lock-Free)

```cpp
const model::Snapshot& front() const {
    return *front_.load(std::memory_order_acquire);  // No mutex!
}
```

**Key Insight**: UI thread never blocks on mutex acquisition. Reads are instantaneous (<1us).

#### Write Path (Serialized)

```cpp
// SnapshotPublisher::update() — mutex-protected
void update(std::function<void(Snapshot&)> updater) {
    std::lock_guard lock(mutex_);
    auto& back = buffers_.back();
    updater(back);
    buffers_.publish();  // Atomic pointer swap + re-sync
}
```

#### Publish: Swap + Re-Sync

```cpp
void publish() {
    back_->seq = front_.load(std::memory_order_acquire)->seq + 1;
    auto* old_front = front_.load(std::memory_order_relaxed);
    front_.store(back_, std::memory_order_release);  // Atomic swap
    back_ = old_front;
    *back_ = *front_.load(std::memory_order_acquire);  // Re-sync for next update
}
```

#### Guarantees

- UI thread **never blocks** on mutex
- Writes are serialized (only one collector modifies at a time via `SnapshotPublisher` mutex)
- Sequence counter detects stale reads
- Post-swap re-sync ensures back buffer starts with latest state

### Threading Model

OUROBOROS runs 4+ threads concurrently:

1. **Main Thread**: UI rendering (30 FPS) + input handling
   - Lock-free reads via `publisher->get_current()` (atomic pointer load)
   - Never blocks on mutexes during render

2. **LibraryCollector**: Background library scanning
   - Metadata parsing with parallel thread pool
   - Artwork extraction
   - Cache management

3. **PlaybackCollector**: Audio decode thread
   - Pushes decoded PCM into SPSC ring buffer
   - Wall-clock position interpolation
   - Gapless track transitions

4. **ArtworkLoader**: Async image decoding
   - Sliding window prefetch (20 items ahead/behind)
   - Priority queue scheduling

5. **ImageDecoderPool Workers**: N threads (hardware_concurrency)
   - Parallel JPEG/PNG decoding
   - Image resizing

### Snapshot Pattern Details

**Immutable State**: Each `Snapshot` is a point-in-time view containing:
- `PlayerState` (playback position, volume, repeat mode)
- `LibraryState` (tracks, albums, artists)
- `QueueState` (Two Stacks: history/current/future for deterministic Previous)
- `UIState` (focus, search query, viewport)

**Double Buffering**: Front buffer for reads, back buffer for writes, atomic pointer swap to publish

**Copy-On-Write**: `shared_ptr` for LibraryState/QueueState (cheap snapshot copies)

**Sequence Numbers**: Change detection via monotonic `seq` counter

### Thread Pool Architecture

**File**: `include/util/ImageDecoderPool.hpp`

Fixed thread pool for async image decoding:

**Design:**
- Worker count = `std::thread::hardware_concurrency()` (typically 4-16)
- Job queue with MAX_SIZE=50 (prevents memory explosion during fast scrolling)
- Condition variable for thread wake-up
- Returns `std::future<CachedPixels>` for completion tracking

**Main Loop Integration:**
```cpp
// Submit job
auto future = decoder_pool.submit(resize_job);

// Non-blocking poll (main loop)
if (future.wait_for(0s) == std::future_status::ready) {
    auto pixels = future.get();  // Image ready!
    trigger_ui_rerender();
}
```

**Performance**: Decoding 32 album covers in parallel (~200ms) vs sequentially (~1.2s).

---

## Audio Pipeline

### Architecture Overview

```
File -> Format Detection -> Decoder (MP3/FLAC/OGG/M4A)
                                |
                           PCM Float32
                                |
                        [SPSC Ring Buffer]   (8192 frames, power-of-2, lock-free)
                                |
                     producer: decode thread writes
                     consumer: PipeWire RT callback pulls
                                |
                           PipeWire -> Speakers
```

### Callback-Driven Pull Model

**File**: `src/audio/PipeWireOutput.cpp`

PipeWire calls `on_process_callback` on its real-time thread. The callback **pulls** audio from the ring buffer -- the decode thread never touches PipeWire directly.

```cpp
// PipeWire RT callback: no allocations, no logging, no locks.
void on_process_callback(void* userdata) {
    auto* output = static_cast<PipeWireOutput*>(userdata);
    struct pw_buffer* pw_buf = pw_stream_dequeue_buffer(output->stream_);
    if (!pw_buf) return;

    // Honor PipeWire's quantum request
    size_t max_frames = buf->datas[0].maxsize / bytes_per_frame;
    if (pw_buf->requested > 0)
        max_frames = std::min(max_frames, static_cast<size_t>(pw_buf->requested));

    // Pull from ring buffer
    size_t frames_read = output->ring_.read(dst, max_frames);

    // Volume scaling in-place
    // Underrun: fill remainder with silence

    pw_buf->size = frames_read;
    pw_stream_queue_buffer(output->stream_, pw_buf);
}
```

### SPSC Ring Buffer

**File**: `include/audio/AudioRingBuffer.hpp`

Lock-free single-producer, single-consumer ring buffer decoupling the decode thread from PipeWire's real-time callback.

**Design:**
- **Capacity**: 8192 frames (~42ms @ 192kHz, ~186ms @ 44.1kHz)
- **Power-of-2 masking**: Bitwise `& mask_` for wrap-around (no modulo)
- **Virtual indices**: `write_pos_` and `read_pos_` monotonically increase; wrap via mask
- **Cache-line isolation**: `alignas(64)` on atomics to prevent false sharing
- **Memory ordering**: `release`/`acquire` pairs for producer-consumer synchronization

```cpp
alignas(64) std::atomic<size_t> write_pos_{0};   // Producer: decode thread
alignas(64) std::atomic<size_t> read_pos_{0};     // Consumer: PipeWire RT thread
alignas(64) std::atomic<size_t> total_consumed_frames_{0};  // Position tracking
```

**Wrap-aware copy** handles split reads/writes across the buffer boundary with two `memcpy` segments.

### Format Detection

1. **Extension-based**: Check file extension (`.mp3`, `.flac`, `.ogg`, `.wav`, `.m4a`)
2. **Magic Bytes**: Verify file headers to prevent misclassification
3. **Decoder Selection**: Polymorphic `AudioDecoder` base class

### Decoder Implementations

**MP3Decoder** (`src/audio/MP3Decoder.cpp`):
- Uses libmpg123
- Supports ID3v1/ID3v2 tag parsing
- Variable bitrate (VBR) support
- Accurate seeking via frame positioning

**FLACDecoder** (`src/audio/FLACDecoder.cpp`):
- Uses libsndfile
- Lossless compression (also handles WAV via same decoder)
- Vorbis comment metadata
- Native 16/24-bit depth support

**OGGDecoder** (`src/audio/OGGDecoder.cpp`):
- Uses libvorbisfile
- Lossy compression (superior to MP3 at same bitrate)
- Vorbis comments for metadata

**M4ADecoder** (`src/audio/M4ADecoder.cpp`):
- Uses FFmpeg (libavformat, libavcodec, libswresample)
- AAC/ALAC support in MP4 container
- iTunes-style metadata extraction
- Accurate seeking via FFmpeg's timestamp API

### PipeWire Integration

**Native PipeWire Output** (`src/audio/PipeWireOutput.cpp`):
- **Modern Linux Audio**: No ALSA/PulseAudio fallbacks
- **Per-Track Format Negotiation**: Dynamically reconfigures stream for each track's sample rate/channels
- **Shared Context Pattern**: Single `PipeWireContext` with thread-loop shared across streams
- **Float32 Pipeline**: Native F32 audio processing through SPSC ring buffer (8192 frames)
- **Thread Safety**: PipeWire operations protected by `pw_thread_loop_lock/unlock`; RT callback is lock-free

### Gapless Playback

**File**: `src/collectors/PlaybackCollector.cpp`

True gapless transitions between tracks of the same format:

1. **Persistent output**: `PipeWireOutput` lives across tracks, not recreated per-track
2. **No drain wait**: When track N finishes decoding, PlaybackCollector immediately opens track N+1's decoder. The ring buffer carries track N's tail samples while N+1 begins filling.
3. **Format-change detection**: Output is only reinitialized when sample rate or channel count changes
4. **PipeWire continuity**: The RT callback pulls continuously from the ring buffer, unaware of track boundaries

```cpp
// GAPLESS: Only reinit output if format changed
bool format_changed = (actual_sample_rate != output_sample_rate ||
                       actual_channels != output_channels);

if (format_changed) {
    output.close();
    output.init(audio_context_, actual_sample_rate, actual_channels);
} else {
    // Reusing PipeWire stream (gapless transition)
}
```

### Wall-Clock Position Interpolation

**File**: `src/collectors/PlaybackCollector.cpp`

Position tracking uses `steady_clock` anchoring rather than polling the decoder's frame position. This provides smooth, jitter-free position display at 30Hz.

**Anchor state:**
```cpp
std::chrono::steady_clock::time_point anchor_time_;
int64_t anchor_position_ms_ = 0;
size_t anchor_consumed_frames_ = 0;
int anchor_sample_rate_ = 0;
```

**Update cycle** (~30Hz):
1. Read `frames_consumed()` from the ring buffer's atomic counter
2. Compute delta from last anchor in frames, convert to milliseconds
3. Advance `anchor_position_ms_`, reset `anchor_time_` to now

**Interpolation** (between updates):
```cpp
int64_t get_interpolated_position_ms() const {
    auto elapsed = steady_clock::now() - anchor_time_;
    return anchor_position_ms_ + duration_cast<milliseconds>(elapsed).count();
}
```

**Seek handling**: Flushes ring buffer, resets anchor to target position.

### Audio Processing

**Volume Control**:
- Software volume adjustment (0-100%, 5% increments)
- Applied in RT callback (no separate DSP pass): `dst[i] *= scale`
- Atomic `volume_` load with `memory_order_relaxed` (no ordering needed)

**Sample Clamping**:
- NaN/Inf clamping on producer side (decode thread), keeping RT callback branch-free
- Clamps to [-1.0, 1.0] range

---

## Library Management

### Directory Scanning: Kernel-Level Syscalls

**File**: `src/util/DirectoryScanner.cpp`

OUROBOROS bypasses `std::filesystem` for raw Linux kernel syscalls.

#### Direct getdents64 Implementation

**System Call**:
```cpp
syscall(SYS_getdents64, fd, buffer, BUFFER_SIZE);
```

**Performance Gain**: **2-3x faster** than std::filesystem::recursive_directory_iterator

**Optimizations:**
1. **256KB stack buffer**: Batches 3000+ directory entries per syscall
2. **d_type filtering**: Uses kernel-provided file type (avoids per-entry `stat()` calls)
3. **fstatat() for mtimes**: Relative path stats (faster than absolute)
4. **Extension pre-filter**: Only stats `.mp3`, `.flac`, `.ogg`, `.wav`, `.m4a` files

**Why It Matters**: Scanning 10,000-track library in <500ms vs 1500ms with std::filesystem.

### Multi-Tier Cache System

**File**: `src/collectors/LibraryCollector.cpp`

Intelligent cache invalidation with three validation tiers:

#### TIER 0: Tree Hash Validation -- O(1)

- Concatenates all file paths, computes SHA-256 once
- Truncates to `uint64_t` for quick equality check
- **Cache hit**: Library loads in <100ms (no file I/O)
- **Cache miss**: Proceeds to TIER 1

#### TIER 1: Directory-Level Dirty Detection -- O(directories)

- Scans only directory mtimes using `getdents64`
- Identifies changed directories without scanning files
- **No changes**: Uses cache
- **Changes detected**: Proceeds to TIER 2

#### TIER 2 & 3: Incremental Parsing with Parallelism -- O(changed files)

- Compares inode/mtime pairs to skip unchanged tracks
- **Parallel extraction** using hardware-aware thread pool:
  - Thread count = `std::thread::hardware_concurrency()`
  - Lock-free work distribution via `std::atomic<size_t>`
  - Per-thread metadata parsing (libmpg123/libsndfile are thread-safe)
  - Pre-allocated result vectors (no locks during accumulation)

**Real-World Example**:
- 10,000-track library, changed 50 tracks
- TIER 0 fails -> TIER 1 identifies 3 dirty directories -> TIER 2 parses only 50 files
- **Result**: 500ms scan instead of 30-second full rescan

### Metadata Extraction

**Tag Support**:
- **MP3**: ID3v1, ID3v2 (title, artist, album, genre, date, track number, TRCK frame)
- **FLAC/OGG**: Vorbis comments via libsndfile
- **WAV**: Metadata via libsndfile

**Parallel Parsing**:
- Hardware-aware thread pool (std::thread::hardware_concurrency)
- Lock-free work distribution with atomic counters
- Pre-allocated buffers (no heap allocations during parsing)

### Cache Persistence

**Binary Format**: Custom serialization with magic/version

**Cache Locations** (`~/.cache/ouroboros/`):
- `library.bin` - Monolithic library cache
- `artwork.cache` - Content-addressed artwork storage

---

## Advanced Algorithms

### PowerSort Implementation

**File**: `include/util/TimSort.hpp`

OUROBOROS implements PowerSort (Munro & Wild 2018), the merge policy adopted by CPython 3.11+ as a drop-in improvement over classic TimSort. It replaces TimSort's ad-hoc stack invariant heuristic with a provably near-optimal merge tree.

#### PowerSort Merge Policy

The `powerloop()` function computes the depth in a conceptual nearly-optimal binary merge tree for the boundary between two adjacent runs:

```cpp
// s1: start of left run, n1: left length, n2: right length, n: total
int powerloop(size_t s1, size_t n1, size_t n2, size_t n) {
    int result = 0;
    size_t a = 2 * s1 + n1;        // doubled midpoint of left run
    size_t b = a + n1 + n2;         // doubled midpoint of right run
    for (;;) {
        ++result;
        if (a >= n) { a -= n; b -= n; }
        else if (b >= n) break;     // bits differ: this is the power
        a <<= 1; b <<= 1;
    }
    return result;
}
```

When a new run is found, runs with power >= the new boundary's power are merged first. This produces a merge order that is provably within O(n) comparisons of optimal.

#### Galloping Mode

When one run dominates during a merge (many consecutive elements from the same side), the algorithm switches from one-at-a-time comparison to **exponential search** (galloping):

- **`gallop_left`**: Find leftmost insertion point via exponential search + binary search
- **`gallop_right`**: Find rightmost insertion point
- **Adaptive threshold**: `min_gallop` starts at 7, decreases when galloping succeeds (encouraging it), increases when it fails (discouraging it)
- **Pre-merge trimming**: Skip elements already in position via `gallop_right`/`gallop_left` before merging

#### Merge Operations

- **`merge_lo`**: Left run smaller -- copy left to temp, merge left-to-right
- **`merge_hi`**: Right run smaller -- copy right to temp, merge right-to-left
- Both use `goto`-based epilogue for the one-element-remaining fast path (matching CPython's structure)

#### Characteristics

- **O(n) best case**: Exploits existing order in pre-sorted music libraries
- **O(n log n) worst case**: Guaranteed performance ceiling
- **Stable sort**: Preserves relative order of equal elements
- **Adaptive**: Automatically detects natural runs (already-sorted sequences)
- Binary insertion sort for small runs (<32 elements)
- Automatic run reversal when descending order detected

#### Parallel PowerSort

`parallel_timsort` divides data into chunks, sorts each in parallel with `timsort`, then merges via parallel tree reduction:

1. **Phase 1**: N worker threads each sort one chunk with PowerSort
2. **Phase 2**: Tree-reduction merge -- chunk pairs merged in parallel at each level

#### Why It Matters

Real-world music libraries are often partially sorted (albums grouped together, artists alphabetized). PowerSort outperforms std::sort by detecting and exploiting this existing order.

**Library Sorting Order**: Artist -> Year -> Track

### Boyer-Moore-Horspool String Search

**File**: `include/util/BoyerMoore.hpp`

Fast substring matching for global search.

#### Performance

- **Average case**: O(n/m) -- sublinear (2-3x faster than naive search)
- **Worst case**: O(n*m) -- extremely rare in practice
- **Space complexity**: O(1) -- fixed 256-byte alphabet table (no heap allocations)

#### Implementation

- Precomputed bad-character skip table
- Right-to-left pattern matching
- Case-sensitive and case-insensitive modes
- Real-time filtering (updates on every keystroke)

#### Real-World Impact

Searching 10,000 tracks for "Radiohead" scans ~3,300 characters instead of ~1,000,000 (naive approach).

### SHA-256 Content Addressing

**File**: `src/util/ArtworkHasher.cpp`

Custom SHA-256 implementation for artwork deduplication.

#### Algorithm Details

**Standard**: NIST FIPS 180-4 compliant

**Implementation**:
- 512-bit chunk processing
- 64 round constants (K[0..63])
- Bitwise operations: rotr (rotate right), ch (choose), maj (majority)
- Proper padding and length encoding

#### Application

100 tracks from same album -> 1 cached JPEG (99% deduplication)

---

## Kernel-Level Optimizations

### Shared Memory Image Transmission

**File**: `src/ui/ImageRenderer.cpp`

Uses `/dev/shm` (RAM filesystem) for Kitty protocol artwork.

#### Fast Path (Kitty/WezTerm)

```
Write JPEG -> /dev/shm/ouroboros-art-XXXXXX -> Send file path to terminal -> Delete
```

#### Slow Path (Ghostty, older terminals)

```
Base64 encode (33% size increase) -> Send full data via stdout
```

#### Performance Impact

Shared memory eliminates Base64 encoding overhead and reduces terminal I/O by 33%.

**Transmission Flow**:
1. Decode image to RGBA pixels
2. Resize to target dimensions (stb_image_resize2)
3. Encode to JPEG in memory
4. Write to `/dev/shm/ouroboros-art-XXXXXX`
5. Send Kitty protocol escape sequence with file path
6. Delete temp file after transmission

---

## Artwork System

### Content-Addressed Storage

**SHA-256 Deduplication**:
- Hash artwork bytes -> unique identifier
- 100 tracks from same album -> 1 cached JPEG
- 99% space savings for typical album-based libraries

**Reference Counting**:
- Tracks artwork usage
- Evicts when ref count hits 0
- LRU eviction for cache overflow

**Binary Cache Format**:
- Magic: `0x4F55524F41525431` ("OUROART1")
- Version: 1
- Custom serialization with size prefixes

### Multi-Tier Caching

**Three-Level Architecture**:

1. **ArtworkCache (Global)**: SHA-256 indexed storage on disk
   - Thread-safe singleton
   - Persists between sessions

2. **ArtworkLoader (Per-Track)**: LRU cache (500 entries)
   - Viewport protection (visible items never evicted)
   - Sliding window prefetch (20 items ahead/behind)

3. **ImageRenderer (Decoded Pixels)**: LRU cache (250 entries)
   - RGBA pixel data ready for transmission
   - Generation tokens for invalidation

### Async Decoding Pipeline

**ImageDecoderPool**:
- Fixed thread pool (hardware_concurrency cores)
- Priority queue (viewport-aware scheduling)
- Radial rendering (distance-based prioritization from cursor)
- Queue backpressure (50-job limit prevents memory explosion)

**Image Processing**:
- **stb_image**: JPEG/PNG decoding (public domain)
- **stb_image_resize2**: High-quality Lanczos3 downsampling
- **Cell Size Detection**: Automatic pixel-per-cell detection via terminal queries

### Terminal Protocol Support

**4 Image Protocols**:

1. **Kitty Graphics Protocol** (gold standard)
   - Shared memory transmission (`/dev/shm`)
   - Surgical deletion (delete-by-ID)
   - Pixel-perfect clipping

2. **Sixel** (xterm, mlterm, mintty)
   - Base64 encoding
   - Most compatible fallback

3. **iTerm2 Inline Images**
   - Base64 transmission
   - macOS support

4. **Unicode Blocks** (always works)
   - Character-based rendering
   - 2x2 pixel per character

**Automatic Detection**:
- Query-based capability detection (DA1 responses, Kitty graphics query)
- Environment variable checks (`KITTY_WINDOW_ID`, `TERM_PROGRAM`, `TERM`)
- Fallback cascade (Kitty -> Sixel -> Unicode)
- Manual override via `OUROBOROS_IMAGE_PROTOCOL`

---

## 5-Phase Rendering Pipeline

**File**: `src/ui/widgets/AlbumBrowser.cpp`

The album grid uses a 5-phase rendering pipeline that eliminates flicker during scrolling by ensuring new images render before old ones are deleted.

### Phase Overview

```
Phase 1: Slot Assignment + Request    -> Assign visible albums to atomic slots, queue artwork requests
Phase 2: Populate Slots               -> Copy decoded pixels from ArtworkWindow to slots (mutex brief)
Phase 3: Lock-Free Render             -> Render from slots atomically, no mutex during draw
Phase 4: Orphan Cleanup               -> Delete images no longer on screen (AFTER all renders)
Phase 5: Prefetch                     -> Queue artwork for items just outside viewport (when idle)
```

### Phase 1: Slot Assignment + Request

For each visible album grid position:
1. Calculate slot index: `visible_row * cols + col`
2. Assign album to slot (bumps generation token if album changed)
3. Request artwork from ArtworkWindow with Manhattan distance priority

```cpp
int distance = std::abs(row - selected_row) + std::abs(col - selected_col);
artwork_window.request(track_path, distance, cols, rows, false);  // batch
```

Requests are batched, then `flush_requests()` wakes worker threads.

### Phase 2: Populate Slots

The **only** place that takes the ArtworkWindow mutex:

```cpp
const auto* artwork = artwork_window.get_decoded(path, cols, rows);
if (!artwork) continue;

// Copy to slot (atomic publish pattern)
slot.decoded_pixels.assign(artwork->data, artwork->data + artwork->data_size);
slot.width = artwork->width;
slot.height = artwork->height;
slot.state.store(SlotState::Ready, std::memory_order_release);  // PUBLISH
```

The `release` store publishes all prior writes to readers using `acquire` loads.

### Phase 3: Lock-Free Render

Reads slots atomically - no mutex during actual terminal rendering:

```cpp
SlotState state = slot.state.load(std::memory_order_acquire);
if (state != SlotState::Ready) continue;

// Safe to read slot data - state is Ready
uint32_t image_id = img_renderer.render_image(slot.decoded_pixels.data(), ...);
```

Renders in radial order (sorted by Manhattan distance from selection).

### Phase 4: Orphan Cleanup

**Key to no-flash**: Delete old images **after** new ones render.

```cpp
for (const auto& [key, info] : displayed_images_) {
    if (new_displayed_images.find(key) == new_displayed_images.end()) {
        img_renderer.delete_image_by_id(info.image_id);
    }
}
```

### Phase 5: Prefetch

Triggered after 150ms idle (no scroll activity):

```cpp
if (time_since_scroll >= PREFETCH_DELAY_MS && !prefetch_completed_) {
    // Queue albums above and below viewport with low priority (priority + 1000)
    for (int r = start_row - prefetch_rows; r < start_row; ++r)
        artwork_window.request(path, distance + 1000, cols, rows, false);
}
```

Low-priority prefetch items don't trigger re-renders when complete.

---

## Smart Scroll Optimization

**File**: `include/ui/widgets/AlbumBrowser.hpp`

### Constants

```cpp
static constexpr auto SCROLL_DEBOUNCE_MS = std::chrono::milliseconds(35);
static constexpr auto PREFETCH_DELAY_MS = std::chrono::milliseconds(150);
static constexpr int BIG_JUMP_ROWS = 10;      // Velocity threshold
static constexpr int HUGE_JUMP_ROWS = 25;     // Distance threshold (always triggers)
static constexpr auto BIG_JUMP_TIME_LIMIT = std::chrono::milliseconds(2000);
```

### Scroll Debouncing

Prevents per-frame request spam during fast scrolling:

```cpp
if (!force_render && (now - last_request_time_) < SCROLL_DEBOUNCE_MS) {
    return;  // Skip artwork requests this frame
}
```

### Big Jump Detection

Uses **velocity + distance formula** to detect fast scrolling vs. slow navigation:

```cpp
// On scroll stop:
int distance = std::abs(scroll_offset_ - scroll_start_offset_);
double time_seconds = elapsed_ms / 1000.0;
double velocity = distance / time_seconds;  // rows per second

// Big jump if: (distance >= 8 AND velocity > 10) OR distance > 25
bool is_big_jump = (distance >= 8 && velocity > 10.0) || (distance > HUGE_JUMP_ROWS);
```

On big jump detection:
1. Clear ArtworkWindow request queue (`artwork_window.reset()`)
2. Delete all displayed images
3. Clear all atomic slots
4. Force re-render

This prevents stale artwork from loading after user has scrolled past.

### Prefetch Delay

Waits 150ms after scroll stops before prefetching:

```cpp
if (time_since_scroll >= PREFETCH_DELAY_MS && !prefetch_completed_) {
    // Queue 20 rows above and below viewport
}
```

---

## Atomic Slot System

**File**: `include/ui/widgets/AlbumBrowser.hpp`

### Slot State Machine

```cpp
enum class SlotState : uint8_t {
    Empty,    // Slot not assigned or cleared
    Loading,  // Request in flight (currently unused - direct to Ready)
    Ready     // Decoded and ready to render
};
```

### Slot Structure

```cpp
struct AlbumBrowserSlot {
    std::atomic<SlotState> state{SlotState::Empty};
    std::atomic<uint64_t> generation{0};  // Bumped on reassignment

    // Data fields (only written during Empty->Ready transition)
    std::vector<uint8_t> decoded_pixels;
    int width, height;
    CachedFormat format;
    std::string hash;
    std::string album_dir;

    // Terminal display state
    uint32_t image_id;
    int display_x, display_y, display_cols, display_rows;
};
```

### Memory Ordering

**Writer** (populate phase):
```cpp
slot.decoded_pixels.assign(...);  // Write data
slot.width = artwork->width;
slot.state.store(SlotState::Ready, std::memory_order_release);  // PUBLISH
```

**Reader** (render phase):
```cpp
SlotState state = slot.state.load(std::memory_order_acquire);  // SUBSCRIBE
if (state == SlotState::Ready) {
    // All data writes are visible here
    render(slot.decoded_pixels.data(), slot.width, ...);
}
```

The `release`/`acquire` pair establishes a happens-before relationship.

### Generation Tokens

Prevent stale results when slot is reassigned to different album:

```cpp
void assign_slot(size_t slot_idx, const std::string& album_dir, ...) {
    if (slot.album_dir != album_dir) {
        slot.generation.fetch_add(1, std::memory_order_release);
        slot.album_dir = album_dir;
        slot.state.store(SlotState::Empty, std::memory_order_release);
        // Clear data but don't delete image - orphan cleanup handles it
    }
}
```

### Slot Array

Fixed-size array avoids allocation during scroll:

```cpp
static constexpr size_t MAX_VISIBLE_SLOTS = 64;
std::array<AlbumBrowserSlot, MAX_VISIBLE_SLOTS> slots_;

size_t get_slot_index(int visible_row, int col) const {
    return static_cast<size_t>(visible_row * cols_ + col);
}
```

---

## ArtworkWindow Coordinator

**File**: `src/ui/ArtworkWindow.cpp`

Coordinates async artwork loading with request batching and priority queue.

### Architecture

```
AlbumBrowser -> request() -> Priority Queue -> Worker Threads -> ArtworkCache
                                ↓                    ↓
                           flush_requests()     get_decoded()
                                ↓                    ↓
                           notify_all()        Slot Population
```

### Request Batching

Requests are queued without waking workers until `flush_requests()`:

```cpp
void request(const std::string& path, int priority, int w, int h, bool notify) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        request_queue_.push(WindowRequest{path, priority, ...});
    }
    if (notify) queue_cv_.notify_one();  // Only wake on explicit notify
}

void flush_requests() {
    queue_cv_.notify_all();  // Wake all workers at once
}
```

### Priority Queue

Manhattan distance determines priority (lower = higher priority):

```cpp
struct WindowRequest {
    std::string path;
    int priority;      // Manhattan distance from selection
    int64_t timestamp; // Tiebreaker for equal priorities
    int target_width, target_height;
    bool force_extract;
};

// Priority comparator: lower priority value = process first
struct RequestComparator {
    bool operator()(const WindowRequest& a, const WindowRequest& b) const {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.timestamp > b.timestamp;  // Earlier requests first
    }
};

std::priority_queue<WindowRequest, std::vector<WindowRequest>, RequestComparator> request_queue_;
```

### Worker Threads

Fixed thread pool processes requests:

```cpp
static constexpr size_t NUM_WORKERS = 4;  // Or hardware_concurrency
std::vector<std::thread> workers_;

void worker_thread() {
    while (!should_stop_) {
        WindowRequest req;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, WORKER_TIMEOUT, [this] {
                return !request_queue_.empty() || should_stop_;
            });
            if (should_stop_ || request_queue_.empty()) continue;
            req = request_queue_.top();
            request_queue_.pop();
        }
        // Process: extract/load JPEG -> decode -> resize -> store in cache
    }
}
```

### Update Notification

Workers signal when visible artwork is ready:

```cpp
// Worker: only signal for visible items (priority < 1000)
if (req.priority < 1000) {
    has_updates_.store(true);
}

// AlbumBrowser: check for updates each frame
if (artwork_window.has_updates()) {
    artwork_window.clear_updates();
    force_render = true;  // Trigger slot population + render
}
```

### LRU Cache with Viewport Protection

**Memory limit**: Configurable (default 3GB), evicts when exceeded:

```cpp
size_t memory_limit_bytes_ = config.get_artwork_memory_limit_mb() * 1024 * 1024;

void evict_until_under_limit() {
    while (total_bytes_.load() > memory_limit_bytes_ && !lru_list_.empty()) {
        auto oldest_key = lru_list_.back();
        auto& entry = cache_[oldest_key];

        if (entry->state == NowPlayingSlotState::Ready) {
            entry->decoded_pixels.clear();
            entry->decoded_pixels.shrink_to_fit();  // Deallocate
            entry->state.store(NowPlayingSlotState::Evicted);
            total_bytes_.fetch_sub(entry_bytes);
        }
        lru_list_.pop_back();
    }
}
```

**Viewport protection**: Visible items move to front of LRU on access:

```cpp
const DecodedArtwork* get_decoded(const std::string& path, ...) {
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second->state == Ready) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second->lru_iter);
        return &result;
    }
}
```

---

## Scattered Album Detection & Merging

**File**: `src/ui/widgets/AlbumBrowser.cpp`

### Problem

Soundtracks and compilations often have tracks from multiple artists:
- "Guardians of the Galaxy" has tracks by multiple artists
- "FACT Mixes" may span multiple years with same artist

### Detection: Title Occurrence Counting

```cpp
std::unordered_map<std::string, int> title_count;
for (const auto& album : albums_) {
    title_count[album.normalized_title]++;
}

// Albums with title appearing more than once are "scattered"
bool is_scattered = title_count[album.normalized_title] > 1;
```

### Merging: By Title Only

Scattered albums are merged by **title only**, ignoring artist. This handles true compilations (soundtracks, Various Artists) where every track has a different artist.

```cpp
std::unordered_map<std::string, size_t> merge_map;  // title -> index
std::vector<AlbumGroup> merged_albums;

for (auto& album : albums_) {
    if (is_scattered) {
        std::string merge_key = album.normalized_title;  // Title only
        auto it = merge_map.find(merge_key);

        if (it != merge_map.end()) {
            // Merge track indices into existing entry
            merged_albums[it->second].track_indices.insert(...);
            // Keep earliest year
            if (album.year < existing.year) existing.year = album.year;
        } else {
            merge_map[merge_key] = merged_albums.size();
            merged_albums.push_back(std::move(album));
        }
    } else {
        merged_albums.push_back(std::move(album));
    }
}
```

### Dual-Sort Strategy

1. **PowerSort** first: Artist -> Year -> Title (primary sort)
2. **stable_sort** second: Group scattered albums by title instead of artist

```cpp
// First: PowerSort by artist/year/title
ouroboros::util::timsort(albums_, [](const AlbumGroup& a, const AlbumGroup& b) {
    int cmp = case_insensitive_compare(a.artist, b.artist);
    if (cmp != 0) return cmp < 0;
    if (sort_by_year) {
        if (year_to_int(a.year) != year_to_int(b.year))
            return year_to_int(a.year) < year_to_int(b.year);
    }
    return case_insensitive_compare(a.title, b.title) < 0;
});

// Second: stable_sort to group scattered albums by title
std::stable_sort(albums_.begin(), albums_.end(), [&](const AlbumGroup& a, const AlbumGroup& b) {
    bool a_scattered = title_count[a.normalized_title] > 1;
    bool b_scattered = title_count[b.normalized_title] > 1;

    // Scattered albums sort by title, others by artist
    std::string key_a = a_scattered ? a.normalized_title : a.artist;
    std::string key_b = b_scattered ? b.normalized_title : b.artist;

    return case_insensitive_compare(key_a, key_b) < 0;
});
```

This groups all "Guardians of the Galaxy" tracks together while keeping regular albums sorted by artist.

---

## Unicode Support

**File**: `include/util/UnicodeUtils.hpp`

Full Unicode normalization for case-insensitive search and sorting using ICU (International Components for Unicode).

### normalize_for_search()

**Transliteration Chain**: `NFD -> [:Nonspacing Mark:] Remove -> NFC -> Latin-ASCII`

**Examples**:
- "Bjork" -> "bjork"
- "Jose" -> "jose"
- "Motorhead" -> "motorhead"

**Purpose**: Enables ASCII search input to match international artist names.

### case_insensitive_compare()

**Uses ICU's foldCase()**:
- Proper Unicode comparison
- Groups "BUTTHOLE SURFERS", "Butthole Surfers", "butthole surfers" correctly
- Handles Turkish I/i, German ss, etc.

**Why ICU?**
- Supports 150+ languages
- Handles 1.4 million+ characters
- Proper case-folding rules per language

---

## Performance Characteristics

| Operation | Complexity | Real-World Performance | Notes |
|-----------|------------|----------------------|-------|
| **Library Scan (10K tracks)** | O(n) | <500ms | Direct `getdents64` syscalls (2-3x faster than `std::filesystem`), 256KB batching |
| **Cache TIER 0 Validation** | O(1) | <100ms | Single SHA-256 tree hash comparison |
| **Cache TIER 1 (dir scan)** | O(directories) | ~200ms | `getdents64` on directories only, `d_type` filtering |
| **Cache TIER 2 (incremental)** | O(changed_files) | 500ms for 50 changes | Parallel metadata extraction (4-16 threads) |
| **Snapshot Read** | O(1) | <1us | Lock-free atomic pointer load with acquire semantics |
| **UI Render (30 FPS)** | O(widgets) | ~33ms/frame | Only redraws on state change, canvas diffing |
| **PowerSort** | O(n) -- O(n log n) | ~10ms for 10K tracks | O(n) for pre-sorted data, galloping for unequal runs |
| **Boyer-Moore Search** | O(n/m) avg, O(nm) worst | ~1ms for 10K tracks | Sublinear: scans ~3,300 chars instead of 1M |
| **SHA-256 Hash** | O(data_size) | ~1ms for 5MB JPEG | 512-bit chunks, 64 rounds per chunk |
| **Artwork Cache Lookup** | O(1) | <1us | `unordered_map` with SHA-256 keys |
| **Image Decode (JPEG)** | Async | 150-250ms for 32 images | Parallel pool (4-16 threads), stb_image + stb_resize |
| **LRU Cache Hit** | O(1) | <1us | 250-entry ImageRenderer cache, hash-based lookup |
| **Canvas Draw** | O(width*height) | ~500us for 120x40 | Linear buffer, UTF-8 aware, ANSI escape parsing |
| **FlexLayout Compute** | O(items*iterations) | ~100us for 8 widgets | Typically 2-3 constraint iterations |
| **EventBus Publish** | O(subscribers) | ~10us | Typically <10 subscribers per event type |
| **Ring Buffer Read/Write** | O(frames) | <1us per quantum | Lock-free SPSC, `alignas(64)` atomics, zero contention |
| **Position Interpolation** | O(1) | <1us | `steady_clock` delta + integer arithmetic |

### Real-World Benchmarks

**Test System**: Ryzen 7 5800X, 721-track library

- **Cold start** (no cache): 850ms to first render
- **Warm start** (TIER 0 hit): 95ms to first render
- **Album grid scroll**: 30 FPS sustained with 32 visible artworks
- **Search latency**: <5ms per keystroke (real-time filtering)
- **Gapless transition**: Zero-silence crossover between same-format tracks

---

## Security

### Cryptographically Secure Shuffle

OUROBOROS uses the Linux `getrandom()` syscall directly for shuffle randomness, providing cryptographically secure pseudo-random number generation (CSPRNG). This ensures:

- **Unpredictable shuffle order** - Cannot be guessed or predicted
- **Proper entropy** - Each random pick reads directly from the kernel's CSPRNG (ChaCha20-based with BLAKE2s on kernel 5.17+)
- **No weak fallbacks** - Direct syscall, no `/dev/urandom` file descriptors, no C++ `<random>` library

While shuffle randomness doesn't require cryptographic strength, using CSPRNG is best practice and demonstrates security-conscious design with zero performance cost.

---

## Code Quality & Engineering Patterns

### Memory Management

- **RAII Everywhere**: Automatic cleanup via destructors, smart pointers (`unique_ptr`, `shared_ptr`)
- **Move Semantics**: Extensive use of `std::move` to avoid copies (vectors, strings, large structs)
- **Perfect Forwarding**: Template functions use `std::forward` for zero-overhead parameter passing
- **Pre-allocation**: `vector.reserve()` calls prevent heap reallocations during growth
- **Stack Allocation**: Small buffers on stack (256KB for getdents64), large data on heap

### Concurrency Patterns

- **Lock-Free Reading**: Atomic operations with `std::memory_order_acquire`/`release`
- **SPSC Ring Buffer**: Lock-free producer-consumer for audio pipeline
- **Mutex Discipline**: `std::lock_guard` for RAII-style locking, never manual lock/unlock
- **Condition Variables**: Thread wake/sleep for worker pools (no busy-waiting)
- **Atomic Counters**: Lock-free work distribution in parallel algorithms
- **Cache-Line Isolation**: `alignas(64)` on contended atomics to prevent false sharing

### Error Handling

- **std::optional**: Returns for operations that may fail (cache lookup, file read)
- **Validation First**: Check preconditions before operations (bounds, null checks)
- **Graceful Degradation**: Fallback paths (Sixel -> Unicode blocks, cache miss -> full scan)
- **Comprehensive Logging**: Every major operation logged for debugging

### Code Organization

- **Single Responsibility**: Each class has one clear purpose
- **Dependency Injection**: Publishers, configs passed to constructors (testable)
- **Interface Segregation**: Abstract base classes (AudioDecoder, Component)
- **Composition Over Inheritance**: Widgets composed of smaller pieces
- **Namespace Hygiene**: Everything in `ouroboros::` with logical subnamespaces

### Performance Discipline

- **Profile-Guided**: Optimizations justified by real-world measurements
- **Algorithm Choice**: Uses theoretically optimal algorithms (PowerSort for adaptive sort)
- **System-Level**: Drops to syscalls when standard library insufficient
- **Cache Locality**: Hot paths use contiguous memory (vector over list)
- **Zero-Copy**: Move semantics, shared_ptr, avoid unnecessary clones
- **RT-Safe Audio**: No allocations, no logging, no locks in PipeWire callback

### Documentation

- **Detailed Comments**: Algorithm explanations (TIER comments, NOTE blocks)
- **Performance Notes**: Why optimizations were chosen (getdents64 rationale)
- **Ownership Documentation**: Who owns what data, lifetime management
- **Architecture Diagrams**: ASCII art pipeline diagrams in comments

---

## Project Structure

```
ouroboros/
├── src/                      # 45 implementation files (~10,878 lines)
│   ├── main.cpp              # Entry point, event loop
│   ├── audio/                # 4 decoders + PipeWire context/output
│   ├── backend/              # Library, metadata, config, snapshot publisher/buffers
│   ├── collectors/           # LibraryCollector, PlaybackCollector threads
│   ├── config/               # Theme and keybind management
│   ├── events/               # EventBus (publish-subscribe), Scheduler
│   ├── model/                # Snapshot, Track, PlayerState data models
│   ├── ui/                   # Terminal, Canvas, Renderer, widgets, FlexLayout, ArtworkWindow
│   └── util/                 # PowerSort, BoyerMoore, DirectoryScanner, Logger, ImageDecoderPool
├── include/                  # 51 header files (~3,798 lines) mirroring src/
├── tests/                    # C++ test framework
│   ├── framework/            # SimpleTest.hpp (custom test runner)
│   ├── unit/                 # PowerSort, BoyerMoore, ArtworkHasher tests
│   └── integration/          # Metadata pipeline tests
├── config/                   # Example configuration files
│   └── ouroboros.toml.example
├── vendor/                   # Third-party libraries (stb_image, etc.)
├── CMakeLists.txt            # Build configuration
└── Makefile                  # Convenience wrapper
```

### Code Statistics

- **Total Lines**: ~14,676 (10,878 implementation + 3,798 headers)
- **Source Files**: 45 `.cpp` files
- **Header Files**: 51 `.hpp` files
- **Audio Decoders**: 4 (MP3, FLAC/WAV via libsndfile, OGG, M4A/AAC via FFmpeg)
- **Image Protocols**: 4 (Kitty, Sixel, iTerm2, Unicode blocks)
- **UI Widgets**: 6 (Browser, Queue, NowPlaying, SearchBox, AlbumBrowser, HelpOverlay)
- **Background Threads**: 4+ (Main, LibraryCollector, PlaybackCollector, ArtworkLoader, ImageDecoderPool workers)
- **Test Suites**: 3 (unit/test_utils, unit/test_core, integration/test_pipeline)

---

## Design Patterns

### Singleton Pattern
- `ImageRenderer` - Global image rendering state
- `ArtworkLoader` - Global artwork loading coordinator
- `EventBus` - Global event pub/sub system
- `Terminal` - Global terminal state

### Factory Pattern
- Decoder creation based on audio format (MP3/FLAC/OGG/M4A)

### Observer Pattern
- `EventBus` publish-subscribe for decoupled communication

### Strategy Pattern
- Polymorphic `AudioDecoder` implementations

### Adapter Pattern
- Terminal protocol abstraction (Kitty/Sixel/iTerm2/Unicode)

### Facade Pattern
- `SnapshotPublisher` hides buffer complexity

### RAII Pattern
- All resource management (files, threads, mutexes, smart pointers)

---

*For user-facing documentation, see [README.md](README.md)*
