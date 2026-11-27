#!/bin/bash
# Build the repkv module out of tree against a built chimera tree.
# Run inside the chimera dev image (same compiler and libraries as the tree).
# usage: build.sh [chimera source dir] [chimera build dir]
set -eu
SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BLD=${2:-$SRC/build/Release}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/build
mkdir -p "$OUT"

cc -DCHIMERA_HAVE_URCU -DURCU_API_MAP -D_LGPL_SOURCE -DXXH_INLINE_ALL -DOTEL_TRACING=0 \
   -I"$SRC/ext/libevpl/ext/prometheus-c" \
   -I"$SRC/ext/libevpl/ext/prometheus-c/ext/stopwatch" \
   -I"$SRC/3rdparty" \
   -I"$SRC/ext/libevpl/include" \
   -I"$SRC/ext/libevpl/ext/oteltracing-c" \
   -I"$SRC/src" \
   -O3 -DNDEBUG -std=gnu11 -fPIC -fno-omit-frame-pointer -g -Wall -Werror \
   -Wno-unused-function -fvisibility=hidden -march=armv8-a \
   -c "$HERE/repkv.c" -o "$OUT/repkv.o"

cc -shared -Wl,--no-as-needed -o "$OUT/libchimera_vfs_repkv.so" "$OUT/repkv.o" \
   -Wl,-rpath,"$BLD/src/vfs:$BLD/src/common:$BLD/ext/libevpl/src/core:$BLD/ext/libevpl/ext/prometheus-c" \
   "$BLD/src/vfs/libchimera_vfs.so" "$BLD/src/common/libchimera_common.so" \
   "$BLD/ext/libevpl/src/core/libevpl.so" -ljansson -lpthread

ls -la "$OUT/libchimera_vfs_repkv.so"
