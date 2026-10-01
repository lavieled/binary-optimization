Names: Lavie Lederman, Shahar Moalem

Probe-mode pintool: TC with BBL + indirect jmp/call target profiling, then after
-prof_time seconds an optimized TC2 with de-virtualization of hot indirect jumps
and calls, code reordering (hot chains, jcc inversion, cold-block outlining,
Pettis-Hansen routine order) and cheap profiling / switch-over.


Compilation (Pin 4.0):
  export PIN_ROOT=/path/to/pin-external-4.0-...-gcc-linux
  mkdir -p ~/build && cp project.cpp makefile makefile.rules ~/build/ && cd ~/build
  make obj-intel64/project.so PIN_ROOT=$PIN_ROOT
  (or copy the three files into $PIN_ROOT/source/tools/SimpleExamples and run the
   same make command there)


How to run:
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./sgcc_peak.mytest-m64 200.i -o 200.s
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./sgcc_base.mytest-m64 200.i -o 200.s
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./cc1 200.i -o 200.s
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./bzip2 -k -f input-long.txt

  Printed at exit:
    Translated code run (including profiling) took: <seconds> seconds

  Same tool with all optimizations off (for comparison):
    $PIN_ROOT/pin -t ./project.so -prof_time 2 -opt_devirt 0 -opt_layout 0 -opt_dead_regs 0
      -live_wide 0 -cold_direct 0 -cold_head_direct 0 -stub_nohead 0 -cheap_targ 0
      -targ_reset 0 -ft_elide 0 -short_br 0 -entry_direct 0 -cold_unprobe 0
      -devirt_imm 0 -migrate_tc 0 -tc_short_br 0 -- ./sgcc_base.mytest-m64 200.i -o 200.s


Result (course VM, printed time vs native elapsed time without Pin, 9 alternating
rounds, median of the per-round gains, output compared with native every run):
  sgcc_peak  +8.5%  (9/9 rounds above 5%)
  sgcc_base  +7.4%
  cc1        -1.5%
  bzip2      -3.7%
  All runs correct. cpugcc_r_base needs AVX-512, which the VM CPU does not have.


What the tool does:
  1. Translate the main image into TC with a counter per BBL, a fall-through
     counter after every conditional branch, and target counters for every
     indirect jmp AND indirect call (16 slots, hash (addr >> 4) & 15).
     With -opt_dead_regs 1 each counter is "inc [rip+x]" when the flags are
     dead at that point, else mov/lea/mov through a dead register, else the
     spill form (RAX saved to a rip-relative slot).  Liveness follows the
     routine's paths from the stub (fall-through, direct jmp, both edges of
     a jcc), at most 64 instructions; call/ret/indirect/syscall or leaving
     the routine counts as a use.  Every stub head gets its own skip jmp
     when profiling stops.  With -entry_direct 1, Pin's probe at each
     routine entry jumps straight to the TC head (no bridge).
  2. After -prof_time seconds a Pin internal thread disables profiling and
     builds TC2 from the profile:
     - opt_layout: in each hot routine the hot BBLs form chains starting at
                   the entry (greedy heaviest successor); a jcc is inverted
                   when its taken successor is laid out next, a direct jmp
                   to the next block is removed; cold BBLs are outlined to a
                   cold region after all hot code.  Hot routines are ordered
                   by Pettis-Hansen chain merging on call weights (counts of
                   the BBLs holding direct calls/tail jumps, plus hot
                   indirect-call targets).  jmp/jcc inside TC2 use rel8 when
                   they fit (short_br).  By default (hot_only 0) cold
                   routines also get a TC2 copy in original order.
     - opt_devirt: the hottest target T of an indirect jmp/call becomes
                   cmp target,T; jne miss; jmp/call T (direct).  With
                   devirt_imm the compare is "cmp [mem]|reg, imm32" (no
                   scratch).  The miss path (original indirect jmp/call) is
                   out of line.  Scratch registers are spilled to slots next
                   to TC2 (not push/pop: red zone).
     - targ_flags: at indirect jmps other than jmp [rip+x], the flags may be
                   live (jump-table targets can read them), so the TC target
                   stub keeps them (seto al; lahf ... add al,0x7f; sahf) and
                   the TC2 guard is flag-free: rcx = target - T (mov + lea),
                   jrcxz hit; jmp miss.  Calls keep the cmp guard (flags are
                   dead across a call).
  3. Commit: every TC routine head is atomically patched (locked 8-byte
     cmpxchg) into a jmp to its TC2 head; with entry_direct the original
     routine entries jump straight to TC2 too; with migrate_tc every TC stub's
     skip jmp is repointed into TC2, so frames still in TC continue in TC2.


Frequency thresholds (frequently vs rarely executed), counts are from the
-prof_time window:

  Routine:  hot if the count of its hottest BBL >= hot_rtn   (default 64).
            Rarely executed (cold) otherwise.  Using the hottest BBL, not
            the entry, keeps routines that are entered once but loop.
            Hot routines get the reordered layout; cold routines are
            copied in original order (or left native with -hot_only 1).
  BBL:      inside a hot routine, a BBL is cold (outlined) if its count
            < cold_bbl (default 4).  The entry BBL is never outlined.
  Indirect target: hot (de-virtualized) only if BOTH
              count >= hot_abs                    (default 64)
              count * 100 >= BBL count * hot_frac (default 60 = 60%)
            and the target is within rel32 reach of TC2.


Knobs (defaults):
  -prof_time 2      profiling window in seconds
  -opt_devirt 1     de-virtualize hot indirect jmp/call
  -opt_layout 1     hot/cold split, jcc inversion, Pettis-Hansen order
  -hot_only 0       1: TC2 holds only hot routines; cold routines run original code
  -short_br 1       rel8 jmp/jcc in TC2 where they fit (only with -opt_layout 1)
  -hot_rtn 64       routine hot threshold (see above)
  -cold_bbl 4       BBL cold threshold (see above)
  -hot_abs 64       min count of a de-virtualized target
  -hot_frac 60      min percent of the BBL count for that target
  -devirt_imm 1     devirt guard "cmp [mem]|reg, imm32" when the target fits
  -targ_flags 1     keep RFLAGS at indirect jmps (TC stub and TC2 guard)
  -entry_direct 1   original routine entries jump straight to the TC head,
                    then to the TC2 head at commit (no Pin bridge)
  -migrate_tc 1     at commit, TC stub skip jmps go into TC2 (frames in TC move over)
  -tc_short_br 1    rel8 jmp/jcc in TC too where they fit
  -opt_dead_regs 1  cheap TC counter stubs (dead flags / dead registers)
  -live_wide 1      stub liveness follows direct jmp and both jcc edges
                    (0: straight-line only)
  -stub_nohead 1    no NOP5 head per profiling stub; the skip jmp overwrites
                    the stub's first instruction (0: NOP5 heads)
  -cheap_targ 1     short indirect-target stub: rip-relative slots, 2 scratch
                    regs, R11 unsaved at indirect calls (0: starter-style stub)
  -targ_reset 1     a target slot's count restarts when its address changes,
                    so each count belongs to the recorded target
  -ft_elide 1       no fall-through stub when the next BBL is entered only by
                    that fall-through; its count is taken from the next BBL
  -cold_direct 1    with hot_only 1: TC2 branches to a cold routine go straight
                    to its original entry (0: through the cold stub)
  -cold_head_direct 1  with hot_only 1: TC head of a cold routine jumps straight
                    to its original entry (0: to the TC2 cold stub)
  -cold_unprobe 1   with hot_only 1: cold routines get their original entry
                    bytes back and run natively with no hops
  -opt_inline 0     1: inline hot small leaf callees (tried: no gain)
  -inl_hot 1000 / -inl_max_ins 24   inlining thresholds
  -ctr_derive 0     1: skip counters derivable from one incoming edge (tried: no gain)
  -ctr_derive_check 0  1: keep them and compare (validation)
  -run_stats 0      1: with a -prof_time longer than the run, write whole-run
                    profile stats to run_stats.txt
  -tc2_stats 0      1: print TC2 summary and build phase times to stderr
  -dump_prof 0      1: write the profile to bprofile.out
  -dump_tc / -dump_tc2 / -dump_orig_code / -verbose / -no_tc_commit / -no_prof : debugging


Notes:
  - The printed time is (time from the TC2 switch to exit) + prof_time, as in
    the course starter; the time spent building TC2 (while the program keeps
    running in TC) is not counted.
  - If TC2 is not installed (build failed or -no_tc_commit) the tool prints
    "TC2 not installed" and the real elapsed time since start instead.
  - loop*/j*cxz (rel8 only) are emitted in TC2 as "op L1; jmp L2; L1: jmp
    target (rel32); L2:".  Routines containing them keep their original
    block order and are not de-virtualized.
