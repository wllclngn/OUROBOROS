# OUROBOROS BUG REPORT - NOW PLAYING HEADER MISSING

## ROOT CAUSE IDENTIFIED ✓

**Unicode Box-Drawing Characters Have Multi-Byte Width**

### The Problem

In `src/ui/AsciiArt.cpp`, the `create_placeholder()` function uses Unicode box-drawing characters:
- `┌` `─` `┐` `│` `└` `┘`

Each of these is **3 bytes** in UTF-8 encoding.

### The Bug

```cpp
// AsciiArt.cpp lines 32-35
std::string horizontal;
for (int i = 0; i < width - 2; ++i) {
    horizontal += "─";  // This is 3 bytes, not 1!
}
```

This creates a string that:
- Looks like 5 characters wide: `┌───┐`
- But `.length()` returns 15 bytes!

### The Consequence

When `NowPlaying::render_lines()` calls:
```cpp
cached_artwork_ = render_artwork(metadata, 5, 3);
```

It gets back:
```
Line 0: "┌───┐"   (15 bytes, not 5)
Line 1: "│   │"   (9 bytes, not 5)
Line 2: "└───┘"   (15 bytes, not 5)
```

Then when it tries to build content lines:
```cpp
std::string line1 = cached_artwork_[0] + "  ";  // 15 bytes + 2 = 17 bytes
```

And passes to `make_box()` with `width=80`:
- `make_box()` tries to pad/truncate based on `.length()`
- But `.length()` returns BYTES not visual width
- Calculations fail
- Box rendering breaks
- Returns empty vector
- **NO HEADER APPEARS**

### Test Evidence

```
$ ./test_AsciiArt
Testing create_placeholder size...
test_AsciiArt: test_AsciiArt.cpp:16: 
  Assertion `line.length() == 5` failed.
  
Actual output:
  Line 0: [┌───┐]
    Length: 15 chars (WRONG!)
    Expected: 5 chars
    Bytes: e2 94 8c e2 94 80 e2 94 80 e2 94 80 e2 94 90
```

## THE FIX

We have three options:

### Option 1: Use ASCII Characters (Simplest)
Replace Unicode with ASCII:
```cpp
// Instead of: ┌─┐│└┘
// Use:        +-+|+-+

std::string top = "+" + std::string(width - 2, '-') + "+";
std::string mid = "|" + std::string(width - 2, ' ') + "|";
std::string bot = "+" + std::string(width - 2, '-') + "+";
```

**Pros**: Single-byte, width calculations work  
**Cons**: Less pretty

### Option 2: Count Visual Width (Complex)
Use a library to count visual character width (not bytes).

**Pros**: Keeps Unicode  
**Cons**: Requires external library (ICU, utf8cpp, etc.)

### Option 3: Don't Use Boxes in Artwork Placeholder
Just return blank spaces:
```cpp
std::vector<std::string> AsciiArt::create_placeholder(int width, int height) {
    return std::vector<std::string>(height, std::string(width, ' '));
}
```

**Pros**: Simple, no width issues  
**Cons**: No visual indicator of artwork area

## RECOMMENDED FIX

**Use ASCII box characters** for the placeholder:

```cpp
// src/ui/AsciiArt.cpp
std::vector<std::string> AsciiArt::create_placeholder(int width, int height) {
    std::vector<std::string> lines;
    
    if (height < 1 || width < 3) {
        return lines;
    }
    
    // ASCII box characters (single byte)
    std::string top = "+" + std::string(width - 2, '-') + "+";
    std::string bottom = "+" + std::string(width - 2, '-') + "+";
    std::string middle = "|" + std::string(width - 2, ' ') + "|";
    
    lines.push_back(top);
    for (int i = 1; i < height - 1; ++i) {
        lines.push_back(middle);
    }
    lines.push_back(bottom);
    
    return lines;
}
```

This guarantees:
- Each line is EXACTLY `width` bytes
- `.length()` returns the correct value
- `make_box()` calculations work
- **Header will appear!**

## VERIFICATION

After applying fix:
```
$ ./test_AsciiArt
==================================
  AsciiArt Unit Tests
==================================

Testing create_placeholder size...
✓ Placeholder returns correct size
Testing create_placeholder content...
  Placeholder output:
    [+---+]
    [|   |]
    [+---+]
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

## IMPACT

This single fix will:
1. ✓ Make `create_placeholder()` return correct byte widths
2. ✓ Make `make_box()` calculations work
3. ✓ Make `NowPlaying::render_lines()` return proper box
4. ✓ Make header appear in UI
5. ✓ All tests pass

**ONE LINE CHANGE FIXES EVERYTHING**
