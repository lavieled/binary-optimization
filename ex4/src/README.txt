Submitters:
  Lavie Lederman
  Shahar Moalem


Compilation (Pin 4):
  export PIN_ROOT="/path/to/pin"
  cp src/bprofile.cpp src/makefile src/makefile.rules $PIN_ROOT/source/tools/SimpleExamples/
  cd $PIN_ROOT/source/tools/SimpleExamples
  make obj-intel64/bprofile.so PIN_ROOT=$PIN_ROOT
  cp obj-intel64/bprofile.so /path/to/ex4/src/bprofile.so


How to run:
  $PIN_ROOT/pin -t ./src/bprofile.so -prof_time 2 -- ./sgcc_base.mytest-m64 200.i -o 200.s
  Output: edge-profile.csv (hottest BBLs first)

  Speedup check (bzip2):
  /usr/bin/time -v $PIN_ROOT/pin -t ./src/bprofile.so -opt_dead_regs 0 -prof_time 20 -- ./bzip2 -k -f input-long.txt
  /usr/bin/time -v $PIN_ROOT/pin -t ./src/bprofile.so -opt_dead_regs 1 -prof_time 20 -- ./bzip2 -k -f input-long.txt

  flags:
  -prof_time "sec" - how long to collect counters 
  -opt_dead_regs 0|1 , 1 = skip dead regs 


Measured performance (bzip2, -prof_time 20):
  opt=0: user 5.82s, system 2.22s, elapsed 8.47s
  opt=1: user 4.12s, system 1.82s, elapsed 6.49s
  got ~29% better user time (~23% better elapsed)


Problems we fixed:
  1. Large ex3 binaries (sgcc) crashed or got killed.
     Added probe checks, only commit simple leaf routines on big images, and allocate smaller maps.
  2. Dead-reg opt (-opt_dead_regs): naive uses RAX/RBX/RCX around
     the counter, opt skips dead regs/flags and uses a cheap INC when it can.
  3. Profiling window / CSV: start -prof_time only after the TC is ready,
     and hook _exit so edge-profile.csv is written.


Why the profile differs from exercise 2:
  Ex2 is a JIT tool that instruments almost every BBL for the whole run.
  We are in a probe mode: we only put a few leaf routines in a TC and only count
  during -prof_time. So the CSVs look very different:

  - Coverage: ex2 had more than 4000 BBLs, ours had only 3.
  - Format: ex2 sometimes skips taken/fallthru, we always print both.
    Ex2 can list up to 10 indirect targets, we only keep 4.
  - Hot addresses / counts won't match, because we see much less code
    and for a shorter time. BBL splitting can also differ a bit.
