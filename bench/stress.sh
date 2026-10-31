#!/usr/bin/env bash
# Stress test: full-vocabulary crack of the hardest shape for the search — the maximum
# length (25 letters) with a 12-letter key and a plaintext of short words.  Logs the time,
# the number of candidates and the rank of the true plaintext to bench/stress.log.
cd "$(dirname "$0")/.."
LOG=bench/stress.log
WORK=$(mktemp -d)
plain=iamtoldtogoandseeitbyfour
key=qzxwvkjplmrt

printf '%s' "$plain" > "$WORK/plain.txt"
echo "$key" | bin/vigenere -e "$WORK/plain.txt" 2>/dev/null > "$WORK/cipher.txt"
{
    echo "== stress: $plain (${#plain} letters), key $key (length ${#key}), started $(date '+%F %T')"
    s=$(date +%s)
    ( ulimit -v 12000000; bin/vigenere crack "$WORK/cipher.txt" "$WORK/stress.idx" --top 5 )
    echo "== crack: $(( $(date +%s)-s ))s"
    bin/vigenere show "$WORK/stress.idx" --rank-of "$plain"
} 2>&1 | tee "$LOG"

# Clean up: the three files this script created, then the directory.
rm -f "$WORK/plain.txt"
rm -f "$WORK/cipher.txt"
rm -f "$WORK/stress.idx"
rmdir "$WORK"
