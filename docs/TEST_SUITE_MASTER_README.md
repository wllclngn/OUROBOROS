# OUROBOROS Test Suite - Complete Package

## What This Is

A complete testing framework for diagnosing and fixing the NOW PLAYING header bug in OUROBOROS.

**Instead of guessing, we TEST and FIND the actual problem.**

## Files Included

### Test Files
- `test_AsciiArt.cpp` - Tests ASCII art generation
- `test_MetadataParser.cpp` - Tests metadata extraction
- `test_NowPlaying.cpp` - Tests NOW PLAYING widget rendering
- `test_Integration.cpp` - Tests entire pipeline end-to-end

### Build & Run
- `CMakeLists_Tests.txt` - Build configuration
- `run_tests.sh` - Automatic test runner (builds + runs all tests)
- `setup_tests.sh` - Quick installer

### Documentation
- `TEST_SUITE_README.md` - Detailed test documentation

## Quick Start (60 seconds)

```bash
# 1. Download all files to your OUROBOROS project directory
cd /path/to/OUROBOROS-PLAYER

# 2. Make scripts executable
chmod +x setup_tests.sh run_tests.sh

# 3. Install test suite
./setup_tests.sh

# 4. Run tests
cd tests
./run_tests.sh
```

That's it! The tests will tell you EXACTLY what's broken.

## What The Tests Will Tell You

### Scenario 1: test_AsciiArt FAILS
```
Testing create_placeholder size...
✗ Placeholder returns 0 lines instead of 3
```
**Problem**: `AsciiArt::create_placeholder()` broken  
**Fix**: Fix the size calculation in AsciiArt.cpp

### Scenario 2: test_MetadataParser FAILS
```
Testing find_folder_artwork...
✗ Did NOT find artwork!
```
**Problem**: Folder image search broken  
**Fix**: Fix path resolution in `find_folder_artwork()`

### Scenario 3: test_NowPlaying FAILS
```
Testing NowPlaying with a track...
  Rendered 0 lines
✗ FAILED: render_lines returned EMPTY vector!
```
**Problem**: NowPlaying crashes on empty artwork vector  
**Fix**: Add safety checks before accessing array indices

### Scenario 4: test_Integration FAILS
```
STEP 2: ✓ Metadata parsed
STEP 3: ✗ ASCII art conversion returned EMPTY!
  This would cause NowPlaying to crash on [0], [1], [2] access
```
**Problem**: Shows EXACT step where pipeline breaks  
**Fix**: Fix that specific component

## Manual Installation (if script doesn't work)

```bash
cd /path/to/OUROBOROS-PLAYER

# Create tests directory
mkdir -p tests

# Copy files
cp test_*.cpp tests/
cp CMakeLists_Tests.txt tests/CMakeLists.txt
cp run_tests.sh tests/
cp TEST_SUITE_README.md tests/README.md

# Build and run
cd tests
mkdir build && cd build
cmake ..
make -j4
./test_Integration  # Start with this - shows full pipeline
```

## Why This Approach Works

**Before (what we've been doing):**
1. Make a change
2. Rebuild
3. Run program
4. See if header shows up
5. If not, guess what's wrong
6. Repeat

**After (with tests):**
1. Run tests
2. See EXACTLY what fails and where
3. Fix THAT specific thing
4. Run tests again to verify
5. Done

**No more guessing. No more patches. Actual diagnosis.**

## Expected Timeline

- **Setup**: 2 minutes
- **First test run**: 30 seconds
- **Identify bug**: Immediately (test output shows it)
- **Fix bug**: 5-10 minutes
- **Verify fix**: 30 seconds (re-run tests)

**Total**: ~15 minutes to go from "header not showing" to "bug fixed and verified"

## What Happens After Tests Pass

Once all tests pass:
1. We know all components work in isolation
2. We know the full pipeline works end-to-end
3. Bug is either:
   - In the rendering/threading integration (not the widgets)
   - In how Renderer calls NowPlaying
   - In the main event loop

We can then add debug output to those specific areas.

## Why I Created This

You asked for testing instead of patches. You're absolutely right. This is the professional approach:

✅ Unit tests verify each component  
✅ Integration test verifies the pipeline  
✅ Clear diagnostic output shows exact failures  
✅ Repeatable - run anytime  
✅ No guesswork - actual data  

## Support

If tests fail, the output will show:
- Which test failed
- What it expected
- What it got
- Where in the code the failure occurred

Share that output and we can fix the exact issue.

---

**Let's find the real bug together. Run the tests!** 🔍
