# Final report material: results and screenshots

All runs are on the course VM, with `-prof_time 2`. Metric: the tool's printed time
(`Translated code run (including profiling) took`) vs a native run (no Pin) of the same round.
The order alternates each round, and every tool output is `cmp`'d against native.
Scripts: `../scripts/vm_test.sh`, `../scripts/vm_all.sh`.

Final build: `src/project.cpp` md5 `acf16d815203421468979d757900e0c0` (snapshot in
`../options/checkpoint_targ_flags_all_correct/`).

## Main result: all binaries, 9 rounds (`screenshots/1_final_all_binaries_9rounds.png`)

| Binary | Native mean (s) | Tool mean (s) | Mean gain | Median per-round gain | Rounds > +5% | Correct |
|---|---|---|---|---|---|---|
| sgcc_peak | 11.140 | 10.118 | **+9.2%** | **+8.5%** | 9/9 | 9/9 |
| sgcc_base | 13.754 | 12.578 | **+8.6%** | **+7.4%** | 6/9 | 9/9 |
| cc1 | 26.984 | 27.322 | -1.3% | -1.5% | 0/9 | 9/9 |
| bzip2 (`-k -f input-long.txt`) | 5.632 | 5.838 | -3.7% | -3.7% | 0/9 | 9/9 |
| cpugcc_r_base | needs AVX-512, which the VM CPU lacks | | | | | |

- **Passes the spec** (more than 5% on one provided binary): sgcc_peak and sgcc_base.
- **Correct on every binary.**
- **bzip2:** stable, but its ~5.6 s run is too short to pay back the 2 s profiling window.

## A/B: flags fix vs old behaviour, sgcc_base, 10 rounds (`screenshots/2_...png`)

| | Mean gain | Median per-round gain | Correct |
|---|---|---|---|
| New (`-targ_flags 1`, default) | +7.3% | +7.4% | 10/10 |
| Old behaviour (`-targ_flags 0`) | +1.4% | +6.6% | 10/10 |

Tool vs alt in the same round: median difference +0.25%. The fix that makes sgcc_peak correct
costs nothing.

## Other runs
- `screenshots/3_...png`: all binaries, 3 rounds, loaded VM. Medians: sgcc_base +6.6%,
  sgcc_peak +8.2%, cc1 +10.4%, bzip2 -0.1%. All correct.
- `screenshots/4_...png`: all binaries, 3 rounds. Medians: sgcc_base +6.2% (every round > 5%),
  sgcc_peak +3.8%, cc1 +1.9%, bzip2 -8.5%. All correct.
- `screenshots/5_...png`: before the flags fix, sgcc_peak gave wrong output (0/3). The profiling stub
  and devirt guard clobbered RFLAGS at jump-table jmps whose targets read them.
- `screenshots/6_...png`: the older build, sgcc_base 10 rounds, +10.1% mean (its best run).

## Data for graphs
- `summary.csv`: per run and binary, native/tool means, mean and median gain, correctness.
- `per_round_gain.csv`: every round's gain for the 9-round run and the A/B run.
- `progression.csv`: the project's steps, from 0.5% vs all-off to the final vs-native result.

Full history of all runs: `../results/vm_sgcc_base_2026-09-26.md`.
