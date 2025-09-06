#!/bin/bash

# Target directory (Videos folder in home)
TARGET_DIR="$HOME/ath/Videos"

# Create target directory if it doesn't exist
mkdir -p "$TARGET_DIR"

# Move session*.mp4 files to the Videos folder
echo "📦 Moving session*.mp4 files to $TARGET_DIR"
mv session*.mp4 "$TARGET_DIR" 2>/dev/null

# Delete any remaining .mp4 files
echo "🧹 Deleting leftover .mp4 files in $(pwd)"
find . -maxdepth 1 -type f -name "*.mp4" -delete

echo "✅ Cleanup complete."
