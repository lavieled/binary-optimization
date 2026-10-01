# Binary Optimization

Technion 046275, Dynamic Binary Translation and Optimization. Pintools built with
[Intel Pin](https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-dynamic-binary-instrumentation-tool.html) 4.0.

Only the submitted files are tracked: each tool's source, makefiles, README and built `.so`.

| Folder | Assignment | Submitted files |
|---|---|---|
| [ex1](ex1/) | Routine profiling: call and instruction counts, register sampling (JIT) | `ex1.so`, `src/ex1.cpp`, makefiles, `README.txt` |
| [ex2](ex2/final_zip/) | BBL and edge profiling, indirect jump targets (JIT) | `ex2.so`, `src/ex2.cpp`, makefiles, `README.txt` |
| [ex3](ex3/ex3_final/) | Fix the probe-mode translator `btranslate.cpp` for cpugcc_r_base | `ex3.so`, `src/` |
| [ex4](ex4/) | Optimize `bprofile.cpp`: dead-register stubs, edge-profile.csv | `bprofile.so`, `src/` |
| [final_project](final_project/src/) | Probe-mode TC + optimized TC2: profiling, code reordering, de-virtualization | `src/`; results in `report/` (screenshots, CSVs) and `results/` (VM run log) |

Final project result on the course VM (printed time vs native, median of 9 rounds):
sgcc_peak +8.5%, sgcc_base +7.4%, correct output on every binary.

Build any tool (Pin 4.0), from its `src/` folder:
```bash
make obj-intel64/<tool>.so PIN_ROOT=/path/to/pin-external-4.0-...-gcc-linux
```
Each folder's `README.txt` has the exact compile and run commands.
