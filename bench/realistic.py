#!/usr/bin/env python3
"""Ranking benchmark on realistic plaintexts.

bench/bench.py draws plaintexts word by word from the unigram distribution the scorer
itself uses, so it cannot show whether a better language model helps.  This benchmark uses
real short phrases (bench/phrases.txt): messages, instructions, sayings, plain sentences.

Each phrase gets one key, alternating between an English word and random letters, with a
key length of 2..min(12, 0.6 * text length), so the key is never nearly as long as the
text.  The crack covers every key length, as an attacker who does not know the key length
would.  Each index is kept in bench/cache/, and every model is measured on the same
candidates: the cached index is re-ranked with `vigenere rerank` for each model, and for
`gpt2` the best --lm-top of the trigram ranking are rescored with `vigenere rescore`.

Results go to bench/realistic-MODEL.csv, one per model; a case already in a model's CSV is
not rerun for that model.

Usage:
  python3 bench/realistic.py                            # run: trigram, and GPT-2 on its best 1000
  python3 bench/realistic.py --models trigram           # only some models
  python3 bench/realistic.py --cached-only              # skip cases not cracked yet
  python3 bench/realistic.py --summary                  # summaries and a side-by-side
  python3 bench/realistic.py --summary --models zipf,bigram,trigram   # include old CSVs
  python3 bench/realistic.py --list                     # the cases and the skipped phrases
"""
import argparse, csv, os, random, re, resource, subprocess, sys, time
from collections import defaultdict
from statistics import median

from bench import BIN, ROOT, WORDS, encrypt

PHRASES = os.path.join(ROOT, "bench", "phrases.txt")
CACHE   = os.path.join(ROOT, "bench", "cache")

FIELDS = ["case", "phrase", "plain", "clen", "keytype", "key", "kl", "cipher",
          "status", "rank", "n_cands", "skipped", "seconds", "top1_key", "top1_plain"]


def load_words():
    """The dictionary as the cracker loads it: 2+ letter words in rank order, plus a and i."""
    with open(WORDS) as f:
        words = [w.strip() for w in f if len(w.strip()) >= 2]
    return words, set(words) | {"a", "i"}


def load_phrases(vocab):
    kept, rejected = [], []
    with open(PHRASES) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            tokens = re.findall(r"[a-z]+", line.lower())
            plain = "".join(tokens)
            missing = [t for t in tokens if t not in vocab]
            if missing:
                rejected.append((line, "not in dictionary: " + " ".join(missing)))
            elif not 8 <= len(plain) <= 25:
                rejected.append((line, f"{len(plain)} letters, outside 8..25"))
            else:
                kept.append((line, plain))
    return kept, rejected


def make_cases(phrases, words, seed):
    rng = random.Random(seed)
    common = [w for w in words[:5000] if w.isalpha()]
    wider = [w for w in words[:20000] if w.isalpha()]   # for key lengths rare among common words
    cases = []
    for i, (phrase, plain) in enumerate(phrases):
        kl = rng.randint(2, max(2, min(12, int(0.6 * len(plain)))))
        keytype = "word" if i % 2 == 0 else "random"
        pool = []
        if keytype == "word":
            pool = [w for w in common if len(w) == kl] or [w for w in wider if len(w) == kl]
        if pool:
            key = rng.choice(pool)
        else:
            keytype = "random"
            key = "".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(kl))
        cases.append({"case": i, "phrase": phrase, "plain": plain, "clen": len(plain),
                      "keytype": keytype, "key": key, "kl": kl, "cipher": encrypt(plain, key)})
    return cases


def limit_memory():
    gb = 12
    resource.setrlimit(resource.RLIMIT_AS, (gb << 30, gb << 30))


def read_header(index):
    """Fields of an index header, or None if the file is missing or not an index."""
    try:
        with open(index, "rb") as f:
            head = f.read(4096).decode("ascii", "replace")
    except OSError:
        return None
    if not head.startswith("VIGIDX 1\n"):
        return None
    h = {"model": "zipf", "notes": []}       # no model line: ranked by the removed zipf model
    for line in head.split("\n"):
        if line == "end":
            break
        tag, _, rest = line.partition(" ")
        if tag in ("cipher", "count"):
            h[tag] = rest
        elif tag == "model":
            h["model"] = rest.split()[0]
        elif tag == "note":
            h["notes"].append(rest)
    return h


def crack(case, index, args):
    """Search the case's ciphertext into `index`; returns (status, seconds)."""
    cipher_file = os.path.join(CACHE, f"{case['cipher']}.txt")
    partial = index + ".part"
    with open(cipher_file, "w") as f:
        f.write(case["cipher"])
    t0 = time.monotonic()
    try:
        r = subprocess.run([args.bin, "crack", cipher_file, partial, "--top", "0", "--no-lm",
                            "--threads", str(args.threads)],
                           capture_output=True, text=True, timeout=args.timeout,
                           preexec_fn=limit_memory)
        status = "ok" if r.returncode == 0 else "error"
    except subprocess.TimeoutExpired:
        status = "timeout"
    secs = round(time.monotonic() - t0, 1)
    os.unlink(cipher_file)
    if status == "ok":
        os.replace(partial, index)
    elif os.path.exists(partial):
        os.unlink(partial)
    return status, secs


def gpt2_rank(index, plain, args):
    """Rescore the best args.lm_top of a trigram index with GPT-2; rank of `plain` in the
    shortlist (0 if it is not in it) and the reading ranked first."""
    rr = subprocess.run([args.bin, "rescore", index, "--top", str(args.lm_top), "--print", "0",
                         "--threads", str(args.threads)], capture_output=True, text=True)
    if rr.returncode != 0:
        sys.exit(f"rescore failed: {rr.stderr}")
    with open(index + ".lm") as f:
        lines = f.read().split("end\n", 1)[1].splitlines()
    entries = [l.split("\t") for l in lines]          # trigram rank, key, score, sentence
    letters = lambda sentence: "".join(c for c in sentence.lower() if c.isalpha())
    rank = next((i + 1 for i, e in enumerate(entries) if letters(e[3]) == plain), 0)
    return dict(rank=rank, top1_key=entries[0][1] if entries else "",
                top1_plain=letters(entries[0][3]) if entries else "")


def run_one(case, models, args):
    """Rows for `models` (model -> row), or None if not cached and --cached-only."""
    os.makedirs(CACHE, exist_ok=True)
    index = os.path.join(CACHE, f"{case['cipher']}.idx")
    secs = ""
    h = read_header(index)
    if h is None:
        if args.cached_only:
            return None
        status, secs = crack(case, index, args)
        if status != "ok":
            return {m: dict(case, status=status, rank=0, n_cands=0, skipped="", seconds=secs,
                            top1_key="", top1_plain="") for m in models}
        h = read_header(index)
    n = int(h["count"])
    skipped = ";".join(re.findall(r"key length (\d+) skipped", "\n".join(h["notes"])))
    rows = {}
    for model in models:
        ranked = index
        base = "trigram" if model == "gpt2" else model     # gpt2 rescores the trigram ranking
        if h["model"] != base:               # re-ranking is cheap next to the search
            ranked = os.path.join(CACHE, f"{case['cipher']}.{base}.idx")
            have = read_header(ranked)
            ok = have and have["model"] == base and have.get("count") == h["count"] and subprocess.run(
                [args.bin, "show", ranked, "--nth", "1"], capture_output=True).returncode == 0
        if h["model"] != base and not ok:     # missing, or ranked with older tables
            rr = subprocess.run([args.bin, "rerank", index, ranked, "--model", base, "--top", "0",
                                 "--threads", str(args.threads)], capture_output=True, text=True)
            if rr.returncode != 0:
                sys.exit(f"rerank failed: {rr.stderr}")
        if model == "gpt2":
            rows[model] = dict(case, status="ok", n_cands=n, skipped=skipped, seconds=secs,
                               **gpt2_rank(ranked, case["plain"], args))
            continue
        rank_out = subprocess.run([args.bin, "show", ranked, "--rank-of", case["plain"]],
                                  capture_output=True, text=True).stdout
        m = re.search(r": rank (\d+) of", rank_out)
        top1_key = top1_plain = ""
        if n:
            top = subprocess.run([args.bin, "show", ranked, "--first", "1"],
                                 capture_output=True, text=True).stdout.split()
            top1_key, top1_plain = top[2], top[3]   # "<rank> key <key> <plain> <split> score <s>"
        rows[model] = dict(case, status="ok", rank=int(m.group(1)) if m else 0, n_cands=n,
                           skipped=skipped, seconds=secs, top1_key=top1_key, top1_plain=top1_plain)
    return rows


def summarize(rows):
    if not rows:
        print("no results")
        return

    def line(label, rs):
        rk = [int(r["rank"]) for r in rs]
        found = [k for k in rk if k > 0]
        pct = lambda a: f"{100 * a / len(rs):5.1f}%"
        mrr = sum(1 / k for k in found) / len(rs)
        print(f"{label:<14} {len(rs):>4}  {pct(len(found))}  {pct(sum(k == 1 for k in rk))}  "
              f"{pct(sum(0 < k <= 10 for k in rk))}  {pct(sum(0 < k <= 100 for k in rk))}  "
              f"{pct(sum(0 < k <= 1000 for k in rk))}  {mrr:6.3f}  "
              f"{(median(found) if found else '-'):>9}")

    print(f"\n{'':<14} {'n':>4}  {'found':>6}  {'top1':>6}  {'top10':>6}  {'top100':>6}  "
          f"{'top1k':>6}  {'MRR':>6}  {'med rank':>9}")
    line("all", rows)
    for kt in ("word", "random"):
        line(f"key: {kt}", [r for r in rows if r["keytype"] == kt])
    for lo, hi in ((8, 12), (13, 17), (18, 21), (22, 25)):
        rs = [r for r in rows if lo <= int(r["clen"]) <= hi]
        if rs:
            line(f"letters {lo}-{hi}", rs)
    times = [float(r["seconds"]) for r in rows if r["seconds"]]
    if times:
        print(f"\ncrack time: median {median(times):.1f}s, max {max(times):.1f}s, total {sum(times) / 3600:.1f}h")
    lost = [r for r in rows if int(r["rank"]) == 0]
    if lost:
        print(f"\nnot found ({len(lost)}):")
        for r in lost:
            why = f"key length {r['kl']} skipped" if r["kl"] in r["skipped"].split(";") else r["status"]
            print(f"  {r['phrase']!r:32} key {r['key']:<12} {why}")
    worst = sorted((r for r in rows if int(r["rank"]) > 10), key=lambda r: -int(r["rank"]))
    if worst:
        print(f"\nranked below 10 ({len(worst)}), with the reading ranked first:")
        for r in worst:
            print(f"  {int(r['rank']):>9}  {r['plain']:<26} key {r['key']:<12} "
                  f"#1 {r['top1_plain']} (key {r['top1_key']})")


def compare(results, models):
    """Cases measured by every model whose ranks differ, side by side."""
    keys = set.intersection(*({(r["plain"], r["key"]) for r in results[m]} for m in models))
    by = {m: {(r["plain"], r["key"]): int(r["rank"]) for r in results[m]} for m in models}
    diff = sorted((k for k in keys if len({by[m][k] for m in models}) > 1),
                  key=lambda k: -max(by[m][k] for m in models))
    print(f"\n== side by side: {len(keys)} cases measured by all of {', '.join(models)}; "
          f"{len(diff)} differ (rank 0 = not found)")
    if diff:
        print(f"  {'plaintext':<26} {'key':<12} " + " ".join(f"{m:>9}" for m in models))
        for k in diff:
            print(f"  {k[0]:<26} {k[1]:<12} " + " ".join(f"{by[m][k]:>9}" for m in models))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--threads", type=int, default=0, help="cracker threads (0 = all)")
    ap.add_argument("--timeout", type=float, default=3600, help="seconds per case")
    ap.add_argument("--bin", default=BIN, help="cracker binary")
    ap.add_argument("--models", default="trigram,gpt2",
                    help="comma-separated: unigram, bigram, trigram, gpt2 (GPT-2 on the trigram's best)")
    ap.add_argument("--lm-top", type=int, default=1000, help="candidates GPT-2 rescores")
    ap.add_argument("--cached-only", action="store_true", help="skip cases whose index is not cached")
    ap.add_argument("--summary", action="store_true", help="only print the summaries of the CSVs")
    ap.add_argument("--list", action="store_true", help="only print the cases")
    args = ap.parse_args()
    models = args.models.split(",")
    out = {m: os.path.join(ROOT, "bench", f"realistic-{m}.csv") for m in models}

    words, vocab = load_words()
    phrases, rejected = load_phrases(vocab)
    cases = make_cases(phrases, words, args.seed)
    if args.list:
        for c in cases:
            print(f"{c['case']:>3}  {c['plain']:<26} {c['clen']:>2}  {c['keytype']:<6} {c['key']:<12} {c['cipher']}")
        for phrase, why in rejected:
            print(f"skipped {phrase!r}: {why}")
        return

    wanted = {(c["plain"], c["key"]) for c in cases}
    results = {}
    for m in models:
        rows = []
        if os.path.exists(out[m]):
            with open(out[m]) as f:
                rows = [r for r in csv.DictReader(f) if r["status"] != "error"]   # errors are retried
        results[m] = [r for r in rows if (r["plain"], r["key"]) in wanted]

    def report():
        for m in models:
            print(f"\n== {m}")
            summarize(results[m])
        if len(models) > 1:
            compare(results, models)

    if args.summary:
        report()
        return

    for phrase, why in rejected:
        print(f"skipped {phrase!r}: {why}", file=sys.stderr)
    have = {m: {(r["plain"], r["key"]) for r in results[m]} for m in models}
    todo = [(c, [m for m in models if (c["plain"], c["key"]) not in have[m]]) for c in cases]
    todo = [(c, ms) for c, ms in todo if ms]
    print(f"{len(cases)} cases; {len(todo)} to run", file=sys.stderr)
    files = {}
    for m in models:
        new_file = not os.path.exists(out[m])
        files[m] = open(out[m], "a", newline="")
        files[m] = (files[m], csv.DictWriter(files[m], FIELDS))
        if new_file:
            files[m][1].writeheader()
    for i, (c, ms) in enumerate(todo, 1):      # one case at a time: each crack uses all threads
        rows = run_one(c, ms, args)
        if rows is None:
            print(f"[{i}/{len(todo)}] {c['plain']:<26} not cached, skipped", file=sys.stderr)
            continue
        for m, r in rows.items():
            files[m][1].writerow(r)
            files[m][0].flush()
            results[m].append({k: str(v) for k, v in r.items()})
        r = next(iter(rows.values()))
        ranks = "  ".join(f"{m} {rows[m]['rank']}" for m in ms)
        print(f"[{i}/{len(todo)}] {r['plain']:<26} key {r['key']:<12} of {r['n_cands']:<10} "
              f"{r['seconds'] or 'cached':>7}s {r['status']}  {ranks}", file=sys.stderr)
    for f, _ in files.values():
        f.close()
    report()

if __name__ == "__main__":
    main()
