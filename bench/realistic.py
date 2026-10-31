#!/usr/bin/env python3
"""Ranking benchmark on realistic plaintexts.

bench/bench.py draws plaintexts word by word from the unigram distribution the scorer
itself uses, so it cannot show whether a better language model helps.  This benchmark uses
real short phrases (bench/phrases.txt): messages, instructions, sayings, plain sentences.

Each phrase gets one key, alternating between an English word and random letters, with a
key length of 2..min(12, 0.6 * text length), so the key is never nearly as long as the
text.  The crack covers every key length, as an attacker who does not know the key length
would.  Each index is kept in bench/cache/ (ranked by zipf, the default model), so another
ranking model can be measured on the same candidates with `vigenere rerank`, without
repeating the search.

Usage:
  python3 bench/realistic.py                  # run (cases already in the CSV are not rerun)
  python3 bench/realistic.py --model bigram   # re-rank the cached indexes by another model
                                              # (results in bench/realistic-bigram.csv)
  python3 bench/realistic.py --summary        # re-print the summary from the CSV
  python3 bench/realistic.py --list           # print the cases and the skipped phrases
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


def run_one(case, args):
    os.makedirs(CACHE, exist_ok=True)
    index = os.path.join(CACHE, f"{case['cipher']}.idx")
    secs, skipped = "", ""
    count = subprocess.run([args.bin, "show", index, "--count"], capture_output=True, text=True)
    if count.returncode != 0 and args.cached_only:
        return None
    if count.returncode != 0:                  # not cached, or built with another dictionary
        cipher_file = os.path.join(CACHE, f"{case['cipher']}.txt")
        partial = index + ".part"
        with open(cipher_file, "w") as f:
            f.write(case["cipher"])
        t0 = time.monotonic()
        try:
            crack = subprocess.run([args.bin, "crack", cipher_file, partial, "--top", "0",
                                    "--threads", str(args.threads)],
                                   capture_output=True, text=True, timeout=args.timeout,
                                   preexec_fn=limit_memory)
            status = "ok" if crack.returncode == 0 else "error"
        except subprocess.TimeoutExpired:
            status = "timeout"
        secs = round(time.monotonic() - t0, 1)
        os.unlink(cipher_file)
        if status != "ok":
            if os.path.exists(partial):
                os.unlink(partial)
            return dict(case, status=status, rank=0, n_cands=0, skipped="", seconds=secs,
                        top1_key="", top1_plain="")
        os.replace(partial, index)
        count = subprocess.run([args.bin, "show", index, "--count"], capture_output=True, text=True)
    if args.model != "zipf":                   # cheap compared with the search: always redone
        ranked = os.path.join(CACHE, f"{case['cipher']}.{args.model}.idx")
        rr = subprocess.run([args.bin, "rerank", index, ranked, "--model", args.model, "--top", "0",
                             "--threads", str(args.threads)], capture_output=True, text=True)
        if rr.returncode != 0:
            sys.exit(f"rerank failed: {rr.stderr}")
        index = ranked
    n = int(re.search(r": (\d+) candidates", count.stdout).group(1))
    skipped = ";".join(re.findall(r"key length (\d+) skipped", count.stdout))
    rank_out = subprocess.run([args.bin, "show", index, "--rank-of", case["plain"]],
                              capture_output=True, text=True).stdout
    m = re.search(r": rank (\d+) of", rank_out)
    top1_key = top1_plain = ""
    if n:
        top = subprocess.run([args.bin, "show", index, "--first", "1"],
                             capture_output=True, text=True).stdout.split()
        top1_key, top1_plain = top[2], top[3]       # "<rank> key <key> <plain> <split> score <s>"
    return dict(case, status="ok", rank=int(m.group(1)) if m else 0, n_cands=n,
                skipped=skipped, seconds=secs, top1_key=top1_key, top1_plain=top1_plain)


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--threads", type=int, default=0, help="cracker threads (0 = all)")
    ap.add_argument("--timeout", type=float, default=3600, help="seconds per case")
    ap.add_argument("--bin", default=BIN, help="cracker binary")
    ap.add_argument("--model", default="zipf", help="ranking model (zipf = as cracked)")
    ap.add_argument("--cached-only", action="store_true", help="skip cases whose index is not cached")
    ap.add_argument("--out", help="CSV (default bench/realistic.csv, or realistic-MODEL.csv)")
    ap.add_argument("--summary", action="store_true", help="only print the summary of the CSV")
    ap.add_argument("--list", action="store_true", help="only print the cases")
    args = ap.parse_args()
    if not args.out:
        name = "realistic.csv" if args.model == "zipf" else f"realistic-{args.model}.csv"
        args.out = os.path.join(ROOT, "bench", name)

    words, vocab = load_words()
    phrases, rejected = load_phrases(vocab)
    cases = make_cases(phrases, words, args.seed)
    if args.list:
        for c in cases:
            print(f"{c['case']:>3}  {c['plain']:<26} {c['clen']:>2}  {c['keytype']:<6} {c['key']:<12} {c['cipher']}")
        for phrase, why in rejected:
            print(f"skipped {phrase!r}: {why}")
        return

    done = []
    if os.path.exists(args.out):
        with open(args.out) as f:
            done = list(csv.DictReader(f))
    done = [r for r in done if r["status"] != "error"]     # errors are retried
    have = {(r["plain"], r["key"]) for r in done}
    wanted = {(c["plain"], c["key"]) for c in cases}
    if args.summary:
        summarize([r for r in done if (r["plain"], r["key"]) in wanted])
        return

    for phrase, why in rejected:
        print(f"skipped {phrase!r}: {why}", file=sys.stderr)
    todo = [c for c in cases if (c["plain"], c["key"]) not in have]
    print(f"{len(cases)} cases; {len(todo)} to run", file=sys.stderr)
    new_file = not os.path.exists(args.out)
    with open(args.out, "a", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        if new_file:
            w.writeheader()
        for i, c in enumerate(todo, 1):        # one case at a time: each crack uses all threads
            r = run_one(c, args)
            if r is None:
                print(f"[{i}/{len(todo)}] {c['plain']:<26} not cached, skipped", file=sys.stderr)
                continue
            w.writerow(r)
            f.flush()
            done.append({k: str(v) for k, v in r.items()})
            print(f"[{i}/{len(todo)}] {r['plain']:<26} key {r['key']:<12} rank {r['rank']:<9} "
                  f"of {r['n_cands']:<10} {r['seconds'] or 'cached':>7}s {r['status']}", file=sys.stderr)
    summarize([r for r in done if (r["plain"], r["key"]) in wanted])


if __name__ == "__main__":
    main()
