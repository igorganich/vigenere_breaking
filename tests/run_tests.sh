#!/usr/bin/env bash
# End-to-end tests of the command-line tool: encrypt a known plaintext with `vigenere -e`,
# crack it into an index with `vigenere crack`, and check with `vigenere show --rank-of`
# that the plaintext is among the candidates.  Prints the rank of each true plaintext.
set -uo pipefail
ulimit -v 10000000    # 10 GB: a runaway case fails instead of exhausting the machine
# The suite keeps the pre-disk cap of 150 million candidates: it works in $TMPDIR (often a
# RAM disk) and must stay short; the skip tests below are calibrated to it.
CAP=(--max-candidates 150000000 --no-lm)   # GPT-2 has its own tests at the end

BIN="$(cd "$(dirname "$0")/.." && pwd)/bin/vigenere"
WORK=$(mktemp -d)
PASS=0; FAIL=0

pass() { echo "PASS  $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL  $1"; FAIL=$((FAIL+1)); }

# run_test <name> <plaintext> <key> [crack options...]
run_test() {
    local name=$1 plain=$2 key=$3; shift 3
    printf '%s' "$plain" > "$WORK/plain.txt"
    echo "$key" | "$BIN" -e "$WORK/plain.txt" 2>/dev/null > "$WORK/cipher.txt"
    if ! "$BIN" crack "$WORK/cipher.txt" "$WORK/run.idx" --top 0 "${CAP[@]}" "$@" 2>/dev/null; then
        fail "$name: crack failed"; return
    fi
    local line; line=$("$BIN" show "$WORK/run.idx" --rank-of "$plain")
    if [[ $line == *": rank "* ]]; then pass "$name: ${line#*: }"
    else                                fail "$name: $line"
    fi
}

echo "=== Vigenere cracker test suite ==="

# expect_skip <name> <plaintext> <key>: the key is nearly as long as the text, so almost
# any word sequence fits; crack must skip that key length as too ambiguous, not fail.
expect_skip() {
    local name=$1 plain=$2 key=$3
    printf '%s' "$plain" > "$WORK/plain.txt"
    echo "$key" | "$BIN" -e "$WORK/plain.txt" 2>/dev/null > "$WORK/cipher.txt"
    if ! "$BIN" crack "$WORK/cipher.txt" "$WORK/run.idx" --top 0 "${CAP[@]}" 2>/dev/null; then
        fail "$name: crack failed"; return
    fi
    if "$BIN" show "$WORK/run.idx" --count | grep -q "key length ${#key} skipped: more than"; then
        pass "$name: key length ${#key} skipped as too ambiguous"
    else
        fail "$name: key length ${#key} was not skipped"
    fi
}

# ── Short ciphertexts: every key length up to len-1 (max 12) ─────────────────
run_test "kl1  5 letters  (hello / b)"                  hello                    b
run_test "kl1  8 letters  (sunshine / z)"               sunshine                 z
run_test "kl4  8 letters  (sunshine / moon)"            sunshine                 moon
run_test "kl6  8 letters  (quickfox / cipher)"          quickfox                 cipher
run_test "kl7  8 letters  (sunshine / network)"         sunshine                 network
run_test "kl7  8 letters  (computer / compute)"         computer                 compute
run_test "kl4  10 letters (helloworld / code)"          helloworld               code
run_test "kl6  10 letters (helloworld / secret)"        helloworld               secret
run_test "kl8  11 letters (sunshineday / security)"     sunshineday              security
run_test "kl2  13 letters (lowpricehotel / sh)"         lowpricehotel            sh
run_test "kl8  13 letters (computerworld / password)"   computerworld            password
run_test "kl9  14 letters (algorithmstudy / computing)" algorithmstudy           computing
run_test "kl7  15 letters (algorithmdesign / network)"  algorithmdesign          network
run_test "kl7  16 letters (passwordsecurity / hacking)" passwordsecurity         hacking

# ── Key nearly as long as the text: below about 1.5 x the key length a Vigenere ciphertext
#    has no unique reading, and these key lengths exceed the suite's --max-candidates ───
expect_skip "kl10 13 letters (encryptionkey / technology)"      encryptionkey   technology
expect_skip "kl11 14 letters (programminglab / mathematics)"    programminglab  mathematics
expect_skip "kl12 15 letters (cryptographykey / introduction)"  cryptographykey introduction

# ── Longer ciphertexts: key lengths 11-12 on 17+ letters take minutes and hundreds of MB
#    (see bench/stress.sh), so these stop at key length 10; their true keys are shorter ──
run_test "kl3  17 letters (cryptographyisfun / key)"    cryptographyisfun        key     --max-key-length 10
run_test "kl3  20 letters (themanwenttothestore / abc)" themanwenttothestore     abc     --max-key-length 10
run_test "kl7  23 letters (sisterlikestogoshopping / somekey)" sisterlikestogoshopping somekey --max-key-length 10
run_test "kl5  24 letters (londonisamagnificentcity / house)" londonisamagnificentcity house --max-key-length 10

# ── Command-line behaviour ────────────────────────────────────────────────────
printf 'abcdefghijklmnopqrstuvwxyz' > "$WORK/long.txt"
if "$BIN" crack "$WORK/long.txt" "$WORK/long.idx" 2>/dev/null; then fail "rejects a 26-letter ciphertext"
else                                                              pass "rejects a 26-letter ciphertext"
fi
printf 'abcabcabcab' > "$WORK/repeat.txt"
if "$BIN" crack "$WORK/repeat.txt" "$WORK/repeat.idx" --top 0 --max-key-length 4 --no-lm 2>/dev/null &&
   "$BIN" show "$WORK/repeat.idx" --count | grep -q "key length 3 skipped"; then
    pass "skips a key length the ciphertext repeats with"
else
    fail "skips a key length the ciphertext repeats with"
fi
if "$BIN" show "$WORK/plain.txt" --count 2>/dev/null; then fail "show rejects a file that is not an index"
else                                                       pass "show rejects a file that is not an index"
fi

# ── GPT-2 stage (needs models/gpt2, from tools/get_gpt2.sh) ─────────────────────
# "money talks" with key "merely": the trigram model ranks it 11th, GPT-2 first.
if [[ -f "$(dirname "$BIN")/../models/gpt2/model.safetensors" ]]; then
    printf 'moneytalks' > "$WORK/plain.txt"
    echo merely | "$BIN" -e "$WORK/plain.txt" 2>/dev/null > "$WORK/cipher.txt"
    best=$("$BIN" crack "$WORK/cipher.txt" "$WORK/run.idx" --top 1 --max-candidates 150000000 2>/dev/null)
    if [[ $best == *"key merely"*"Money talks."* ]] &&
       "$BIN" show "$WORK/run.idx" --lm 1 | grep -q "key merely .*Money talks\."; then
        pass "GPT-2: money talks / merely ranked first ($("$BIN" show "$WORK/run.idx" --rank-of moneytalks | sed 's/.*: //') by trigram)"
    else
        fail "GPT-2: money talks / merely not ranked first: $best"
    fi
    "$BIN" rerank "$WORK/run.idx" "$WORK/run.idx" --model bigram --top 0 2>/dev/null
    if [[ ! -e "$WORK/run.idx.lm" ]] && ! "$BIN" show "$WORK/run.idx" --lm 1 2>/dev/null; then
        pass "GPT-2: the shortlist is dropped when the index is re-ranked"
    else
        fail "GPT-2: a stale shortlist survived re-ranking"
    fi
    rm -f "$WORK/run.idx.lm"
else
    echo "SKIP  GPT-2 tests: models/gpt2 not found (run tools/get_gpt2.sh)"
fi

# Clean up: the files this script created in $WORK, one by one, then the directory.
rm -f "$WORK/plain.txt"
rm -f "$WORK/cipher.txt"
rm -f "$WORK/run.idx"
rm -f "$WORK/long.txt"
rm -f "$WORK/long.idx"
rm -f "$WORK/repeat.txt"
rm -f "$WORK/repeat.idx"
rmdir "$WORK"

echo
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
