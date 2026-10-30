#!/bin/sh
# Builds the native simulator, runs logic tests, and renders a preview GIF.
set -e
cd "$(dirname "$0")"
c++ -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -I../firmware/lib/bearcore/src \
  sim.cpp ../firmware/lib/bearcore/src/bearcore.cpp -o sim.bin
./sim.bin test
./sim.bin frames demo.rgb
uv run -q --with pillow python render_gif.py demo.rgb demo.gif
rm demo.rgb
