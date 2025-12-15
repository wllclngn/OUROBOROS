# OUROBOROS Architecture

## Overview
OUROBOROS is a terminal-based music player with a lock-free, snapshot-based architecture.

## Component Responsibilities

### Backend
- **Library** - Track metadata storage, filesystem scanning with parallel parsing
- **MetadataParser** - Audio file metadata extraction (ID3, Vorbis, FLAC)
- **SnapshotPublisher** - Thread-safe state updates, COW coordination
- **PipeWireClient** - Audio output via PipeWire

### Collectors (Background Threads)
- **LibraryCollector** - Library scanning (once at startup), cache I/O
- **PlaybackCollector** - Audio decoding and playback coordination

### UI
- **Renderer** - Main rendering loop, widget coordination
- **ArtworkLoader** - Artwork loading and caching (owns artwork lifecycle)
- **ImageRenderer** - Image decoding, resizing, terminal protocol encoding
- **Widgets** - UI components (Browser, Queue, NowPlaying, StatusBar, etc.)

## Key Design Patterns

### Copy-On-Write (COW)
- All state updates create new immutable snapshots
- shared_ptr enables cheap copies
- Lock-free reads for UI thread

### Track Representation
- **LibraryState**: Stores full Track objects (single source of truth)
- **QueueState**: Stores indices into LibraryState::tracks (memory efficient)
- Indices enable track reuse and automatic metadata updates

## Data Flow

```
Filesystem → Library::scan_directory() → MetadataParser::parse_file()
                                              ↓
                                         LibraryState
                                              ↓
                                      SnapshotPublisher
                                              ↓
                                      Renderer → Widgets
```

## Threading Model

- **Main thread**: Event loop, rendering, input handling
- **LibraryCollector thread**: Scanning, cache I/O (runs once at startup)
- **PlaybackCollector thread**: Audio decoding
- **ArtworkLoader worker thread**: Image file I/O

## Recent Changes (2025-12-12)

### Metadata Integration
- **Before**: Library scan created minimal tracks, MetadataCollector re-parsed everything
- **After**: Library::scan_directory() parses metadata in parallel (8 workers)
- **Result**: 50% faster, no double I/O, eliminated MetadataCollector

### Architecture Benefits
- Single I/O pass per audio file
- No race conditions between collectors
- Simpler threading model (one fewer thread)
- Predictable behavior (no background polling)
