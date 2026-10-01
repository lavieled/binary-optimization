Full name: Lavie Lederman



Compilation:
from test dir
  export PIN_ROOT=(pin_dir)/pin-external-4.0-99633-g5ca9893f2-gcc-linux(not must but easy)
  cp ex1.cpp $PIN_ROOT/source/tools/SimpleExamples/
  cd $PIN_ROOT/source/tools/SimpleExamples
  mkdir -p obj-intel64(if needed)
  make obj-intel64/ex1.so
  cp obj-intel64/ex1.so /path/to/test/dir/

Run:
  chmod +x ex1.so tst bzip2
  gunzip -k tst.gz
  $PIN_ROOT/pin -t ./ex1.so -- ./tst
  gunzip -k bzip2.gz
  time $PIN_ROOT/pin -t ./ex1.so -- ./bzip2 -k -f input.txt( time on my VM was 2.3 sec)

  Output: rtn-output.csv
