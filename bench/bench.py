#!/usr/bin/env python3
"""Benchmark the CSP cracker on short ciphertexts.

Generates a deterministic set of (plaintext, key) cases, runs bin/vigenere -d
with each search configuration, and records where the true plaintext ranks
in the candidate list, how many candidates were produced, and wall time.

Usage:
  python3 bench/bench.py                      # default grid, default CSP settings
  python3 bench/bench.py --methods csp-strict,csp-t30  # compare search settings
  python3 bench/bench.py --summary            # re-print summary from CSV
"""
import argparse, csv, os, random, subprocess, sys, tempfile, time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from statistics import median

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN   = os.path.join(ROOT, "bin", "vigenere")
WORDS = os.path.join(ROOT, "txt", "english_words.txt")

METHODS = {
    "csp":        [],                          # default settings (300 s time limit)
    "csp-strict": ["--wcs-min-word", "99"],    # first word ≥ kl−1 only (stage 0)
    "csp-t10":    ["--time-limit", "10"],
    "csp-t30":    ["--time-limit", "30"],
    "csp-t60":    ["--time-limit", "60"],
    "csp-full":   ["--time-limit", "0"],       # complete search
}

FIELDS = ["case", "clen", "kl", "key", "plain", "method",
          "status", "rank", "n_cands", "seconds"]


def load_words(top):
    with open(WORDS) as f:
        words = [w.strip() for w in f if w.strip()]
    # Plaintext vocabulary: common words, 2+ letters (the dictionary used for
    # coverage scoring ignores 1-letter words, so "a"/"i" could never be covered).
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
                cases.append({"case": len(cases), "clen": clen, "kl": kl,
                              "key": key, "plain": plain})
    return cases


def encrypt(plain, key):
    return "".join(chr((ord(p) - 97 + ord(key[i % len(key)]) - 97) % 26 + 97)
                   for i, p in enumerate(plain))


def run_one(case, method, timeout, binary, threads):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(encrypt(case["plain"], case["key"]))
        path = f.name
    t0 = time.monotonic()
    try:
        out = subprocess.run([binary, "-d", path, "--threads", str(threads)] + METHODS[method],
                             capture_output=True, text=True, timeout=timeout).stdout
        status = "ok"
    except subprocess.TimeoutExpired:
        out, status = "", "timeout"
    finally:
        os.unlink(path)
    secs = time.monotonic() - t0

    rank, n = 0, 0
    for line in out.splitlines():
        if line.startswith("--- candidate"):
            n += 1
        elif line.startswith("plain:") and rank == 0 and line.split()[1] == case["plain"]:
            rank = n
    return dict(case, method=method, status=status, rank=rank,
                n_cands=n, seconds=round(secs, 2))


def summarize(rows, methods):
    def pct(a, b):
        return f"{100 * a / b:5.1f}%" if b else "   - "

    by_m = defaultdict(list)
    for r in rows:
        by_m[r["method"]].append(r)
    methods = [m for m in methods if m in by_m]

    print("\n== Overall ==")
    print(f"{'method':<11} {'cases':>5} {'top1':>7} {'top10':>7} {'found':>7} "
          f"{'timeout':>7} {'med cands':>9} {'med s':>7} {'max s':>7}")
    for m in methods:
        rs = by_m[m]; n = len(rs)
        r = [int(x["rank"]) for x in rs]
        print(f"{m:<11} {n:>5} {pct(sum(0 < k <= 1 for k in r), n):>7} "
              f"{pct(sum(0 < k <= 10 for k in r), n):>7} {pct(sum(k > 0 for k in r), n):>7} "
              f"{sum(x['status'] == 'timeout' for x in rs):>7} "
              f"{median(int(x['n_cands']) for x in rs):>9.0f} "
              f"{median(float(x['seconds']) for x in rs):>7.1f} "
              f"{max(float(x['seconds']) for x in rs):>7.1f}")

    for dim in ("kl", "clen"):
        vals = sorted({int(r[dim]) for r in rows})
        for label, ok in (("found (any rank)", lambda k: k > 0), ("top-10", lambda k: 0 < k <= 10)):
            print(f"\n== {label} by {dim} ==")
            print(f"{'method':<11} " + " ".join(f"{dim}={v:<4}" for v in vals))
            for m in methods:
                cells = []
                for v in vals:
                    rs = [x for x in by_m[m] if int(x[dim]) == v]
                    cells.append(pct(sum(ok(int(x["rank"])) for x in rs), len(rs)).strip().rjust(len(dim) + 5))
                print(f"{m:<11} " + " ".join(cells))

    # Head-to-head: cases CSP found that no other method found, and vice versa.
    if "csp" in by_m:
        found = defaultdict(set)
        for r in rows:
            if int(r["rank"]) > 0:
                found[r["method"]].add(int(r["case"]))
        others = set().union(*(found[m] for m in methods if m != "csp"))
        common = set.intersection(*(set(int(x["case"]) for x in by_m[m]) for m in methods))
        print(f"\n== Head-to-head on {len(common)} cases run by every method ==")
        print(f"found only by csp:          {len((found['csp'] - others) & common)}")
        print(f"found by others, not csp:   {len((others - found['csp']) & common)}")
        print(f"found by nobody:            {len(common - found['csp'] - others)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--methods", default="csp", help="comma list of: " + ", ".join(METHODS))
    ap.add_argument("--clens", default="8,12,16,20,24", type=lambda s: [int(x) for x in s.split(",")])
    ap.add_argument("--kls", default="2,4,6,8,10,12", type=lambda s: [int(x) for x in s.split(",")])
    ap.add_argument("--per-cell", type=int, default=3, help="cases per (clen, kl) cell")
    ap.add_argument("--vocab", type=int, default=5000, help="plaintext words drawn from the top N")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=120, help="seconds per run")
    ap.add_argument("--jobs", type=int, default=4, help="cases run in parallel")
    ap.add_argument("--threads", type=int, default=1, help="cracker threads per run (jobs x threads <= cores)")
    ap.add_argument("--bin", default=BIN, help="cracker binary to benchmark")
    ap.add_argument("--out", default=os.path.join(ROOT, "bench", "results.csv"))
    ap.add_argument("--summary", action="store_true", help="only print summary of existing CSV")
    args = ap.parse_args()
    methods = args.methods.split(",")
    for m in methods:
        if m not in METHODS:
            sys.exit(f"unknown method: {m}")

    done = []
    if os.path.exists(args.out):
        with open(args.out) as f:
            done = list(csv.DictReader(f))
    if args.summary:
        summarize(done, methods)
        return

    cases = make_cases(args)
    have = {(r["plain"], r["key"], r["method"]) for r in done}
    todo = [(c, m) for c in cases for m in methods if (c["plain"], c["key"], m) not in have]
    print(f"{len(cases)} cases x {len(methods)} methods; {len(todo)} runs to do "
          f"({len(done)} cached in {args.out})", file=sys.stderr)

    new_file = not os.path.exists(args.out)
    with open(args.out, "a", newline="") as f, ThreadPoolExecutor(args.jobs) as pool:
        w = csv.DictWriter(f, FIELDS)
        if new_file:
            w.writeheader()
        futs = [pool.submit(run_one, c, m, args.timeout, args.bin, args.threads) for c, m in todo]
        for i, fut in enumerate(as_completed(futs), 1):
            r = fut.result()
            w.writerow(r); f.flush()
            done.append({k: str(v) for k, v in r.items()})
            print(f"[{i}/{len(todo)}] {r['method']:<10} clen={r['clen']:<2} kl={r['kl']:<2} "
                  f"rank={r['rank']:<5} cands={r['n_cands']:<6} {r['seconds']:6.1f}s {r['status']}",
                  file=sys.stderr)

    case_keys = {(c["plain"], c["key"]) for c in cases}
    summarize([r for r in done if (r["plain"], r["key"]) in case_keys], methods)


if __name__ == "__main__":
    main()
