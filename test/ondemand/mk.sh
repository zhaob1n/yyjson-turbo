#!/bin/sh
# build bench_od: mk.sh OUT [extra cflags]
set -e
H=$(cd "$(dirname "$0")/../.." && pwd)
# Environment (defaults in parentheses):
#   SIMDJSON_DIR  simdjson checkout with singleheader/ ($H/../simdjson)
#   SJ_OBJ        cached simdjson object (./sj_all.o)
#   CC, CXX       compilers (clang, clang++)
#   CXXLIB        C++ library flags, e.g. for libc++:
#                 "-stdlib=libc++ -L/path/to/llvm/lib -Wl,-rpath,/path/to/llvm/lib"
#                 (empty: the compiler's default C++ library)
SIMDJSON_DIR=${SIMDJSON_DIR:-$H/../simdjson}
CC=${CC:-clang}
CXX=${CXX:-clang++}
L=${CXXLIB:-}
O=${1:-bench_od}
# simdjson built without -march so runtime dispatch keeps every kernel
# (icelake, haswell, westmere, fallback); SJ_IMPL=haswell then works.
SJ=${SJ_OBJ:-./sj_all.o}
[ -f "$SJ" ] || $CXX -fno-pie -O3 -std=c++17 $L -c $SIMDJSON_DIR/singleheader/simdjson.cpp -o "$SJ"
$CC -fno-pie -O3 -march=native -Wall -Wextra -g $2 -c $H/src/yyjson.c -o $O.yy.o
$CC -fno-pie -O3 -march=native -std=c89 -Wall -Wextra -pedantic -g $2 -c $H/src/yyjson_ondemand.c -o $O.od.o
$CXX -fno-pie -O3 -march=native -std=c++17 -g $2 -I$H/src -I$SIMDJSON_DIR/singleheader $H/test/ondemand/bench_od.cpp $O.yy.o $O.od.o $SJ $L -no-pie $2 -o $O
