#!/usr/bin/env python3
"""Benchmark the cracker on short ciphertexts.

Generates a deterministic set of (plaintext, key) cases, cracks each ciphertext with
`bin/vigenere crack`, and records the rank of the true plaintext among all candidates
(`show --rank-of`), the number of candidates and the wall time.

Usage:
  python3 bench/bench.py                  # default grid
  python3 bench/bench.py --clens 8,12     # subset of lengths
  python3 bench/bench.py --summary        # re-print the summary from the CSV
"""
import argparse, csv, os, random, re, subprocess, sys, tempfile, time
from collections import defaultdict
from statistics import median

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN   = os.path.join(ROOT, "bin", "vigenere")
WORDS = os.path.join(ROOT, "txt", "en", "words.txt")

FIELDS = ["case", "clen", "kl", "key", "plain", "status", "rank", "n_cands", "skipped", "seconds"]


def load_words(top):
    with open(WORDS) as f:
        words = [w.strip() for w in f if w.strip()]
    return [w for w in words[:top] if len(w) >= 2 and w.isalpha()]


def make_plain(rng, vocab, weights, n):
    """Concatenate Zipf-weighted common words to exactly n letters."""
    while True:
        s = ""
        while len(s) < n:
            s += rng.choices(vocab, weights)[0]
        if len(s) == n:
            return s


def make_cases(args):
    rng = random.Random(args.seed)
    vocab = load_words(args.vocab)
    weights = [1.0 / (i + 1) for i in range(len(vocab))]
    cases = []
    for clen in args.clens:
        for kl in args.kls:
            if kl >= clen:
                continue
            for _ in range(args.per_cell):
                plain = make_plain(rng, vocab, weights, clen)
                key = "".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(kl))
                cases.append({"case": len(cases), "clen": clen, "kl": kl, "key": key, "plain": plain})
    return cases


def encrypt(plain, key):
    return "".join(chr((ord(p) - 97 + ord(key[i % len(key)]) - 97) % 26 + 97) for i, p in enumerate(plain))


def run_one(case, args):
    work = tempfile.mkdtemp()
    cipher_file = os.path.join(work, "cipher.txt")
    index_file = os.path.join(work, "run.idx")
    with open(cipher_file, "w") as f:
        f.write(encrypt(case["plain"], case["key"]))
    t0 = time.monotonic()
    try:
        crack = subprocess.run([args.bin, "crack", cipher_file, index_file, "--top", "0",
                                "--threads", str(args.threads), "--max-candidates", str(args.max_candidates), "--no-lm"],
                               capture_output=True, text=True, timeout=args.timeout)
        secs = time.monotonic() - t0
        if crack.returncode != 0:
            status, rank, n, skipped = "error", 0, 0, ""
        else:
            out = subprocess.run([args.bin, "show", index_file, "--rank-of", case["plain"]],
                                 capture_output=True, text=True).stdout
            m = re.search(r"rank (\d+) of (\d+)", out)
            total = re.search(r"of (\d+)", out)
            rank = int(m.group(1)) if m else 0
            n = int(total.group(1)) if total else 0
            status = "ok"
            skipped = ";".join(re.findall(r"key length +(\d+): skipped", crack.stderr))
    except subprocess.TimeoutExpired:
        status, rank, n, skipped, secs = "timeout", 0, 0, "", args.timeout
    finally:
        for f in (cipher_file, index_file):   # the two files this run created
            if os.path.exists(f):
                os.unlink(f)
        os.rmdir(work)
    return dict(case, status=status, rank=rank, n_cands=n, skipped=skipped, seconds=round(secs, 2))


def summarize(rows):
    def pct(a, b):
        return f"{100 * a / b:5.1f}%" if b else "   - "
    if not rows:
        print("no results")
        return
    n = len(rows)
    ranks = [int(r["rank"]) for r in rows]
    print("\n== Overall ==")
    print(f"cases {n}; found {pct(sum(k > 0 for k in ranks), n)}; "
          + "; ".join(f"top-{t} {pct(sum(0 < k <= t for k in ranks), n)}" for t in (1, 10, 100, 1000))
          + f"; median candidates {median(int(r['n_cands']) for r in rows):.0f}"
          + f"; median time {median(float(r['seconds']) for r in rows):.1f}s"
          + f"; max time {max(float(r['seconds']) for r in rows):.1f}s")
    for dim in ("kl", "clen"):
        print(f"\n== by {dim}: found / top-1 / top-10 / median rank of found / median time ==")
        groups = defaultdict(list)
        for r in rows:
            groups[int(r[dim])].append(r)
        for v in sorted(groups):
            rs = groups[v]
            rk = [int(r["rank"]) for r in rs]
            found = [k for k in rk if k > 0]
            print(f"{dim}={v:<3} {pct(len(found), len(rs))}  {pct(sum(k == 1 for k in rk), len(rs))}  "
                  f"{pct(sum(0 < k <= 10 for k in rk), len(rs))}  "
                  f"{(median(found) if found else '-'):>10}  {median(float(r['seconds']) for r in rs):6.1f}s")
    skipped = [r for r in rows if r["skipped"]]
    if skipped:
        print(f"\n{len(skipped)} cases had key lengths skipped (too many candidates or repeating ciphertext)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clens", default="8,12,16,20,24", type=lambda s: [int(x) for x in s.split(",")])
    ap.add_argument("--kls", default="2,4,6,8,10,12", type=lambda s: [int(x) for x in s.split(",")])
    ap.add_argument("--per-cell", type=int, default=3, help="cases per (clen, kl) cell")
    ap.add_argument("--vocab", type=int, default=5000, help="plaintext words drawn from the top N")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--threads", type=int, default=0, help="cracker threads (0 = all)")
    ap.add_argument("--max-candidates", type=int, default=150_000_000)
    ap.add_argument("--timeout", type=float, default=3600, help="seconds per case")
    ap.add_argument("--bin", default=BIN, help="cracker binary to benchmark")
    ap.add_argument("--out", default=os.path.join(ROOT, "bench", "results.csv"))
    ap.add_argument("--summary", action="store_true", help="only print the summary of an existing CSV")
    args = ap.parse_args()

    done = []
    if os.path.exists(args.out):
        with open(args.out) as f:
            done = [r for r in csv.DictReader(f) if "n_cands" in r and "skipped" in r]
    if args.summary:
        summarize(done)
        return

    cases = make_cases(args)
    have = {(r["plain"], r["key"]) for r in done}
    todo = [c for c in cases if (c["plain"], c["key"]) not in have]
    print(f"{len(cases)} cases; {len(todo)} to run ({len(done)} cached in {args.out})", file=sys.stderr)
    new_file = not os.path.exists(args.out)
    with open(args.out, "a", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        if new_file:
            w.writeheader()
        for i, c in enumerate(todo, 1):     # one case at a time: each crack uses all threads
            r = run_one(c, args)
            w.writerow(r); f.flush()
            done.append({k: str(v) for k, v in r.items()})
            print(f"[{i}/{len(todo)}] clen={r['clen']:<2} kl={r['kl']:<2} rank={r['rank']:<9} "
                  f"cands={r['n_cands']:<10} {r['seconds']:7.1f}s {r['status']}"
                  + (f" skipped kl {r['skipped']}" if r["skipped"] else ""), file=sys.stderr)
    keys = {(c["plain"], c["key"]) for c in cases}
    summarize([r for r in done if (r["plain"], r["key"]) in keys])


if __name__ == "__main__":
    main()
