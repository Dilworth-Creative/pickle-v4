#!/bin/bash
#
# Zero-Copy Verification Test Script
# Tests that DRM PRIME zero-copy path is now enabled
#

echo "=========================================="
echo "Zero-Copy Hardware Decode Test"
echo "=========================================="
echo ""

# Check if video files are provided
if [ $# -lt 1 ]; then
    echo "Usage: $0 <video1.mp4> [video2.mp4]"
    echo ""
    echo "Example:"
    echo "  $0 /path/to/video.mp4"
    echo "  $0 /path/to/video1.mp4 /path/to/video2.mp4"
    echo ""
    exit 1
fi

VIDEO1="$1"
VIDEO2="${2:-$1}"  # Use same video twice if only one provided

# Verify video files exist
if [ ! -f "$VIDEO1" ]; then
    echo "Error: Video file not found: $VIDEO1"
    exit 1
fi

if [ ! -f "$VIDEO2" ]; then
    echo "Error: Video file not found: $VIDEO2"
    exit 1
fi

echo "Test Configuration:"
echo "  Video 1: $VIDEO1"
echo "  Video 2: $VIDEO2"
echo ""

# Test 1: Verify DRM context initialization
echo "=========================================="
echo "Test 1: DRM Context Initialization"
echo "=========================================="
echo "Running with --hw-debug flag to verify DRM context is created..."
echo ""

# Run for 3 seconds and capture output
timeout 3s ./pickle --hw --hw-debug "$VIDEO1" "$VIDEO2" 2>&1 | tee /tmp/pickle_test.log

echo ""
echo "Checking log for success indicators..."
echo ""

# Check for critical success indicators
if grep -q "DRM context initialized - zero-copy enabled" /tmp/pickle_test.log; then
    echo "✓ PASS: DRM context successfully initialized"
else
    echo "✗ FAIL: DRM context initialization not found in logs"
    echo "        Expected: 'DRM context initialized - zero-copy enabled'"
fi

if grep -q "FORMAT CALLBACK INVOKED" /tmp/pickle_test.log; then
    echo "✓ PASS: Format callback was invoked"
else
    echo "✗ FAIL: Format callback was NOT invoked"
    echo "        Expected: 'FORMAT CALLBACK INVOKED' in logs"
fi

if grep -q "Selected: DRM_PRIME" /tmp/pickle_test.log; then
    echo "✓ PASS: DRM_PRIME format selected"
else
    echo "✗ FAIL: DRM_PRIME format not selected"
    echo "        Expected: 'Selected: DRM_PRIME'"
fi

if grep -q "Using external texture zero-copy path" /tmp/pickle_test.log; then
    echo "✓ PASS: Zero-copy rendering path active"
else
    echo "⚠ WARNING: Zero-copy rendering path not confirmed"
    echo "           This may indicate GPU doesn't support external textures"
fi

echo ""
echo "=========================================="
echo "Test 2: Performance Comparison"
echo "=========================================="
echo "Running with --timing to measure performance..."
echo ""

# Test with hardware decode
echo "Testing Hardware Decode (--hw)..."
timeout 5s ./pickle --hw --timing "$VIDEO1" "$VIDEO2" 2>&1 | grep -E "(Decode|Upload|Render|Total)" | tee /tmp/pickle_hw_timing.log

echo ""
echo "Full test log saved to: /tmp/pickle_test.log"
echo ""
echo "=========================================="
echo "Expected Results (with zero-copy working):"
echo "=========================================="
echo "✓ DRM context initialized"
echo "✓ Format callback invoked"
echo "✓ DRM_PRIME selected"
echo "✓ Video 1 texture upload: <2ms (was 12-27ms)"
echo "✓ Total frame time: <17ms (was 31ms)"
echo ""
echo "If all checks passed, zero-copy is working!"
echo "=========================================="
