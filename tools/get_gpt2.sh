#!/usr/bin/env bash
# Download GPT-2 small (OpenAI, MIT licence) from Hugging Face into models/gpt2/, where
# bin/vigenere looks for it.  About 550 MB.  The files are pinned to one revision and
# checked by SHA-256, so the scores match the tests.
set -euo pipefail
cd "$(dirname "$0")/.."
DIR=models/gpt2
REV=607a30d783dfa663caf39e06633721c8d4cfcd7e
URL=https://huggingface.co/openai-community/gpt2/resolve/$REV
mkdir -p "$DIR"

fetch() {   # fetch <file> <sha256>
    local file=$1 sum=$2
    if [[ -f $DIR/$file ]] && echo "$sum  $DIR/$file" | sha256sum -c --quiet 2>/dev/null; then
        echo "$file: already present"
        return
    fi
    echo "$file: downloading"
    curl -fL --progress-bar --retry 3 -o "$DIR/$file.part" "$URL/$file"
    echo "$sum  $DIR/$file.part" | sha256sum -c --quiet
    mv "$DIR/$file.part" "$DIR/$file"
}

fetch vocab.json        196139668be63f3b5d6574427317ae82f612a97c5d1cdaf36ed2256dbf636783
fetch merges.txt        1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5
fetch model.safetensors 248dfc3911869ec493c76e65bf2fcf7f615828b0254c12b473182f0f81d3a707
echo "GPT-2 is ready in $DIR"
