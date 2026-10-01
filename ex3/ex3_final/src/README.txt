Exercise 3 - btranslate.cpp fixes

Names + IDs:
Shahar Moalem
Lavie Lederman

How to build:
Copy src/btranslate.cpp into Pin's source/tools/SimpleExamples directory, or keep it there as the tool source, and run:
    make clean
    make btranslate.test

How to run:
    ../../../pin -t obj-intel64/btranslate.so -- ./sgcc_base.mytest-m64 200.i -o 200.s

If the course checker requires the original binary name, run:
    ../../../pin -t obj-intel64/btranslate.so -- ./cpugcc_r_base.Oz-m64 200.i -o 200.s

Main fixes:
1. Fixed the off-by-one bug in jump_to_orig_addr_map insertion: the target address is now stored before advancing the counter.
2. Fixed target-entry chaining comparison from > 0 to >= 0, so target entry 0 is also valid.
3. Added RTN_IsSafeForProbedReplacement() before RTN_ReplaceProbed().
4. Replaced the unreliable RTN_Size() style check with a real XED decoding of the first probe patch bytes.
5. Added proper handling for conditional branches whose target remains in original code. Jcc cannot be rewritten as an indirect branch, so the tool retargets it with a direct rel32 displacement.
6. Avoided fragile indirect jump/call-to-original through jump_to_orig_addr_map when a direct rel32 displacement is possible.
7. Added local routine chaining only, so cross-routine control flow goes through the original entry and Pin's probe if committed.
8. Added conservative default commit mode. The tool commits only simple straight-line leaf routines and leaves complex GCC routines in the original code. This avoids crashes caused by jump tables, cold internal labels, call/pop idioms, or other complex control flow that this educational translator does not fully model.

Notes:
The program still executes correctly because routines skipped by the translator remain in the original executable. Conservative mode can be disabled with:
    -conservative_commit 0
but for this exercise binary the default conservative mode is safer.
