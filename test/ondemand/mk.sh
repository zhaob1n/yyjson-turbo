#!/bin/sh
# build bench_od: mk.sh OUT [extra cflags]
set -e
H=$(cd "$(dirname "$0")/../.." && pwd)
B=/workspace/yyjson-bench
L="-stdlib=libc++ -L/home/box/.local/llvm/lib/x86_64-unknown-linux-gnu -Wl,-rpath,/home/box/.local/llvm/lib/x86_64-unknown-linux-gnu"
O=${1:-bench_od}
clang -fno-pie -O3 -march=native -Wall -Wextra -g $2 -c $H/src/yyjson.c -o $O.yy.o
clang -fno-pie -O3 -march=native -std=c89 -Wall -Wextra -pedantic -g $2 -c $H/src/yyjson_ondemand.c -o $O.od.o
clang++ -fno-pie -O3 -march=native -std=c++17 -g $2 -I$H/src -I$B/simdjson/singleheader $H/test/ondemand/bench_od.cpp $O.yy.o $O.od.o $B/r7/sj.o $L -no-pie $2 -o $O
