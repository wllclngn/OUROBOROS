# OUROBOROS Test Suite

## Overview

This test suite provides comprehensive testing for the OUROBOROS music player, focusing on the UI rendering pipeline and metadata extraction that powers the NOW PLAYING header.

## Test Files

### Unit Tests

1. **test_AsciiArt.cpp** - Tests the ASCII art generation system
   - `create_placeholder()` size validation
   - Box drawing character rendering
   - Edge case handling (negative sizes, zero dimensions)

2. **test_MetadataParser.cpp** - Tests metadata extraction
   - Basic filename parsing
   - EnhancedMetadata structure
   - Folder artwork search (`find_folder_artwork()`)
   - Embedded artwork extraction stubs

3. **test_NowPlaying.cpp** - Tests the NOW PLAYING widget
   - Rendering with no track
   - Rendering with track metadata
   - Various terminal widths (40, 80, 120 columns)
   - **This test will reveal if NowPlaying crashes on empty artwork**

### Integration Test

4. **test_Integration.cpp** - Tests the complete pipeline
   - Creates fake music file and cover.jpg
   - Parses metadata (should find artwork)
   - Converts to ASCII art
   - Renders in NowPlaying widget
   - Verifies final output
   - **This shows the EXACT failure point in the chain**

## Quick Start

### Option 1: Automatic (Recommended)

```bash
cd /path/to/OUROBOROS-PLAYER
chmod +x run_tests.sh
./run_tests.sh
```

This will:
1. Create `tests/` directory
2. Copy all test files
3. Build all tests
4. Run them in sequence
5. Show detailed results

### Option 2: Manual

```bash
cd /path/to/OUROBOROS-PLAYER
mkdir -p tests
cp test_*.cpp tests/
cp CMakeLists_Tests.txt tests/CMakeLists.txt
cd tests
mkdir build && cd build
cmake ..
make -j4
./test_AsciiArt
./test_MetadataParser
./test_NowPlaying
./test_Integration
```

## Expected Findings

### If Tests Pass ✓

The code is working correctly. The UI issue is elsewhere (possibly threading or initialization).

### If test_NowPlaying Fails ✗

**Most likely scenario**: NowPlaying is crashing because:
- `cached_artwork_` vector is empty
- Accessing `cached_artwork_[0]`, `[1]`, `[2]` causes out-of-bounds access
- Returns empty vector → no header in UI

### If test_MetadataParser Fails ✗

Metadata extraction is broken:
- `find_folder_artwork()` not finding cover.jpg
- File reading issues
- Path resolution problems

### If test_Integration Fails ✗

Shows EXACTLY where the pipeline breaks:
```
STEP 1: ✓ Test files created
STEP 2: ✓ Metadata parsed
STEP 3: ✗ ASCII art returned EMPTY  ← FOUND THE BUG
STEP 4: ✗ NowPlaying crashed        ← CONSEQUENCE
```

## What We'll Learn

These tests will definitively answer:

1. **Does `AsciiArt::create_placeholder()` return the right size?**
   - If NO → that's the bug
   - If YES → problem is elsewhere

2. **Does `MetadataParser::find_folder_artwork()` actually find images?**
   - If NO → folder search is broken
   - If YES → artwork is being found

3. **Does NowPlaying handle missing/empty artwork?**
   - If NO → needs safety checks
   - If YES → make_box() is the issue

4. **Where exactly does the chain break?**
   - Integration test pinpoints the exact failure

## Test Output Format

```
==================================
  AsciiArt Unit Tests
==================================

Testing create_placeholder size...
✓ Placeholder returns correct size
Testing create_placeholder content...
  Placeholder output:
    [┌───┐]
    [│   │]
    [└───┘]
✓ Placeholder has box drawing chars
Testing create_placeholder edge cases...
  Width=2, Height=3 -> 0 lines
  Width=5, Height=0 -> 0 lines
  Width=-1, Height=-1 -> 0 lines
✓ Edge cases handled

==================================
  ALL TESTS PASSED ✓
==================================
```

## Debugging Failed Tests

If a test fails, it will:
1. Print `TEST FAILED: <reason>`
2. Show exact line where assertion failed
3. Print relevant debug output
4. Return exit code 1

Example:
```
Testing NowPlaying with a track...
  Rendered 0 lines
  ✗ FAILED: render_lines returned EMPTY vector!
  This is the BUG - it's crashing or returning nothing
TEST FAILED: Should render box with track info
```

## Integration with Main Build

These tests are **separate** from the main OUROBOROS build. They:
- Compile independently
- Link against OUROBOROS source files
- Don't affect the main binary
- Can be run anytime

## Next Steps After Testing

Based on test results:

### If artwork vector is empty:
→ Fix `AsciiArt::create_placeholder()` to always return correct size

### If folder search fails:
→ Fix `find_folder_artwork()` path resolution

### If NowPlaying crashes:
→ Add safety: `while (cached_artwork_.size() < 3) { ... }`

### If make_box() fails:
→ Debug `Formatting.cpp:make_box()` function

## Files Included

- `test_AsciiArt.cpp` - AsciiArt unit tests
- `test_MetadataParser.cpp` - MetadataParser unit tests  
- `test_NowPlaying.cpp` - NowPlaying widget unit tests
- `test_Integration.cpp` - Full pipeline integration test
- `CMakeLists_Tests.txt` - Build configuration
- `run_tests.sh` - Automatic test runner
- `TEST_SUITE_README.md` - This file

## Requirements

- C++23 compiler
- CMake 3.20+
- Filesystem library (libstdc++fs)

## Running Individual Tests

After building:
```bash
cd tests/build
./test_AsciiArt        # Just ASCII art tests
./test_MetadataParser  # Just metadata tests
./test_NowPlaying      # Just widget tests
./test_Integration     # Full pipeline test
```

---

**Result**: We'll know EXACTLY what's broken and where to fix it! 🎯
