#!/bin/bash

# Mastered - Quick Beat Mastering Script
# Usage: ./master.sh your_beat.wav [reference.wav]

set -e

BEAT="$1"
REFERENCE="${2:-reference.wav}"
OUTPUT_DIR="mastered_output"
TIMESTAMP=$(date +%s)

# Check inputs
if [[ ! -f "$BEAT" ]]; then
    echo "❌ Beat file not found: $BEAT"
    echo "Usage: ./master.sh your_beat.wav [reference.wav]"
    exit 1
fi

if [[ ! -f "$REFERENCE" ]]; then
    echo "❌ Reference file not found: $REFERENCE"
    echo "Usage: ./master.sh your_beat.wav [reference.wav]"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Extract filename without extension
BEAT_NAME=$(basename "$BEAT" .wav)
MASTERED="${OUTPUT_DIR}/mastered_${BEAT_NAME}.wav"
ANALYSIS="${OUTPUT_DIR}/analysis_${BEAT_NAME}_${TIMESTAMP}.json"

echo "╔════════════════════════════════════════╗"
echo "║  Mastered - Quick Mastering            ║"
echo "╚════════════════════════════════════════╝"
echo ""
echo "📀 Beat:      $BEAT"
echo "🎯 Reference: $REFERENCE"
echo "💾 Output:    $MASTERED"
echo ""

# Run mastering engine
./build/mastered_cli "$REFERENCE" "$BEAT" "$ANALYSIS"

# Move output to proper location
if [[ -f "mastered_${BEAT_NAME}.wav" ]]; then
    mv "mastered_${BEAT_NAME}.wav" "$MASTERED"
    echo ""
    echo "✅ Mastering complete!"
    echo "📊 Analysis saved: $ANALYSIS"
    echo "🎵 Mastered beat: $MASTERED"
else
    echo "❌ Mastering failed"
    exit 1
fi
