# TODO

## Time-limited mode: replace the node budget with a per-bucket time share

**Problem.** In a time-limited run, each bucket (key length × first-word length) stops at a
fixed node budget of 500,000 word placements. Since the search became multithreaded, all
threads draw on one bucket's budget at once, so each first word gets only about 1/20 of it.
Most expensive buckets end with no first word finished, and the run stops long before the
time limit.

Stress test (`bench/stress.sh`, default settings, 20 threads, 25 letters, key length 12):

- every run ended after 22–27 s of the 300 s limit, with under 10% of the full vocabulary
  searched;
- the key-length-12 buckets with 1–3-letter first words each used their budget in about 8 s
  and finished 0 first words;
- with 6-letter first words: 0 of 5,812 finished.

**Proposal.** Drop the node budget in time-limited mode and give each bucket a share of wall
time instead: `remaining time / remaining buckets`, recomputed as buckets finish, so cheap
buckets pass their unused time on. The whole time limit then gets used, and no bucket is
starved by an earlier one. The coverage report stays as it is, with "time share" replacing
"node budget" as the reason a bucket was cut.

Does not affect full-vocabulary runs (`--wcs-budget 0 --wcs-results 0 --time-limit 0`),
which have no caps.

## GPU port of the full-vocabulary search

**Why.** Full-vocabulary coverage is CPU-bound: `helloworld` (10 letters) takes 10 min on
20 CPU threads, and the 25-letter, key-length-12 stress case takes hours. A plain GPU key
brute force has the opposite profile. Its cost depends only on the key space,
min(12, length − 1) letters, so it is fast on very short texts but hits a fixed wall at 26¹²:

| ciphertext | keys | RTX 4090 at 10¹⁰ / 10¹¹ / 10¹² keys/s (estimated, not measured) |
|---|---|---|
| 10 letters (keys ≤ 9) | 5.7 × 10¹² | 9 min / 56 s / 6 s |
| 12 letters (keys ≤ 11) | 3.8 × 10¹⁵ | 4.4 days / 11 h / 1.1 h |
| 13–25 letters (keys ≤ 12) | 9.9 × 10¹⁶ | 115 days / 11.5 days / 28 h |

Both approaches produce the same candidate list under the same acceptance rule (≥ 90%
word coverage) and ranking; the GPU only buys speed.

**Idea.** Run the CSP itself on the GPU: dictionary pruning (search only word sequences
that fit) combined with ~100× the parallelism of 20 CPU threads.

- Work units as on the CPU: (bucket, first word), split by second word where a bucket has
  few first words. One unit per thread block or warp, scheduled in plan order.
- Trie in GPU global memory (read-only, a few MB); the top of the trie and a
  valid-4-letter-prefix bitmap (26⁴ bits = 57 KB) in shared memory for cheap early
  rejection.
- The recursion becomes an explicit per-thread stack: depth ≤ 25 positions, plus the key
  buffer (≤ 12 slots).
- Accepted keys go to a per-block buffer; the host does segmentation, scoring and the
  top-N pool, as now. Candidates are rare compared to search nodes.
- Main risk: warp divergence. Subtree sizes vary by orders of magnitude, so neighbouring
  threads finish at very different times; this needs work stealing or fine-grained units.

**Expected payoff.** Largest exactly where the CPU struggles: 13–25-letter texts with long
keys, where brute force needs days to months and the CSP explores only a tiny fraction of
the key space.

**Baseline to beat.** Wall time of `bench/stress.sh` case A at full coverage on 20 CPU
threads (run in progress; see `bench/stress_full.log`).
