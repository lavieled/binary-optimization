# Results snapshot: course VM, sgcc_base, tool vs native (26 Sep 2026)

Build: `src/project.cpp` md5 `deeca0db8557409f3ee387dadeebb276` (all defaults on, including
`entry_direct`, `cold_unprobe`, `devirt_imm`, `migrate_tc`, `tc_short_br`).
Command: `RUNS=10 TOOL=./project.so ./vm_test.sh` (order alternates each round; every tool
output `cmp`'d against native). Metric: the tool's printed time vs native wall time.

## Run 2: 10 rounds (`vm_sgcc_base_10runs.png`)

| Round | Native (s) | Tool (s) | Gain |
|---|---|---|---|
| 1 | 9.251 | 13.712 | -48.2% |
| 2 | 16.379 | 15.011 | +8.4% |
| 3 | 16.545 | 15.524 | +6.2% |
| 4 | 14.187 | 10.096 | +28.8% |
| 5 | 16.948 | 13.767 | +18.8% |
| 6 | 14.733 | 13.494 | +8.4% |
| 7 | 12.875 | 9.164 | +28.8% |
| 8 | 10.026 | 9.042 | +9.8% |
| 9 | 10.152 | 9.027 | +11.1% |
| 10 | 10.150 | 9.176 | +9.6% |

- Mean: native 13.125 s, tool 11.801 s, **+10.1%**.
- Median per-round gain: **+9.7%**. The tool won 9 of 10 rounds.
- Quiet rounds 8-10 (native back to about 10.1 s): native 10.109, tool 9.082, **+10.2%**.
- Correct: 10/10.
- Rounds 1-7 ran while the host was loaded (native varied from 9.3 to 16.9 s), so they are noisy.

## Run 1: 6 rounds (`vm_sgcc_base_6runs.png`)

| Round | Native (s) | Tool (s) | Gain |
|---|---|---|---|
| 1 | 9.883 | 9.130 | +7.6% |
| 2 | 11.027 | 10.414 | +5.6% |
| 3 | 13.722 | 13.995 | -2.0% |
| 4 | 14.619 | 13.518 | +7.5% |
| 5 | 21.839 | 9.901 | outlier |
| 6 | 9.944 | 8.925 | +10.3% |

- Mean +18.7% (inflated by round 5). Median per-round gain +7.6%. Correct: 6/6.

## Run 3: all binaries, 3 rounds each (`vm_all_binaries_before_flags_fix.png`)

Same build, `vm_all.sh`. Printed time vs native, mean / median per round:

| Binary | Native (s) | Tool (s) | Mean | Median | Correct |
|---|---|---|---|---|---|
| sgcc_base | 11.870 | 12.748 | -7.4% | -13.2% | 3/3 |
| sgcc_peak | 10.199 | 2.949 | (wrong output) | | **0/3** |
| cc1 | 27.103 | 30.617 | -13.0% | -12.3% | 3/3 |
| bzip2 (`input-long.txt`) | 8.240 | 8.983 | -9.0% | -11.9% | 3/3 |
| cpugcc_r_base | skipped (not present / needs AVX-512) | | | | |

sgcc_base swung to -7% here, against +10% in run 2 on the same build (only 3 rounds).
**sgcc_peak bug:** the indirect-jmp target-profiling stub and the TC2 devirt guard clobbered
RFLAGS, and sgcc_peak has jump-table targets that read flags. Fixed afterwards (`-targ_flags`):
flags are kept with seto/lahf in the TC stub, and a flag-free `lea`+`jrcxz` guard is used in TC2.
The fixed build was re-run on the VM: see runs 5-8.

## Run 4: all binaries again, old build, 3 rounds (`vm_all_binaries_old_build_run2.png`)

| Binary | Native (s) | Tool (s) | Mean | Per round | Correct |
|---|---|---|---|---|---|
| sgcc_base | 15.297 | 14.410 | +5.8% | -0.3, +2.5, +14.9 (median +2.5%) | 3/3 |
| sgcc_peak | 14.956 | 3.172 | (wrong output) | | **0/3** |
| cc1 | 35.899 | 38.890 | -8.3% | -10.1, -4.9, -10.2 | 3/3 |
| bzip2 | 8.493 | 9.207 | -8.4% | -18.4, -5.8, -1.2 | 3/3 |

The VM was slow in this run (sgcc native 15.3 s vs about 10 s in quiet rounds), so the numbers are noisy.

## Run 5: all binaries, NEW build with `-targ_flags` fix, 3 rounds (`vm_all_binaries_new_build_targ_flags.png`)

Source md5 `acf16d815203421468979d757900e0c0`. The VM was quiet (sgcc native 9.0 s).

| Binary | Native (s) | Tool (s) | Mean | Per round | Correct |
|---|---|---|---|---|---|
| sgcc_base | 9.027 | 8.380 | **+7.2%** | +6.2, +9.4, +5.9 (median +6.2%) | 3/3 |
| sgcc_peak | 8.196 | 8.808 | -7.5% | +3.8, +4.9, -30.3 (median +3.8%) | **3/3** |
| cc1 | 22.912 | 23.740 | -3.6% | +1.9, -15.1, +2.2 | 3/3 |
| bzip2 | 4.607 | 4.862 | -5.5% | -9.4, +1.0, -8.5 | 3/3 |

- Every binary now produces correct output.
- sgcc_base beat native by more than 5% in every round.

## Run 6: A/B, new build vs old behaviour, 10 rounds (`vm_sgcc_base_ab_targ_flags_10runs.png`)

`RUNS=10 ALT="-targ_flags 0" ./vm_test.sh`: `tool` = new build (flags fix on), `alt` = the same
.so with `-targ_flags 0` (identical to the old build on sgcc_base). Same rounds, alternating order.

| | Mean took | Mean vs native | Median per-round vs native | Correct |
|---|---|---|---|---|
| native | 11.547 s | | | |
| tool (new) | 10.706 s | +7.3% | **+7.4%** | 10/10 |
| alt (old behaviour) | 11.380 s | +1.4% | +6.6% | 10/10 |

tool vs alt per round, as (alt - tool) / alt: +0.6, -0.3, -3.0, -0.1, -2.4, +1.4, +34.7, +17.4,
-3.8, +1.6. Median +0.25%, so there is no real difference. The +34.7/+17.4 rounds are VM noise in the
alt run. Conclusion: the flags fix costs nothing on sgcc_base, so keep the new build.
Tool per round vs native: +7.0 +7.5 -44.9 +8.1 +37.5 +8.0 +11.7 +6.2 +7.2 +7.2. That is 9/10 rounds
above 5%; the -44.9 is round 3, when the VM slowed down (13.7 s tool vs 9.4 s native).

## Run 7: all binaries, new build, 3 rounds (`vm_all_binaries_new_build_run2.png`)

The VM was loaded (sgcc native 15.9 s), so compare within rounds.

| Binary | Native (s) | Tool (s) | Mean | Per round | Median | Correct |
|---|---|---|---|---|---|---|
| sgcc_base | 15.927 | 14.284 | +10.3% | +29.9, -6.4, +6.6 | +6.6% | 3/3 |
| sgcc_peak | 13.535 | 12.220 | +9.7% | +13.4, +7.3, +8.2 | **+8.2%** | 3/3 |
| cc1 | 31.510 | 28.440 | +9.7% | +13.3, +10.4, +5.6 | **+10.4%** | 3/3 |
| bzip2 | 6.844 | 6.908 | -0.9% | +2.2, -5.0, -0.1 | -0.1% | 3/3 |

In this run, sgcc_peak and cc1 beat native by more than 5% in every round. All binaries are correct.

## Run 8: all binaries, new build, 9 rounds (`vm_all_binaries_new_build_9runs.png`) - main result

| Binary | Native (s) | Tool (s) | Mean | Median | Rounds > +5% | Correct |
|---|---|---|---|---|---|---|
| sgcc_base | 13.754 | 12.578 | +8.6% | +7.4% | 6/9 | 9/9 |
| sgcc_peak | 11.140 | 10.118 | **+9.2%** | **+8.5%** | **9/9** | 9/9 |
| cc1 | 26.984 | 27.322 | -1.3% | -1.5% | 0/9 | 9/9 |
| bzip2 (`input-long.txt`) | 5.632 | 5.838 | -3.7% | -3.7% | 0/9 | 9/9 |

Per round:
- sgcc_base: +7.4 -0.3 +8.7 +7.1 +28.6 +7.5 +0.1 +4.5 +10.4
- sgcc_peak: +7.8 +8.8 +7.6 +8.5 +10.5 +13.5 +8.3 +9.5 +7.6
- cc1: -1.9 -0.5 +2.7 -2.4 -1.5 -4.6 +0.7 -2.6 -1.3
- bzip2: -4.5 -3.7 -3.3 -3.4 -4.5 -4.6 -2.7 -2.4 -3.8

- sgcc_peak is the most stable pass: every round is between +7.6% and +13.5%.
- bzip2 is very stable, but about 3.7% slower than native. Its ~5.6 s run is too short to pay back
  the 2 s profiling window.

## For comparison
- Old build (e737728d) on the same VM: 10.77 s vs native 8.90 s, 21% slower.
