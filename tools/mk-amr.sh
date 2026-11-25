#!/bin/bash
# Build Apache-2.0 opencore-amr (AMR-NB enc/dec + AMR-WB dec) and vo-amrwbenc (AMR-WB enc)
# static libraries for iOS arm64 into userland/obj/amr-arm64/ for userland/src/volte_rtp.c.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/userland/obj/amr-arm64"
WORK="${TMPDIR:-/tmp}/hakios-amr-build"
SDK="${SDK:-$ROOT/xnu-deps/sdk}"

if [ ! -d "$SDK" ]; then
    echo "mk-amr: iOS SDK not found at $SDK" >&2
    exit 1
fi

CC_BIN="${CC:-clang}"
CXX_BIN="${CXX:-clang++}"
AR_BIN="${AR:-llvm-ar}"
RANLIB_BIN="${RANLIB:-llvm-ranlib}"
TARGET="-target arm64-apple-ios16.5 -isysroot $SDK"

mkdir -p "$WORK" "$OUT/lib" "$OUT/include"
cd "$WORK"

OPENCORE_VER=0.1.6
VOAMR_VER=0.1.3
[ -f "opencore-amr-${OPENCORE_VER}.tar.gz" ] || \
    curl -fsSL "https://downloads.sourceforge.net/project/opencore-amr/opencore-amr/opencore-amr-${OPENCORE_VER}.tar.gz" -o "opencore-amr-${OPENCORE_VER}.tar.gz"
[ -f "vo-amrwbenc-${VOAMR_VER}.tar.gz" ] || \
    curl -fsSL "https://downloads.sourceforge.net/project/opencore-amr/vo-amrwbenc/vo-amrwbenc-${VOAMR_VER}.tar.gz" -o "vo-amrwbenc-${VOAMR_VER}.tar.gz"

rm -rf "opencore-amr-${OPENCORE_VER}" "vo-amrwbenc-${VOAMR_VER}"
tar xf "opencore-amr-${OPENCORE_VER}.tar.gz"
tar xf "vo-amrwbenc-${VOAMR_VER}.tar.gz"

(
    cd "opencore-amr-${OPENCORE_VER}"
    CC="$CC_BIN $TARGET" CXX="$CXX_BIN $TARGET -std=c++11" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    CFLAGS="-O2" CXXFLAGS="-O2 -std=c++11" \
    ./configure --host=aarch64-apple-darwin --prefix="$OUT" --disable-shared --enable-static
    make -j"$(nproc)"
    make install
)

(
    cd "vo-amrwbenc-${VOAMR_VER}"
    mkdir -p "$OUT/include/vo-amrwbenc"
    cp enc_if.h "$OUT/include/vo-amrwbenc/"
    mkdir -p obj
    for f in wrapper.c common/cmnMemory.c amrwbenc/src/*.c; do
        o="obj/$(basename "$f" .c).o"
        $CC_BIN $TARGET -O2 -I. -Iamrwbenc/inc -Icommon/include -c "$f" -o "$o"
    done
    $AR_BIN rcs "$OUT/lib/libvo-amrwbenc.a" obj/*.o
    $RANLIB_BIN "$OUT/lib/libvo-amrwbenc.a"
)

ls -lh "$OUT/lib/libopencore-amrnb.a" "$OUT/lib/libopencore-amrwb.a" "$OUT/lib/libvo-amrwbenc.a"
