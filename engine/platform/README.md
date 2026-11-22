# engine/platform

Engine-side code for one processor family. Display code lives in `backends/`.

- `er_fault_cortex_m.c`: the Cortex-M fault handler, which fills an `ERFaultRecord` (`er_fault_cortex_m.h`).
