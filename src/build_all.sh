#!/bin/sh
set -e
cd ~/sf3000
CC=/home/m/Downloads/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc
SDL=$HOME/sf3000/sdl
FLAGS="-O2 -Wall -EL -static -I$SDL/include/SDL -I$SDL/include/freetype2 -I."
LIBS="$SDL/lib/libSDL_ttf.a $SDL/lib/libSDL_image.a $SDL/lib/libSDL.a $SDL/lib/libfreetype.a -lpthread -lm"

echo "[1/5] mywm (compositor)..."
 $CC $FLAGS mywm.c -o myapp $LIBS
echo "EXIT: $?"

echo "[2/5] demo..."
 $CC $FLAGS demo.c myapp_lib.c -o demo $LIBS
echo "EXIT: $?"

echo "[3/5] paint..."
 $CC $FLAGS paint.c myapp_lib.c -o paint $LIBS
echo "EXIT: $?"

echo "[4/5] myterm..."
 $CC $FLAGS myterm.c myapp_lib.c -o myterm $LIBS
echo "EXIT: $?"

echo "[5/5] mykeyboard..."
 $CC $FLAGS mykeyboard.c myapp_lib.c -o mykeyboard $LIBS
echo "EXIT: $?"

echo "--- verificação ---"
file mywm demo paint myterm mykeyboard
md5sum mywm demo paint myterm mykeyboard
echo "copiar (PROTOCOLO v4 — TODOS juntos!):"
echo "  mywm      -> cubegm/myapp"
echo "  demo      -> cubegm/wm/demo"
echo "  paint     -> cubegm/wm/paint"
echo "  myterm    -> cubegm/wm/myterm"
echo "  mykeyboard-> cubegm/wm/mykeyboard"
echo "comparar no cartão:"
echo "  md5sum /mnt/sdcard/cubegm/myapp /mnt/sdcard/cubegm/wm/*"