#!/bin/bash
# §607 bisect: compile the listed optimizing TUs at -O0, ALL OTHER optimizing/*.cc at -O2, relink.
# usage: bisect_o0.sh "tu1 tu2 ..."   (basenames without .cc)
set -e
cd /home/dspfac/art-latest
O0LIST="$1"
FLAGS_O0=$(cat /tmp/claude-1000/-home-dspfac-openharmony/4a658cc2-2a6f-46a4-8b26-7a072da2b538/scratchpad/o0flags.txt)
FLAGS_O2=$(echo "$FLAGS_O0" | sed 's/-O0/-O2/')
n0=0; n2=0
for src in $(ls /home/dspfac/aosp-art-15/compiler/optimizing/*.cc | grep -vE '_test\.cc|_fuzzer\.cc|graph_color|_sve\.cc|reference_type_propagation\.cc'); do
  t=$(basename "$src" .cc); obj=build-ohos-arm64/compiler/optimizing/$t.o
  if echo " $O0LIST " | grep -q " $t "; then
    $FLAGS_O0 -c "$src" -o "$obj" 2>/dev/null && n0=$((n0+1))
  else
    $FLAGS_O2 -c "$src" -o "$obj" 2>/dev/null && n2=$((n2+1))
  fi
done
bash /home/dspfac/bridge-build-arm64/build_libart_so_arm64.sh >/dev/null 2>&1
echo "bisect: O0=$n0 O2=$n2 -> $(md5sum /home/dspfac/bridge-build-arm64/out/libart.so | cut -c1-8)"
