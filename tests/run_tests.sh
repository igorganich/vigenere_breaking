#!/usr/bin/env bash
set -euo pipefail

BIN="$(dirname "$0")/../bin/vigenere"
PASS=0; FAIL=0

# Run the cracker, check that expected_plain appears as a candidate
# with non-overlapping coverage ≥ min_cov (default 90%).
# Prints the matching candidate block on pass, first few lines on fail.
run_test() {
    local name="$1" expected_plain="$2" cipher="$3" min_cov="${4:-90}"

    local tmpcipher; tmpcipher=$(mktemp)
    printf '%s' "$cipher" > "$tmpcipher"
    local output
    output=$(timeout 600 "$BIN" -d "$tmpcipher" 2>/dev/null || true)
    rm -f "$tmpcipher"

    # Find the line number of "plain:    <expected>" in the output
    # grep exits 1 on no match; || true prevents set -e from killing the script
    local match_line; match_line=$(echo "$output" | grep -n "^plain:    ${expected_plain}$" | head -1 | cut -d: -f1 || true)

    if [[ -z "$match_line" ]]; then
        echo "FAIL  $name"
        echo "      '$expected_plain' not found among candidates"
        head -20 <<< "$output" | sed 's/^/      /'
        FAIL=$((FAIL+1))
        return
    fi

    # Extract coverage value from the coverage: line that follows the match
    local cov_line; cov_line=$(echo "$output" | sed -n "$((match_line+2))p")
    local cov_val; cov_val=$(echo "$cov_line" | grep -oE '[0-9]+\.[0-9]+' | head -1)
    local cov_int; cov_int=$(printf '%.0f' "${cov_val:-0}")

    if [[ "$cov_int" -ge "$min_cov" ]]; then
        echo "PASS  $name"
        # Print the 5-line candidate block (header+key+plain+words+coverage)
        local start=$((match_line-2))
        echo "$output" | sed -n "${start},$((start+4))p" | sed 's/^/      /'
        PASS=$((PASS+1))
    else
        echo "FAIL  $name  (coverage ${cov_val:-?}% < ${min_cov}%)"
        echo "$output" | sed -n "$((match_line-2)),$((match_line+2))p" | sed 's/^/      /'
        FAIL=$((FAIL+1))
    fi
}

encrypt_py() {
    local plain="$1" key="$2"
    python3 -c "
plain, key = '$plain', '$key'
print(''.join(
    chr((ord(p)-ord('a')+ord(key[i%len(key)])-ord('a'))%26+ord('a'))
    for i,p in enumerate(plain)
), end='')"
}

echo "=== Vigenere cracker test suite ==="
echo

# ── Existing cases ────────────────────────────────────────────────────────────

run_test "short_key_2   (13 chars, key=sh)"        "lowpricehotel"           "dvowjpulzvlld"
run_test "middle_key_7  (23 chars, key=somekey)"    "sisterlikestogoshopping" "kwexovjayqwdseggtsztgfu"
run_test "new_key_3     (20 chars, key=abc)"        "themanwenttothestore"    "$(encrypt_py themanwenttothestore abc)"
run_test "new_key_dict  (17 chars, key=key)"        "cryptographyisfun"       "$(encrypt_py cryptographyisfun key)"
run_test "new_key_5     (24 chars, key=house)"      "londonisamagnificentcity" "$(encrypt_py londonisamagnificentcity house)"

# ── Long keys (kl=7): the first word must be ≥ kl-1 letters ──────────────────

run_test "hard_long_word (15 chars, key=network)"  "algorithmdesign"         "$(encrypt_py algorithmdesign network)"  90

run_test "hard_kl7_word  (16 chars, key=hacking)"  "passwordsecurity"        "$(encrypt_py passwordsecurity hacking)"  90

# ── Short-ciphertext stress tests ─────────────────────────────────────────────
# Explore performance as ciphertext length approaches key length.
# Constraint: keylen ∈ [1, len-1]  (len==keylen → infinite plaintext space).

# kl=1 (Caesar) — trivially solvable at any length
run_test "kl1_5chars     (5 chars,  key=b)"           "hello"             "$(encrypt_py hello b)"                          80
run_test "kl1_8chars     (8 chars,  key=z)"           "sunshine"          "$(encrypt_py sunshine z)"                       80

# kl=4
run_test "kl4_8chars     (8 chars,  key=moon)"        "sunshine"          "$(encrypt_py sunshine moon)"                    80
run_test "kl4_10chars    (10 chars, key=code)"        "helloworld"        "$(encrypt_py helloworld code)"                  90

# kl=6
run_test "kl6_8chars     (8 chars,  key=cipher)"      "quickfox"          "$(encrypt_py quickfox cipher)"                  80
run_test "kl6_10chars    (10 chars, key=secret)"      "helloworld"        "$(encrypt_py helloworld secret)"                90

# kl=7, minimal length (len = kl+1): position 7 wraps back to key position 0,
# so the single word must be self-consistent — it is, since it was encrypted that way.
run_test "kl7_8chars     (8 chars,  key=network)"     "sunshine"          "$(encrypt_py sunshine network)"                 80
run_test "kl7_8chars_2   (8 chars,  key=compute)"     "computer"          "$(encrypt_py computer compute)"                 80

# kl=8 — the 8-char first word fixes every key position
run_test "kl8_11chars    (11 chars, key=security)"    "sunshineday"       "$(encrypt_py sunshineday security)"             80
run_test "kl8_13chars    (13 chars, key=password)"    "computerworld"     "$(encrypt_py computerworld password)"           90

# kl=9 — "algorithm"(9) ≥ kl=9
run_test "kl9_14chars    (14 chars, key=computing)"   "algorithmstudy"    "$(encrypt_py algorithmstudy computing)"         90

# kl=10 — "encryption"(10) ≥ kl=10
run_test "kl10_13chars   (13 chars, key=technology)"  "encryptionkey"     "$(encrypt_py encryptionkey technology)"         80

# kl=11 — "programming"(11) ≥ kl=11
run_test "kl11_14chars   (14 chars, key=mathematics)" "programminglab"    "$(encrypt_py programminglab mathematics)"       80

# kl=12 — "cryptography"(12) ≥ kl=12
run_test "kl12_15chars   (15 chars, key=introduction)" "cryptographykey"  "$(encrypt_py cryptographykey introduction)"     80

echo
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
