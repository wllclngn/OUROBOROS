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
7. [Unicode Support](#unicode-support)
8. [Performance Characteristics](#performance-characteristics)
9. [Code Quality & Engineering Patterns](#code-quality--engineering-patterns)
10. [Project Structure](#project-structure)

---

## Concurrency Architecture

### Lock-Free Snapshot System

**File**: `src/backend/SnapshotBuffers.cpp`

OUROBOROS achieves **zero deadlocks** through an immutable snapshot architecture using atomic double-buffering.

#### Three-Buffer Pattern

- **Front buffer**: Atomically readable by UI thread
- **Back buffer**: Writeable by collector threads (mutex-protected)
- **Spare buffer**: For atomic swap

#### Atomic Read Path (Lock-Free)

```cpp
std::atomic<Snapshot*> front_;
Snapshot* snapshot = front_.load(std::memory_order_acquire);  // No mutex!
```

**Key Insight**: UI thread never blocks on mutex acquisition. Reads are instantaneous (<1μs).

#### Write Path

```cpp
{
    std::lock_guard lock(write_mutex_);
    modify_back_buffer();
    front_.store(back_, std::memory_order_release);  // Atomic swap
}
```

#### Guarantees

- UI thread **never blocks** on mutex
- Writes are serialized (only one collector modifies at a time)
- Sequence counter detects stale reads
- State accumulation ensures back buffer stays current

### Threading Model

OUROBOROS runs 4+ threads concurrently:

1. **Main Thread**: UI rendering (30 FPS) + input handling
   - Lock-free reads via `publisher->get_current()` (atomic pointer load)
   - Never blocks on mutexes during render

2. **LibraryCollector**: Background library scanning
   - Metadata parsing with parallel thread pool
   - Artwork extraction
   - Cache management

3. **PlaybackCollector**: Audio decoding thread
   - Format-specific decoder instantiation
   - PipeWire output management
   - Playback state updates

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
- `QueueState` (current queue, index)
- `UIState` (focus, search query, viewport)

**Double Buffering**: Front buffer for reads, back buffer for atomic swap publishing

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
File → Format Detection → Decoder (MP3/FLAC/OGG/WAV) → PCM Float Buffer → PipeWire → Speakers
         ↓                     ↓                              ↓
    Extension + Magic       Per-track sample rate       Format negotiation
```

### Format Detection

1. **Extension-based**: Check file extension (`.mp3`, `.flac`, `.ogg`, `.wav`)
2. **Magic Bytes**: Verify file headers to prevent misclassification
3. **Decoder Selection**: Polymorphic `AudioDecoder` base class

### Decoder Implementations

**MP3Decoder** (`src/audio/MP3Decoder.cpp`):
- Uses libmpg123
- Supports ID3v1/ID3v2 tag parsing
- Variable bitrate (VBR) support
- Accurate seeking via frame positioning

**FLACDecoder** (`src/audio/FlacDecoder.cpp`):
- Uses libsndfile
- Lossless compression
- Vorbis comment metadata
- Native 16/24-bit depth support

**OGGDecoder** (`src/audio/OggDecoder.cpp`):
- Uses libvorbisfile
- Lossy compression (superior to MP3 at same bitrate)
- Vorbis comments for metadata

**WAVDecoder** (`src/audio/WavDecoder.cpp`):
- Uses libsndfile
- Uncompressed PCM
- Fast decoding (no decompression)

### PipeWire Integration

**Native PipeWire Output** (`src/audio/PipeWireOutput.cpp`):
- **Modern Linux Audio**: No ALSA/PulseAudio fallbacks
- **Per-Track Format Negotiation**: Dynamically reconfigures stream for each track's sample rate/channels
- **Shared Context Pattern**: Single `PipeWireContext` with thread-loop shared across streams
- **Float32 Pipeline**: Native F32 audio processing (16384 frame buffer = 85ms @ 192kHz)
- **Thread Safety**: All PipeWire operations protected by `pw_thread_loop_lock/unlock`

### Audio Processing

**Precision Seeking**:
- Millisecond-accurate seek via frame-based API
- Maintains audio sync during seeks

**Volume Control**:
- Software volume adjustment (0-100%, 5% increments)
- Applied during audio callback (no separate DSP pass)

**Position Tracking**:
- Real-time position reporting (ms and frames)
- Duration calculation from decoder frame count

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

#### TIER 0: Tree Hash Validation — O(1)

- Concatenates all file paths, computes SHA-256 once
- Truncates to `uint64_t` for quick equality check
- **Cache hit**: Library loads in <100ms (no file I/O)
- **Cache miss**: Proceeds to TIER 1

#### TIER 1: Directory-Level Dirty Detection — O(directories)

- Scans only directory mtimes using `getdents64`
- Identifies changed directories without scanning files
- **No changes**: Uses cache
- **Changes detected**: Proceeds to TIER 2

#### TIER 2 & 3: Incremental Parsing with Parallelism — O(changed files)

- Compares inode/mtime pairs to skip unchanged tracks
- **Parallel extraction** using hardware-aware thread pool:
  - Thread count = `std::thread::hardware_concurrency()`
  - Lock-free work distribution via `std::atomic<size_t>`
  - Per-thread metadata parsing (libmpg123/libsndfile are thread-safe)
  - Pre-allocated result vectors (no locks during accumulation)

**Real-World Example**:
- 10,000-track library, changed 50 tracks
- TIER 0 fails → TIER 1 identifies 3 dirty directories → TIER 2 parses only 50 files
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
- `library.bin` - Monolithic cache (CACHE_VERSION 3)
- `dirs/` - Hierarchical per-directory caches
- `artwork.cache` - Content-addressed artwork storage

---

## Advanced Algorithms

### TimSort Implementation

**File**: `include/util/TimSort.hpp`

OUROBOROS implements the TimSort algorithm (standard in Python/Java) for library sorting.

#### Characteristics

- **O(n) best case**: Exploits existing order in pre-sorted music libraries
- **O(n log n) worst case**: Guaranteed performance ceiling
- **Stable sort**: Preserves relative order of equal elements
- **Adaptive**: Automatically detects natural runs (already-sorted sequences)

#### Key Optimizations

- Binary insertion sort for small runs (<32 elements)
- Automatic run reversal when descending order detected
- Galloping mode for efficient merging of unequal-length runs
- Min-run length computed via bit manipulation for optimal stack depth

#### Why It Matters

Real-world music libraries are often partially sorted (albums grouped together, artists alphabetized). TimSort outperforms std::sort by detecting and exploiting this existing order.

**Library Sorting Order**: Artist → Year → Track

### Boyer-Moore-Horspool String Search

**File**: `include/util/BoyerMoore.hpp`

Fast substring matching for global search.

#### Performance

- **Average case**: O(n/m) — sublinear! (2-3x faster than naive search)
- **Worst case**: O(n×m) — extremely rare in practice
- **Space complexity**: O(1) — fixed 256-byte alphabet table (no heap allocations)

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
- Bitwise operations: rotr (rotate right), ch (choose), maj (majority), Σ (sigma), σ (gamma)
- Proper padding and length encoding

#### Application

100 tracks from same album → 1 cached JPEG (99% deduplication)

---

## Kernel-Level Optimizations

### Shared Memory Image Transmission

**File**: `src/ui/ImageRenderer.cpp`

Uses `/dev/shm` (RAM filesystem) for Kitty protocol artwork.

#### Fast Path (Kitty/WezTerm)

```
Write JPEG → /dev/shm/ouroboros-art-XXXXXX → Send file path to terminal → Delete
```

#### Slow Path (Ghostty, older terminals)

```
Base64 encode (33% size increase) → Send full data via stdout
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
- Hash artwork bytes → unique identifier
- 100 tracks from same album → 1 cached JPEG
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
   - 2×2 pixel per character

**Automatic Detection**:
- Query-based capability detection (DA1 responses, Kitty graphics query)
- Environment variable checks (`KITTY_WINDOW_ID`, `TERM_PROGRAM`, `TERM`)
- Fallback cascade (Kitty → Sixel → Unicode)
- Manual override via `OUROBOROS_IMAGE_PROTOCOL`

---

## Unicode Support

**File**: `include/util/UnicodeUtils.hpp`

Full Unicode normalization for case-insensitive search and sorting using ICU (International Components for Unicode).

### normalize_for_search()

**Transliteration Chain**: `NFD → [:Nonspacing Mark:] Remove → NFC → Latin-ASCII`

**Examples**:
- "Björk" → "bjork"
- "José" → "jose"
- "Motörhead" → "motorhead"

**Purpose**: Enables ASCII search input to match international artist names.

### case_insensitive_compare()

**Uses ICU's foldCase()**:
- Proper Unicode comparison
- Groups "BUTTHOLE SURFERS", "Butthole Surfers", "butthole surfers" correctly
- Handles Turkish İ/i, German ß, etc.

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
| **Snapshot Read** | O(1) | <1μs | Lock-free atomic pointer load with acquire semantics |
| **UI Render (30 FPS)** | O(widgets) | ~33ms/frame | Only redraws on state change, canvas diffing |
| **TimSort** | O(n) – O(n log n) | ~10ms for 10K tracks | O(n) for pre-sorted data, binary insertion for runs <32 |
| **Boyer-Moore Search** | O(n/m) avg, O(nm) worst | ~1ms for 10K tracks | Sublinear: scans ~3,300 chars instead of 1M |
| **SHA-256 Hash** | O(data_size) | ~1ms for 5MB JPEG | 512-bit chunks, 64 rounds per chunk |
| **Artwork Cache Lookup** | O(1) | <1μs | `unordered_map` with SHA-256 keys |
| **Image Decode (JPEG)** | Async | 150-250ms for 32 images | Parallel pool (4-16 threads), stb_image + stb_resize |
| **LRU Cache Hit** | O(1) | <1μs | 250-entry ImageRenderer cache, hash-based lookup |
| **Canvas Draw** | O(width×height) | ~500μs for 120×40 | Linear buffer, UTF-8 aware, ANSI escape parsing |
| **FlexLayout Compute** | O(items×iterations) | ~100μs for 8 widgets | Typically 2-3 constraint iterations |
| **EventBus Publish** | O(subscribers) | ~10μs | Typically <10 subscribers per event type |

### Real-World Benchmarks

**Test System**: Ryzen 7 5800X, 721-track library

- **Cold start** (no cache): 850ms to first render
- **Warm start** (TIER 0 hit): 95ms to first render
- **Album grid scroll**: 30 FPS sustained with 32 visible artworks
- **Search latency**: <5ms per keystroke (real-time filtering)

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
- **Mutex Discipline**: `std::lock_guard` for RAII-style locking, never manual lock/unlock
- **Condition Variables**: Thread wake/sleep for worker pools (no busy-waiting)
- **Atomic Counters**: Lock-free work distribution in parallel algorithms
- **Thread Safety**: All shared state documented with ownership rules

### Error Handling

- **std::optional**: Returns for operations that may fail (cache lookup, file read)
- **Validation First**: Check preconditions before operations (bounds, null checks)
- **Graceful Degradation**: Fallback paths (Sixel → Unicode blocks, cache miss → full scan)
- **Comprehensive Logging**: Every major operation logged for debugging

### Code Organization

- **Single Responsibility**: Each class has one clear purpose
- **Dependency Injection**: Publishers, configs passed to constructors (testable)
- **Interface Segregation**: Abstract base classes (AudioDecoder, Component)
- **Composition Over Inheritance**: Widgets composed of smaller pieces
- **Namespace Hygiene**: Everything in `ouroboros::` with logical subnamespaces

### Performance Discipline

- **Profile-Guided**: Optimizations justified by real-world measurements
- **Algorithm Choice**: Uses theoretically optimal algorithms (TimSort for adaptive sort)
- **System-Level**: Drops to syscalls when standard library insufficient
- **Cache Locality**: Hot paths use contiguous memory (vector over list)
- **Zero-Copy**: Move semantics, shared_ptr, avoid unnecessary clones

### Documentation

- **Detailed Comments**: Algorithm explanations (TIER comments, NOTE blocks)
- **Performance Notes**: Why optimizations were chosen (getdents64 rationale)
- **Ownership Documentation**: Who owns what data, lifetime management
- **Architecture Diagrams**: ASCII art pipeline diagrams in comments

---

## Project Structure

```
ouroboros/
├── src/                      # 47 implementation files (8,103 lines)
│   ├── main.cpp              # Entry point, event loop
│   ├── audio/                # 4 decoders + PipeWire output
│   ├── backend/              # Library, queue, metadata, config, snapshot publisher
│   ├── collectors/           # LibraryCollector, PlaybackCollector threads
│   ├── config/               # Theme and keybind management
│   ├── events/               # EventBus (publish-subscribe)
│   ├── model/                # Snapshot, Track, PlayerState data models
│   ├── ui/                   # Terminal, Canvas, Renderer, widgets, FlexLayout
│   └── util/                 # TimSort, BoyerMoore, DirectoryScanner, Logger, ImageDecoderPool
├── include/                  # 50 header files (2,719 lines) mirroring src/
├── tests/                    # New C++ test framework
│   ├── framework/            # SimpleTest.hpp (custom test runner)
│   ├── unit/                 # TimSort, BoyerMoore, ArtworkHasher tests
│   └── integration/          # Metadata pipeline tests
├── config/                   # Example configuration files
│   └── ouroboros.toml.example
├── vendor/                   # Third-party libraries (stb_image, etc.)
├── CMakeLists.txt            # Build configuration
└── Makefile                  # Convenience wrapper
```

### Code Statistics

- **Total Lines**: ~10,822 (8,103 implementation + 2,719 headers)
- **Source Files**: 47 `.cpp` files
- **Header Files**: 50 `.hpp` files
- **Audio Decoders**: 4 (MP3, FLAC, OGG, WAV)
- **Image Protocols**: 4 (Kitty, Sixel, iTerm2, Unicode blocks)
- **UI Widgets**: 9 (Browser, Queue, NowPlaying, Controls, StatusBar, SearchBox, AlbumBrowser, DirectoryBrowser, HelpOverlay)
- **Background Threads**: 4+ (Main, LibraryCollector, PlaybackCollector, ArtworkLoader, ImageDecoderPool workers)
- **Test Files**: 3 suites (unit tests, core tests, integration tests)

---

## Design Patterns

### Singleton Pattern
- `ImageRenderer` - Global image rendering state
- `ArtworkLoader` - Global artwork loading coordinator
- `EventBus` - Global event pub/sub system
- `Terminal` - Global terminal state

### Factory Pattern
- Decoder creation based on audio format (MP3/FLAC/OGG/WAV)

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
