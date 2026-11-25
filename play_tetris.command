#!/usr/bin/env bash
cd "/Users/renseven/.gemini/antigravity/scratch/thrum" || exit 1
if [ ! -x "./tetris" ]; then
    ./thrumc build examples/tetris.thrum -o tetris || exit 1
fi
./tetris
echo ""
echo "Session ended. Press Enter to close..."
read -r _
