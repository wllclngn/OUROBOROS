#!/bin/bash

# Verification script for PREVIOUS button artwork fix
# This script analyzes the debug log to verify the fix is working

LOG_FILE="/tmp/ouroboros_debug.log"

echo "============================================"
echo "OUROBOROS PREVIOUS Button Fix Verification"
echo "============================================"
echo

# Check if log file exists
if [ ! -f "$LOG_FILE" ]; then
    echo "❌ Error: Log file not found at $LOG_FILE"
    echo "   Please run ouroboros first to generate logs"
    exit 1
fi

echo "📋 Analyzing log file: $LOG_FILE"
echo

# Count track changes
echo "1. Track Navigation Events:"
echo "   -------------------------"
NEXT_COUNT=$(grep -c "NextTrack: Advanced from index" "$LOG_FILE" 2>/dev/null || echo "0")
PREV_COUNT=$(grep -c "PrevTrack: Went back from index" "$LOG_FILE" 2>/dev/null || echo "0")
TRACK_CHANGES=$(grep -c "NowPlaying: Track changed from" "$LOG_FILE" 2>/dev/null || echo "0")
echo "   • NEXT button pressed: $NEXT_COUNT times"
echo "   • PREVIOUS button pressed: $PREV_COUNT times"
echo "   • Total track changes detected: $TRACK_CHANGES times"
echo

# Count image operations
echo "2. Image Renderer Operations:"
echo "   ---------------------------"
UPLOAD_COUNT=$(grep -c "ImageRenderer: UPLOAD command for image_id=" "$LOG_FILE" 2>/dev/null || echo "0")
PLACEMENT_COUNT=$(grep -c "ImageRenderer: PLACEMENT command for image_id=" "$LOG_FILE" 2>/dev/null || echo "0")
TRANSMITTED_SIZE=$(grep "transmitted_ids_ size" "$LOG_FILE" | tail -1 | sed 's/.*set_size=\([0-9]*\).*/\1/' 2>/dev/null || echo "0")

echo "   • Upload commands (a=T): $UPLOAD_COUNT"
echo "   • Placement commands (a=p): $PLACEMENT_COUNT"
echo "   • Transmitted cache size: $TRANSMITTED_SIZE images"
echo

# Analyze the fix
echo "3. Fix Verification:"
echo "   -----------------"

if [ "$PREV_COUNT" -eq 0 ]; then
    echo "   ⚠️  WARNING: No PREVIOUS button presses detected"
    echo "      Please test by pressing PREVIOUS during playback"
elif [ "$PLACEMENT_COUNT" -eq 0 ]; then
    echo "   ❌ FAILED: No placement commands found"
    echo "      The fix may not be working correctly"
    echo "      Expected placement commands when pressing PREVIOUS"
elif [ "$PLACEMENT_COUNT" -gt 0 ] && [ "$PREV_COUNT" -gt 0 ]; then
    echo "   ✅ SUCCESS: Placement commands detected!"
    echo "      The PREVIOUS button fix appears to be working"
    echo "      Placement commands: $PLACEMENT_COUNT"
    echo "      PREVIOUS presses: $PREV_COUNT"
else
    echo "   ℹ️  Insufficient data to verify fix"
fi
echo

# Show recent image operations
echo "4. Recent Image Operations (last 10):"
echo "   -----------------------------------"
grep -E "(UPLOAD command|PLACEMENT command)" "$LOG_FILE" | tail -10 | while read line; do
    if echo "$line" | grep -q "UPLOAD"; then
        echo "   🔼 UPLOAD: $(echo $line | sed 's/.*image_id=\([0-9]*\).*/id=\1/')"
    elif echo "$line" | grep -q "PLACEMENT"; then
        echo "   📍 PLACE:  $(echo $line | sed 's/.*image_id=\([0-9]*\).*/id=\1/')"
    fi
done
echo

# Show track change correlation
echo "5. Track Change → Artwork Request Correlation:"
echo "   --------------------------------------------"
grep -A2 "NowPlaying: Track changed from" "$LOG_FILE" | tail -20 | grep -E "(Track changed|Requested artwork)" | while read line; do
    if echo "$line" | grep -q "Track changed"; then
        TRACK=$(echo "$line" | sed "s/.*to '\(.*\)'/\1/")
        echo "   🎵 Track: ...${TRACK: -40}"
    elif echo "$line" | grep -q "Requested artwork"; then
        echo "      → Artwork requested ✓"
    fi
done | tail -10
echo

echo "============================================"
echo "Manual Testing Instructions:"
echo "============================================"
echo "1. Start ouroboros: ./build/ouroboros"
echo "2. Queue 3-5 tracks and start playback"
echo "3. Press NEXT to advance to track 2"
echo "4. Press NEXT again to advance to track 3"
echo "5. Press PREVIOUS to go back to track 2"
echo "6. Verify artwork displays for track 2"
echo "7. Press PREVIOUS again to go back to track 1"
echo "8. Verify artwork displays for track 1"
echo "9. Run this script again to analyze results"
echo
echo "Expected behavior:"
echo "  • First time playing a track → UPLOAD command"
echo "  • Returning to a track via PREVIOUS → PLACEMENT command"
echo "============================================"
