Names: Lavie Lederman, Shahar Moalem

Probe-mode pintool. Profiles the program in TC, then after -prof_time seconds
switches to an optimized TC2 with de-virtualization and code reordering.


Compilation (Pin 4.0):
  export PIN_ROOT=/path/to/pin-external-4.0-...-gcc-linux
  mkdir -p ~/build && cp project.cpp makefile makefile.rules ~/build/ && cd ~/build
  make obj-intel64/project.so PIN_ROOT=$PIN_ROOT


How to run:
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./sgcc_peak.mytest-m64 200.i -o 200.s
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./sgcc_base.mytest-m64 200.i -o 200.s
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./cc1 200.i -o 200.s
  $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./bzip2 -k -f input-long.txt

  Printed at exit (starter formula: time from the TC2 switch to exit + prof_time):
    Translated code run (including profiling) took: <seconds> seconds

  Comparison runs (sgcc_peak shown, same for the other binaries):
    Native, no Pin:
      time ./sgcc_peak.mytest-m64 200.i -o 200.s
    Tool, optimizations on (default):
      $PIN_ROOT/pin -t ./project.so -prof_time 2 -- ./sgcc_peak.mytest-m64 200.i -o 200.s
    Tool, optimizations off:
      $PIN_ROOT/pin -t ./project.so -prof_time 2 -opt_devirt 0 -opt_layout 0 \
        -opt_dead_regs 0 -live_wide 0 -cold_direct 0 -cold_head_direct 0 \
        -stub_nohead 0 -cheap_targ 0 -targ_reset 0 -ft_elide 0 -short_br 0 \
        -entry_direct 0 -cold_unprobe 0 -devirt_imm 0 -migrate_tc 0 -tc_short_br 0 \
        -- ./sgcc_peak.mytest-m64 200.i -o 200.s


Result (course VM, printed time vs native time, 9 rounds per binary):
  Binary      Median   Mean    Rounds > 5%
  sgcc_peak   +8.5%    +9.2%   9/9
  sgcc_base   +7.4%    +8.6%   6/9
  cc1         -1.5%    -1.3%   0/9
  bzip2       -3.7%    -3.7%   0/9

  Target is > +5%. Output correct on all runs.
  cpugcc_r_base needs AVX-512, which the VM does not have.


What the tool does:
  1. TC: the main image is translated with counters per BBL, per jcc fallthrough,
     and per indirect jmp/call target (16 slots). Counters use dead flags or dead
     registers when possible, to keep profiling cheap.
  2. After -prof_time seconds a Pin thread stops profiling and builds TC2:
     - Code reordering: hot BBLs form chains from the routine entry, jcc are
       inverted to make the hot path fall through, cold BBLs move to a cold
       region, and hot routines are ordered by call counts.
     - De-virtualization: the hottest target T of an indirect jmp/call gets a
       guard and a direct jmp/call to T. The miss path keeps the original
       indirect branch. Guards at indirect jmps do not change RFLAGS, because
       jump-table targets may read them.
  3. Commit: each TC routine head is atomically patched (locked cmpxchg) into a
     jmp to its TC2 head. Frames still in TC continue in TC2.


Frequency thresholds (counts from the -prof_time window):
  Routine: hot if its hottest BBL count >= hot_rtn (64). Hot routines get the
           reordered layout. Cold routines are copied in original order.
  BBL:     in a hot routine, cold (moved out) if count < cold_bbl (4).
           The entry BBL always stays.
  Indirect target: de-virtualized if count >= hot_abs (64) and
           count >= hot_frac (60%) of the BBL count, and within rel32 of TC2.


Main knobs (defaults):
  -prof_time 2     profiling window in seconds
  -opt_devirt 1    de-virtualization
  -opt_layout 1    code reordering
  -hot_rtn 64, -cold_bbl 4, -hot_abs 64, -hot_frac 60   thresholds above
  -targ_flags 1    keep RFLAGS at indirect jmps
  Other knobs (each optimization can be turned off) are listed in project.cpp.
