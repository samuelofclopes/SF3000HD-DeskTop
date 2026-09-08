#!/bin/sh
set -e
cd ~/sf3000
CC=$(find -L "$HOME" -maxdepth 8 -name mips-mti-linux-gnu-gcc -path "*opt/ext-toolchain/bin*" 2>/dev/null | head -n1)
[ -n "$CC" ] || { echo "ERRO: compilador nao encontrado sob \$HOME (procura: */opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc)"; echo "       extrai a toolchain dentro do \$HOME e corre relocate-sdk.sh"; exit 1; }
SDL=$HOME/sf3000/sdl
FLAGS="-O2 -Wall -EL -static -I$SDL/include/SDL -I$SDL/include/freetype2 -I."
LIBS="$SDL/lib/libSDL_ttf.a $SDL/lib/libSDL_image.a $SDL/lib/libSDL.a $SDL/lib/libfreetype.a -lpthread -lm"

echo "CC: $CC"
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


file myapp demo paint myterm mykeyboard
md5sum myapp demo paint myterm mykeyboard
echo "  myapp     -> cubegm/desktop"
echo "  demo      -> cubegm/wm/demo"
echo "  paint     -> cubegm/wm/paint"
echo "  myterm    -> cubegm/wm/myterm"
echo "  mykeyboard-> cubegm/wm/mykeyboard"
