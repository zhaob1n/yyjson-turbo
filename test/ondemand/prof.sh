#!/bin/sh
# prof.sh BIN DATA_DIR FILTER [impl=0] [iters]  (sampling profile; env: LLVM_SYMBOLIZER, CPU (2))
OD_PROF=${4:-0} OD_PROF_IT=${5:-2000} taskset -c ${CPU:-2} $1 $2 1 "$3" && sort ips.txt | uniq -c | awk '{print $1, "0x"$2}' > top.txt
awk '{print $2}' top.txt | ${LLVM_SYMBOLIZER:-llvm-symbolizer} --obj=$1 --output-style=GNU -f --no-inlines | paste - - > sym.txt
paste -d' ' top.txt sym.txt | awk '{n=split($4,a,"/"); fn[$3]+=$1; ln[$3" "a[n]]+=$1; t+=$1} END{for(k in fn) printf "F %5.1f%% %s\n",100*fn[k]/t,k; for(k in ln) printf "L %5.1f%% %s\n",100*ln[k]/t,k}' | sort -k1,1 -k2rn > prof.out
grep ^F prof.out | head -12; grep ^L prof.out | head -${6:-25}
