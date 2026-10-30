#!/usr/bin/env bash
# Worst-case stress runs with default settings; log to bench/stress.log.
cd "$(dirname "$0")/.."
LOG=bench/stress.log; W=bench/validate_work; mkdir -p "$W"; : > "$LOG"
enc() { python3 -c "import sys;p,k=sys.argv[1:];print(''.join(chr((ord(c)-97+ord(k[i%len(k)])-97)%26+97) for i,c in enumerate(p)),end='')" "$1" "$2"; }
for case in "A iamtoldtogoandseeitbyfour qzxwvkjplmrt" \
            "B iamtoldtogoandseeitbyfour qzxw" \
            "C zephyrquixoticjovialbanjo qzxwvkjplmrt"; do
    set -- $case; id=$1 plain=$2 key=$3
    enc "$plain" "$key" > "$W/stress_$id.c"
    echo "== $id: $plain (${#plain} letters), key $key (kl ${#key}), started $(date +%T)" >> "$LOG"
    s=$(date +%s)
    ( ulimit -v 8000000; bin/vigenere -d "$W/stress_$id.c" --report buckets ) > "$W/stress_$id.out" 2> "$W/stress_$id.err"
    r=$(grep '^plain:' "$W/stress_$id.out" | grep -nx "plain:    $plain" | cut -d: -f1)
    echo "   wall $(( $(date +%s)-s ))s, true plaintext rank: ${r:-not found}, candidates: $(grep -c '^plain:' "$W/stress_$id.out")" >> "$LOG"
    echo "   top 3: $(grep '^plain:' "$W/stress_$id.out" | head -3 | awk '{printf "%s ", $2}')" >> "$LOG"
    grep -E "^(buckets|coverage|for full)" "$W/stress_$id.err" | sed 's/^/   /' >> "$LOG"
    awk 'NR>1 && $4!="complete"' "$W/stress_$id.err" | grep -v "^coverage\|^for full" | sort -k3n | tail -4 | sed 's/^/   bucket: /' >> "$LOG"
done
echo "== done $(date +%T)" >> "$LOG"
