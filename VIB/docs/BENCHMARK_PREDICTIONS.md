# VIB Benchmark Predictions and Evidence Register

This file tracks hypotheses, not benchmark results. Do not enter predicted numbers in the measured-results column.

| ID | Hypothesis | Confidence | Experiment | Disconfirmation condition | Measured result / update |
|---|---|---|---|---|---|
| P-001 | Persistent artifact caching will reduce repeated unchanged build time. | Medium-high, conditional on correct cache keys and useful cache hit rate. | Compare clean cold build with repeated warm build; report median and spread. | Warm build is not reliably faster after excluding unrelated filesystem noise. | Not measured |
| P-002 | Dependency-aware incremental compilation will reduce rebuild work for local edits in large projects. | Medium, dependent on module graph granularity and interface hashing. | Edit one leaf module, one public interface, and a shared dependency; compare incremental and clean builds. | Rebuild time does not improve for leaf edits or outputs diverge from clean builds. | Not measured |
| P-003 | Native execution can improve steady-state throughput for suitable compute-heavy workloads after compilation cost is amortized. | Medium, depends on code generation and runtime overhead. | Compare interpreter, VM, and native modes on identical semantics and multiple workload families. | Native mode does not improve steady-state time on the targeted workload family. | Not measured |
| P-004 | The dispatcher may reduce total elapsed time by choosing routes based on workload length and cache state. | Low-medium, requires accurate cost model and route capabilities. | Compare explicit modes with dispatcher across short-lived and long-running programs. | Selection overhead or bad decisions erase gains or change behavior. | Not measured |
| P-005 | Adaptive compilation may help long-running programs with stable hot paths. | Low until profiling and safe fallback/deoptimization exist. | Include profiling overhead, compilation cost, warm-up, and steady-state measurements. | Gains vanish when total elapsed time and overhead are included. | Not measured |
| P-006 | VIB can outperform selected JIT baselines on some workloads. | Unknown until implemented and measured. | Use named versions, equivalent outputs, disclosed settings, repeated runs, and workload-specific comparisons. | No statistically reliable end-to-end advantage on the claimed workload. | Not measured |

## Required reporting format

For every new experiment, record:

- date and source commit;
- machine, OS, CPU, memory, compiler configuration, target and optimization settings;
- benchmark source, inputs, command and expected output;
- cold compile, warm cache, incremental compile, startup, total elapsed time, steady-state throughput, peak memory and artifact size;
- number of repetitions, median, spread, outliers and warm-up policy;
- correctness result and comparison baseline;
- prediction before the experiment, result after the experiment, and updated confidence.

Never compare a cached VIB build with an uncached competitor without stating that difference. Never combine compilation and runtime into one number without also reporting them separately.
