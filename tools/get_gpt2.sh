#!/usr/bin/env bash
# Download a GPT-2 small from Hugging Face into models/, where bin/vigenere looks for it.
#
#   tools/get_gpt2.sh        English: OpenAI's GPT-2 (MIT licence) → models/gpt2/, about 550 MB
#   tools/get_gpt2.sh de     German: dbmdz/german-gpt2 (MIT licence) → models/gpt2-de/, about 510 MB
#   tools/get_gpt2.sh uk     Ukrainian: benjamin/gpt2-wechsel-ukrainian (MIT licence) → models/gpt2-uk/,
#                            about 670 MB
#
# The files are pinned to one revision and checked by SHA-256, so the scores match the tests.
set -euo pipefail
cd "$(dirname "$0")/.."
LANG_CODE=${1:-en}

case $LANG_CODE in
    en) DIR=models/gpt2
        URL=https://huggingface.co/openai-community/gpt2/resolve/607a30d783dfa663caf39e06633721c8d4cfcd7e ;;
    de) DIR=models/gpt2-de
        URL=https://huggingface.co/dbmdz/german-gpt2/resolve/0579faad6d549cb924aa34e53baf3a0b3add7cda ;;
    uk) DIR=models/gpt2-uk
        URL=https://huggingface.co/benjamin/gpt2-wechsel-ukrainian/resolve/fac65ca77db57ef928c5b6db1be75fe1d4459238 ;;
    *)  echo "usage: $0 [en|de|uk]" >&2; exit 1 ;;
esac
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

case $LANG_CODE in
    en) fetch vocab.json        196139668be63f3b5d6574427317ae82f612a97c5d1cdaf36ed2256dbf636783
        fetch merges.txt        1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5
        fetch model.safetensors 248dfc3911869ec493c76e65bf2fcf7f615828b0254c12b473182f0f81d3a707 ;;
    de) fetch tokenizer.json    3a71b5f040f581c983dc1d9b367b9fc3b82962ef240834a0adc605fdb96d7ed8
        fetch model.safetensors fb9d37175ff0520fdd688221e4c80fa5bbf123d5849d43698b8a4bc52eb0069c ;;
    uk) fetch vocab.json        8f4d34adc4fc7ffaecc8c11b4b283a7cba365b7ce0fea55cf181665c36dd542b
        fetch merges.txt        c911c1e53808b1ea6dd1a71f66b2eb3818c76894514a4320c54b8cce8bcce95f
        fetch model.safetensors 5879c9d1d143dd82af93c063a2f3c7f32f8e55123642c58ed308f59004086c27 ;;
esac
echo "GPT-2 is ready in $DIR"
