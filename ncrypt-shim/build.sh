#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${1:-$SCRIPT_DIR/dist}"
mkdir -p "$OUT_DIR"

NCRYPT_SRC="$SCRIPT_DIR/ncrypt_shim.c"
NCRYPT_DEF="$SCRIPT_DIR/ncrypt.def"
DINPUT8_SRC="$SCRIPT_DIR/dinput8_shim.c"
DINPUT8_DEF="$SCRIPT_DIR/dinput8.def"

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 && command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "[shims] Building with MinGW-w64 GCC..."
    x86_64-w64-mingw32-gcc -shared -O2 -s "$NCRYPT_SRC" "$NCRYPT_DEF" \
        -o "$OUT_DIR/ncrypt64.dll" \
        -lbcrypt -luser32 -lkernel32 -Wl,--enable-stdcall-fixup
    i686-w64-mingw32-gcc -shared -O2 -s "$NCRYPT_SRC" "$NCRYPT_DEF" \
        -o "$OUT_DIR/ncrypt32.dll" \
        -lbcrypt -luser32 -lkernel32 -Wl,--kill-at
    i686-w64-mingw32-gcc -shared -O2 -s "$DINPUT8_SRC" "$DINPUT8_DEF" \
        -o "$OUT_DIR/dinput8.dll" \
        -luser32 -lkernel32 -Wl,--kill-at
elif command -v clang >/dev/null 2>&1 && [ -d /usr/include/wine/windows ]; then
    echo "[shims] Building with Clang + LLD + Wine headers..."
    clang -target x86_64-windows-gnu -fuse-ld=lld -shared -O2 -fno-builtin \
        -DGOLEY_NOSTDLIB -nostdlib -Wl,-e,DllMain \
        -I/usr/include/wine/msvcrt -I/usr/include/wine/windows \
        "$NCRYPT_SRC" "$NCRYPT_DEF" \
        -L/usr/lib/wine/x86_64-windows -L/usr/lib64/wine/x86_64-windows \
        -lkernel32 -luser32 -lbcrypt \
        -o "$OUT_DIR/ncrypt64.dll"

    WINE32_LIB="/usr/lib/wine/i386-windows"
    [ -d "/usr/lib32/wine/i386-windows" ] && WINE32_LIB="/usr/lib32/wine/i386-windows"

    clang -target i686-windows-gnu -fuse-ld=lld -shared -O2 -fno-builtin \
        -DGOLEY_NOSTDLIB -nostdlib -Wl,-e,_DllMain@12 -Wl,--kill-at \
        -I/usr/include/wine/msvcrt -I/usr/include/wine/windows \
        "$NCRYPT_SRC" "$NCRYPT_DEF" \
        -L"$WINE32_LIB" \
        -lkernel32 -luser32 -lbcrypt \
        -o "$OUT_DIR/ncrypt32.dll"

    clang -target i686-windows-gnu -fuse-ld=lld -shared -O2 -fno-builtin \
        -DGOLEY_NOSTDLIB -nostdlib -Wl,-e,_DllMain@12 -Wl,--kill-at \
        -I/usr/include/wine/msvcrt -I/usr/include/wine/windows \
        "$DINPUT8_SRC" "$DINPUT8_DEF" \
        -L"$WINE32_LIB" \
        -lkernel32 -luser32 \
        -o "$OUT_DIR/dinput8.dll"
else
    echo "Error: Neither mingw-w64-gcc nor (clang + lld + wine headers) found!" >&2
    exit 1
fi

echo "[shims] Successfully built:"
ls -lh "$OUT_DIR/ncrypt64.dll" "$OUT_DIR/ncrypt32.dll" "$OUT_DIR/dinput8.dll"
