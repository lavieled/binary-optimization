Full name: Lavie Lederman

Description:
  ex2.so is a Pin JIT pintool that profiles basic block execution and branch
  edges (conditional taken/fallthrough, indirect jump targets) into
  edge-profile.csv, and collects per-routine register samples (RAX, RBX, RCX,
  RDX, RSI, RDI) into rtn-output.csv.

Compilation:
  export PIN_ROOT=/path/to/pin-kit
  cp ex2.cpp makefile makefile.rules $PIN_ROOT/source/tools/SimpleExamples/
  cd $PIN_ROOT/source/tools/SimpleExamples
  mkdir -p obj-intel64
  make obj-intel64/ex2.so
  cp obj-intel64/ex2.so /path/to/test/dir/

Run (tst):
  gcc -o tst tst.c
  chmod +x ex2.so tst
  gunzip -k tst.gz
  $PIN_ROOT/pin -t ./ex2.so -- ./tst
  Output: edge-profile.csv, rtn-output.csv
  Sanity: gal/bar/foo call_count = 4000 or 4001 (4 * MAX_CALLS); see ex2/README.txt

Run (bzip2):
  gunzip -k bzip2.gz input.txt.gz
  time $PIN_ROOT/pin -t ./ex2.so -- ./bzip2 -k -f input.txt
  Output: edge-profile.csv, rtn-output.csv
