# OUROBOROS - TERMINAL THEME ONLY FIX

## What This Does

**Removes ALL hardcoded colors.** Everything now inherits from YOUR terminal theme.

- ✅ Progress bars: grey/dim (your terminal's dim color)
- ✅ Highlights: grey/dim (your terminal's dim color)  
- ✅ Current track: bold (your terminal's bold color)
- ✅ NO blue, NO hardcoded RGB values
- ✅ Looks clean and matches your terminal

## Option 1: Automatic Fix (Recommended)

Run the script:

```bash
cd /path/to/OUROBOROS-PLAYER
bash apply_terminal_theme.sh
```

Then rebuild:

```bash
cd build
make clean
make -j4
./ouroboros ~/Music
```

## Option 2: Manual Fix

### Step 1: Replace Theme.cpp

Replace `src/config/Theme.cpp` with `Theme_TERMINAL_ONLY.cpp`:

```bash
cp Theme_TERMINAL_ONLY.cpp src/config/Theme.cpp
```

### Step 2: Update All Widgets

Replace these files:
- `Browser_TERMINAL.cpp` → `src/ui/widgets/Browser.cpp`
- `Queue_TERMINAL.cpp` → `src/ui/widgets/Queue.cpp`

### Step 3: Rebuild

```bash
cd build
make clean
make -j4
```

## Files Included

1. **apply_terminal_theme.sh** - Automatic fix script
2. **Theme_TERMINAL_ONLY.cpp** - Theme manager with ONLY terminal theme
3. **Browser_TERMINAL.cpp** - Browser widget using terminal theme
4. **Queue_TERMINAL.cpp** - Queue widget using terminal theme
5. **NowPlaying_NO_MAKEBOX.cpp** - Fixed NOW PLAYING header

## What You'll See

```
┌─ NOW PLAYING ──────────────────────────────────────┐
│ +---+  Deftones - locked club                      │
│ |   |  White Pony (2000) • Alternative Metal       │
│ +---+  MP3 320kbps • 44kHz Stereo                  │
└────────────────────────────────────────────────────┘
┌─ Library ────────┐┌─ Queue ───────────────────────┐
│▶ Deftones - lock ││▶ Deftones - infinite [3:22]   │
│  Deftones - cXz  ││  Deftones - cXz      [3:12]   │
└──────────────────┘└───────────────────────────────┘
[51 tracks] │ ▶ [████████░░░░] 0:06 / 3:32 │ Vol: [███░░] 55%
j/k: nav │ Enter: add │ space: pause │ n/p: skip │ q: quit
```

**All colors:** Inherited from YOUR terminal theme!

## Verification

After rebuilding, check:

```bash
./ouroboros ~/Music
```

- Progress bar should be **grey/dim**, NOT blue
- Highlights should be **grey/dim**, NOT coral/orange
- Everything matches your terminal colors

## Restore Backups (If Needed)

The script creates .backup files:

```bash
mv src/config/Theme.cpp.backup src/config/Theme.cpp
mv src/ui/widgets/Browser.cpp.backup src/ui/widgets/Browser.cpp
mv src/ui/widgets/Queue.cpp.backup src/ui/widgets/Queue.cpp
```

Then rebuild.

---

**Result:** Clean, terminal-native interface with NO hardcoded colors!
