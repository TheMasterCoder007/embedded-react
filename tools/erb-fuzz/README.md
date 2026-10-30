# erb-fuzz

"Nothing in a package can crash the device", checked: a libFuzzer target that mutates real Flow B packages,
hands them to `erb_load`, and runs whatever passes on the engine and the software backend, under ASan and
UBSan. CI's **Fuzz the package loader and VM** job runs it on every push and pull request, and any crash,
hang or sanitizer report fails the job.

One input is a package, optionally followed by a script of what the host does to it: frames, touches, keys,
input values, reloads. `fuzz_package.c` describes the format. A package with no script gets a default one
that taps across the panel, drags, types and sets its inputs.

## The corpus

| Folder                | What it is                                                                                      | Rule                |
| --------------------- | ----------------------------------------------------------------------------------------------- | ------------------- |
| `corpus/seeds/`       | Both demos and each parity fixture as a package. `npm run erb:corpus` rewrites them             | Each must load      |
| `corpus/found/`       | Inputs that reached code the seeds don't                                                        | Each must run clean |
| `corpus/regressions/` | Inputs that once crashed, hung or tripped a sanitizer. Add one with each fix, and never drop it | Each must run clean |

Run `npm run erb:corpus` from `packages/embedded-react` after a format change. The fuzz job fails when a seed
stops loading, since the fuzzer would otherwise only mutate packages the verifier rejects at once.

## Running it

On Linux, with clang and its runtime (`clang-18` and `libclang-rt-18-dev` on Ubuntu 24.04):

```bash
cmake -S tools/erb-fuzz -B tools/erb-fuzz/build -DCMAKE_C_COMPILER=clang-18 -DERB_FUZZ_LIBFUZZER=ON
cmake --build tools/erb-fuzz/build -j
ctest --test-dir tools/erb-fuzz/build --output-on-failure
mkdir -p /tmp/erb-fuzz-work
tools/erb-fuzz/build/erb-fuzz /tmp/erb-fuzz-work tools/erb-fuzz/corpus/seeds tools/erb-fuzz/corpus/found tools/erb-fuzz/corpus/regressions -max_total_time=300 -timeout=30
```

Apple's clang has no libFuzzer. Leave `ERB_FUZZ_LIBFUZZER` off and `erb-fuzz` runs the files it is given, still
under the sanitizers, which is enough to replay a failure:

```bash
cmake -S tools/erb-fuzz -B tools/erb-fuzz/build
cmake --build tools/erb-fuzz/build -j
tools/erb-fuzz/build/erb-fuzz crash-file
```

## When CI fails

The job uploads what it found as the `erb-fuzz-artifacts` artifact. Replay the file as above, fix the cause, and
add the file to `corpus/regressions/`.

To keep what a long run found, merge it into `corpus/found/`, which copies only the inputs that add coverage:

```bash
tools/erb-fuzz/build/erb-fuzz -merge=1 tools/erb-fuzz/corpus/found /tmp/erb-fuzz-work
```
