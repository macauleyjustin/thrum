#!/usr/bin/env bash
cd "/Users/renseven/.gemini/antigravity/scratch/thrum" || exit 1
if [ -d "./ThrumSolitaire.app" ]; then
    open "./ThrumSolitaire.app"
    exit 0
fi
if [ ! -x "./solitaire" ]; then
    ./thrumc build examples/solitaire.thrum -o solitaire || exit 1
fi
./solitaire
