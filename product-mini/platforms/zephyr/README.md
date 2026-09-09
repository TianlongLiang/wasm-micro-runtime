# How to use WAMR with Zephyr

[Zephyr](https://www.zephyrproject.org/) is an open source real-time operating
system (RTOS) with a focus on security and broad hardware support. WAMR is
compatible with Zephyr via the [Zephyr WAMR
port](../../../core/shared/platform/zephyr), and is packaged as a [Zephyr
module](../../../zephyr) so that an application only has to enable a few
Kconfig options to get the runtime linked into its image.

## Samples

| Sample                       | What it demonstrates                                          |
| ---------------------------- | ------------------------------------------------------------- |
| [simple](./simple)           | Minimal application: run a WASM module with the built-in libc |
| [simple-file](./simple-file) | WASI file system API on top of Zephyr `fs_*`                  |
| [simple-http](./simple-http) | WASI socket API on top of Zephyr `zsock_*`                    |
| [user-mode](./user-mode)     | Running the runtime inside a Zephyr user-mode thread          |
| [user-mode-multi-thread](./user-mode-multi-thread) | Running guest pthreads from static thread and sync pools in a Zephyr user-mode WAMR thread |

## Setup

Using WAMR with Zephyr can be accomplished by either using the provided Docker
image, or by installing Zephyr locally. Both approaches are described below.

### Docker

The provided [Dockerfile](./Dockerfile) sets up the Zephyr SDK, `west`, a
Zephyr workspace matching the CI layout, and the wasi-sdk in `/opt/wasi-sdk`
(`$WASI_SDK_PATH`) for recompiling the samples' WASM applications. Only the ARC and x86 toolchains are
installed to keep the image reasonably small (~5 GB); add more `-t
<toolchain>` options to `setup.sh` in the Dockerfile if you need other
architectures.

The helper script [build_and_run.py](./build_and_run.py) builds
the image and runs a sample inside a container against your local checkout. It
only needs Python 3 and `docker` on the host, so it works on Linux, macOS and
Windows alike:

```shell
# build the image (only needed once)
python3 build_and_run.py --build

# build and run the simple sample on native_sim (the default simulator)
python3 build_and_run.py simple

# another simulator / another sample
python3 build_and_run.py --sim qemu_arc user-mode
```

The console only carries progress; the full `docker build`, CMake and emulator
output goes to `build/logs/`, and the tail of the relevant log is printed if a
step fails. See `--help` for details.

WAMR itself is not baked into the image. The script bind mounts the repository
at `/root/zephyrproject/modules/wasm-micro-runtime`, so the sources being built
are always the ones in your working tree and the image does not have to be
rebuilt when you change them.

To work inside the container interactively instead — the mount is required, the
module directory is empty otherwise:

```shell
docker build -t wamr-zephyr .
docker run -it --rm \
  -v "$(git rev-parse --show-toplevel)":/root/zephyrproject/modules/wasm-micro-runtime \
  wamr-zephyr
```

If you are planning to flash a device from the container, pass the device with
[`--device`](https://docs.docker.com/engine/reference/run/#runtime-privilege-and-linux-capabilities),
e.g. `--device=/dev/ttyUSB0`.

### Local Environment

Zephyr can also be set up locally. This gives you more control over which
modules and tools are installed, which can drastically reduce the required
storage compared to the Docker image. Follow the [Zephyr Getting Started
guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html),
then install the Zephyr SDK toolchains for the architectures you target.

### Workspace

WAMR is consumed as a Zephyr module, so the repository has to be visible to
`west`. The layout used by CI and by the Docker image is a
[T2 star topology](https://docs.zephyrproject.org/latest/develop/west/workspaces.html)
workspace:

```
zephyrproject/                     <- topdir
├── .west/config
├── zephyr/                        <- Zephyr source code
├── zephyr-sdk/
├── modules/
│   └── wasm-micro-runtime         <- this repository
│       ├── zephyr/module.yml      <- declares the Zephyr module
│       ├── zephyr/Kconfig         <- CONFIG_WAMR_* options
│       ├── zephyr/CMakeLists.txt  <- builds the runtime as a Zephyr library
│       └── product-mini/platforms/zephyr/<sample>/
│           ├── CMakeLists.txt     <- the application only adds its own sources
│           ├── prj.conf           <- CONFIG_WAMR_* selections for the sample
│           └── src/main.c
└── application/                   <- dummy manifest repo, holds west_lite.yml
```

Create it with the minimal manifest shipped in this tree:

```shell
export ZWS=~/zephyrproject
mkdir -p $ZWS/application $ZWS/modules
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git \
  $ZWS/modules/wasm-micro-runtime
cp $ZWS/modules/wasm-micro-runtime/product-mini/platforms/zephyr/west_lite.yml \
  $ZWS/application/west_lite.yml

cd $ZWS
west init -l --mf west_lite.yml application
west update --stats
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
```

If your checkout lives outside the workspace, keep it where it is and point
the build at it instead of moving it:

```shell
west build . -b <board> -p always -- \
  -DEXTRA_ZEPHYR_MODULES=/path/to/wasm-micro-runtime
```

## Building

With the environment set up, build any of the samples with
[`west`](https://docs.zephyrproject.org/latest/develop/west/index.html) from
the sample directory:

```shell
west build . -b <board-identifier> -p always
```

The `<board-identifier>` can be found in the [Zephyr supported boards
documentation](https://docs.zephyrproject.org/latest/boards/index.html). Board
specific Kconfig fragments go into the sample's `boards/<board>.conf`.

`WAMR_BUILD_TARGET` is derived from the board architecture by
[zephyr/CMakeLists.txt](../../../zephyr/CMakeLists.txt), so it normally does
not have to be passed. Override it to select a sub-variant (e.g. `THUMBV7`
instead of the generic `THUMB`):

```shell
west build . -b <board-identifier> -p always -- -DWAMR_BUILD_TARGET=THUMBV7
```

The list of supported targets is in the main project
[README.md](../../../README.md#supported-architectures-and-platforms).

### Running under QEMU

Emulated boards are built the same way and run with `west`:

```shell
west build . -b qemu_x86 -p always
west build -t run
```

> Press `CTRL+a, x` to exit QEMU.

Boards that are regularly exercised:

| Board                           | Arch    | Notes                                          |
| ------------------------------- | ------- | ---------------------------------------------- |
| `qemu_x86`                      | X86_32  | Default smoke test target                      |
| `qemu_arc/qemu_arc_hs`          | ARC     | Needs `arc-zephyr-elf` and `arc64-zephyr-elf`  |
| `qemu_cortex_a53`               | AARCH64 | 64-bit ARM                                     |
| `qemu_riscv32` / `qemu_riscv64` | RISCV   | AOT is not supported, add `-DWAMR_BUILD_AOT=0` |
| `qemu_xtensa`                   | XTENSA  |                                                |

AOT is not available on every architecture. Where it is not, disable it with
`-DWAMR_BUILD_AOT=0` or `CONFIG_WAMR_AOT=n`.

### Running with native_sim on Linux

[`native_sim`](https://docs.zephyrproject.org/latest/boards/native/native_sim/doc/index.html)
compiles Zephyr and the application into a native Linux executable. There is
no emulation involved, so builds and runs are fast, which makes it the
quickest way to smoke test a change to the runtime.

```shell
# 32-bit host build, WAMR_BUILD_TARGET is derived as X86_32
west build . -b native_sim -p always
./build/zephyr/zephyr.exe

# 64-bit host build, WAMR_BUILD_TARGET is derived as X86_64
west build . -b native_sim/native/64 -p always
./build/zephyr/zephyr.exe
```

`west build -t run` works as well. The 32-bit variant needs the multilib host
compiler (`gcc-multilib g++-multilib` on Debian/Ubuntu); the Docker image
already has it.

Note that `native_sim` runs with the host libc and host memory sizes, so it
will not catch problems that only show up under the tight memory constraints
of a real target.

### Flashing a device

```shell
west flash
```

`west` automatically identifies the board if it is connected to the host
machine.

## Reporting results

Every layer reports what happened, so a failure is visible both in the output
and in the exit status:

- The WASM application returns `0` on success and a distinct non-zero code per
  failure (see the `EXIT_*` defines in its source), after printing
  `ERROR: <what went wrong>`. The runtime hands that code to the Zephyr
  application, as the WASI exit code for the WASI samples and as the return
  value of the entry point for the others.
- The Zephyr application checks the call result, the exception and the module
  exit code, prints an `ERROR:` line for anything unexpected and
  `PASS: <what was verified>` once everything completed, then returns:

  | Code | Meaning |
  | --- | --- |
  | 0 | the module ran to completion and reported success |
  | 1 | the host failed: runtime init, load, instantiate, missing entry point |
  | 2 | the module faulted or returned a non-zero code |

- Each sample declares in its `sample.yaml` which `PASS:` line a successful run
  must print, so [twister](https://docs.zephyrproject.org/latest/develop/test/twister.html)
  turns that into a test verdict.

## Testing with twister

The samples are twister test cases: `sample.yaml` lists the scenarios, the
platforms they may run on and the expected console output.
[build_and_run.py](./build_and_run.py) is a thin wrapper that runs twister for
one sample on one simulator, either in the Docker image or, with `--no-docker`,
in the current environment — which is exactly what CI does:

```shell
python3 build_and_run.py --sim qemu_arc user-mode
```

To run twister directly, from the workspace:

```shell
west twister -T modules/wasm-micro-runtime/product-mini/platforms/zephyr/simple \
  -p native_sim -x EXTRA_ZEPHYR_MODULES=$PWD/modules/wasm-micro-runtime \
  --disable-warnings-as-errors
```

`--disable-warnings-as-errors` is needed because twister compiles with
`-Werror`, which the runtime is not built with in any other configuration.

### Dedicated Ztest suites

`simple`, `simple-file`, `simple-http`, and `user-mode` remain sample programs:
they demonstrate an integration and retain their console harnesses. The
dedicated `tests/platform_api`, `tests/runtime`, and `tests/usermode_faults`
applications are blocking Ztest suites that make contract assertions and let
Twister decide the verdict. The fault suite is the QEMU ARC-only isolation
lane; the other two suites retain their native and QEMU ARC lanes.

### User-mode fault ownership

`tests/runtime` owns positive and recoverable runtime workflows.
`tests/usermode_faults` owns the sole fatal override and five representative
WAMR-specific cases: the required pool, writable-module, and WAMR-global
partitions; publication into supervisor-only runtime state; and containment of
a Wasm linear-memory out-of-bounds access as a runtime trap. Zephyr owns the
generic MPU, syscall-verifier, illegal-pointer, and kernel-object permission
matrices. `tests/platform_api` remains the existing representative API suite;
it is not a Phase Two expansion of every platform API contract. These scopes
do not overlap: the fault suite neither replaces Zephyr's generic matrices nor
broadens the platform API suite's representative role.

Run these commands from `product-mini/platforms/zephyr` to use the repository
Docker environment (the default):

```bash
python3 build_and_run.py --sim native_sim tests/platform_api
python3 build_and_run.py --sim qemu_arc tests/runtime
python3 build_and_run.py --sim qemu_arc tests/usermode_faults
```

In an already configured local Zephyr workspace, use the same interface with
`--no-docker`; this is the interface CI uses inside its Zephyr container:

```bash
python3 build_and_run.py --no-docker --sim native_sim tests/platform_api
python3 build_and_run.py --no-docker --sim qemu_arc tests/runtime
python3 build_and_run.py --no-docker --sim qemu_arc tests/usermode_faults
```

Each invocation writes its streamed log to
`build/logs/<test-root>-<sim>.log` and the Twister report, including individual
Ztest case records, to `build/twister-<test-root>-<sim>/twister.json`. For
example, `tests/platform_api` on `native_sim` uses
`build/twister-tests-platform_api-native_sim/`. The wrapper forwards Twister's
exit status; do not infer a result from console text.

### Informational coverage

Coverage is a manually requested measurement job, not a pass threshold. The
pinned CI baseline remains Zephyr 3.7.0. It runs three exact `native_sim`
platform-API scenarios sequentially because WAMR's generated version header
races when configurations share a checkout. The second scenario changes only
`CONFIG_THREAD_STACK_INFO=y`; the third is the isolated mocked-error scenario,
which compiles only its FFF fixture and leaves normal builds on direct calls.
See [the focused platform-API guide](tests/platform_api/README.md) for the
mock boundary and error-path classification.

```bash
python3 build_and_run.py --no-docker --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.kernel tests/platform_api
python3 build_and_run.py --no-docker --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.kernel_stack_info tests/platform_api
python3 build_and_run.py --no-docker --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.mocked_errors tests/platform_api
python3 coverage_report.py \
  build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage.json \
  build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/coverage.json \
  build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-mocked-errors-e2d36566-coverage/coverage.json
```

The wrapper uses the Zephyr 3.7 Twister options `--coverage`,
`--coverage-basedir`, `--coverage-tool gcovr`, and `--coverage-formats
html,xml`. The individual trace artifacts are:

- `build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage/`
  and its `coverage.json`;
- `build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/coverage/`
  and its `coverage.json`.
- `build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-mocked-errors-e2d36566-coverage/coverage/`
  and its `coverage.json`.

`coverage_report.py` publishes the focused aggregate under
`build/coverage-zephyr-platform-aggregate/`, including HTML, Cobertura XML,
JSON, text summaries, and `inputs.txt`. The aggregate is union evidence from
three real builds, not coverage from a single binary; the mocked-errors trace
is not folded into either ordinary configuration. CI uploads all three
individual coverage directories and raw traces with this aggregate as
`zephyr-wamr-coverage-native-sim`.

### 2026-09-08 Zephyr 4.4 mocked-error compatibility probes

The focused native_sim probe ran ordinary `wamr.zephyr.platform_api.kernel`
and isolated `wamr.zephyr.platform_api.mocked_errors` on these exact revisions:

- `v4.4.0`: `684c9e8f32e4373a21098559f748f06915f950c9`.
- `v4.4.1`: `1f6485eca25431b5ff27ce9a754218c9e559bbbb`.

| Release | Scenario | JSON passed/skipped | JSON execution time | Combined Twister wall time |
| --- | --- | ---: | ---: | ---: |
| 4.4.0 | kernel | 67/19 | 0.08 s | 21.04 s |
| 4.4.0 | mocked_errors | 3/0 | 0.01 s | same invocation |
| 4.4.1 | kernel | 67/19 | 0.07 s | 21.26 s |
| 4.4.1 | mocked_errors | 3/0 | 0.01 s | same invocation |

Each release passed 2/2 executed configurations with no failed, error, or null
testcase statuses; testcase identities and statuses match across releases.
Counts match the pinned 3.7 focused evidence below. The 19 kernel skips are
existing configuration-dependent cases; no skips or expected results changed.
The ordinary timeout-return and timeout-mutex-reacquisition tests both passed.
The pre-existing 4.4.x condvar timeout workaround remains unchanged: both
probed upstream `kernel/condvar.c` implementations still relock only for a
zero wait result. No new WAMR product or test-harness regression was found,
and no compatibility code or official Zephyr 3.7.0 pin changed.

The first sandboxed 4.4.0 command failed before building because Python's
multiprocessing manager could not create its local listener (`PermissionError`,
then `EOFError`). A minimal manager-only program reproduced that environment
failure; the identical probe succeeded with approved local socket access.
The 4.4.1 probe used the same access. No toolchain/execution-harness failure
occurred after that correction. This is an environment restriction, not a
Zephyr/WAMR regression.

All four retained build logs contain compiler warnings from `bh_common.c`:
implicit `strtok_r` declaration and int-to-pointer conversion. These diagnostics
also occur in the pinned Zephyr 3.7 baseline, so they are not a new 4.4
regression. The kernel logs also contain unused-function warnings, and all
four logs contain a CMake warning about unused `TC_NAME`. Twister reports zero
warning configurations, which does not mean compiler output is warning-free;
all testcase results remain valid. Completed review found `bh_strtok_r` and its
affected line unexecuted in all three coverage traces. The pre-existing
declaration/ABI hazard is not introduced or worsened here and does not block
these cleanup tests; track its declaration/configuration fix and an executed
tokenizer regression test separately. Unused-code warnings are in unchanged
ordinary tests, and `TC_NAME` is supplied by Twister.

The disposable workspaces use Python 3.12 and Zephyr SDK 1.0.1. Reports are
`/tmp/wamr-zephyr-4.4-probe/{4.4.0,4.4.1}/outputs/wamr-mocked-errors/twister.json`;
reusable commands are in the
[focused probe guide](tests/platform_api/README.md#zephyr-44-focused-probes).
This probe covers only the two native scenarios, not a new full 4.4 matrix or
pin upgrade.

### 2026-09-07 final Zephyr 3.7 evidence

At revision `2744aaf5`, the complete eleven-entry pinned-v3.7.0 matrix passed:
20 configurations passed in total (19 executed plus the `simple-http`
build-only configuration), and four additional configurations were statically
filtered; its Twister JSON reports contain 326 passed and 60 skipped testcase
records, with zero failed, error, or null statuses. Per-entry testcase
pass/skip and wall times were:

| Entry | JSON testcases | Wall time |
| --- | ---: | ---: |
| `native_sim simple` | 1/0 | 9.46 s |
| `qemu_arc simple` | 1/0 | 9.80 s |
| `native_sim simple-file` | 1/0 | 10.66 s |
| `native_sim simple-http` | 0/1 | 8.21 s |
| `qemu_arc user-mode` | 2/0 | 34.37 s |
| `qemu_arc user-mode-multi-thread` | 1/0 | 17.56 s |
| `native_sim tests/platform_api` | 137/38 | 18.35 s |
| `qemu_arc tests/platform_api` | 153/21 | 27.10 s |
| `native_sim tests/runtime` | 8/0 | 6.91 s |
| `qemu_arc tests/runtime` | 17/0 | 24.52 s |
| `qemu_arc tests/usermode_faults` | 5/0 | 76.93 s |

Host discovery passed 33/33 in 0.042 s. The separate raw coverage traces
passed kernel 67/19 in 7.22 s, kernel-stack-info 67/19 in 7.38 s, and
mocked-errors 3/0 in 6.89 s. Their aggregate has 531/608 lines and 228/332
branches; it is measurement only, not a threshold. `zephyr_thread.c` records
463/538 lines and 219/322 branches. Compared with the previous 519/625-line,
224/338-branch aggregate, the newly taken failure edges are thread-object
allocation (`zephyr_thread.c:862`), thread-data allocation (`:870`), and join
failure (`:1001`); invariant cleanup removes unreachable native-initializer
and `k_thread_create()` recovery branches, reducing the denominators. All
three raw paths are retained in the aggregate `inputs.txt`.

The `af729dc6`, `d286e15c`, and `e2d36566` suffixes are the first eight hex
characters of the scenario SHA-256. They keep same-slug scenarios distinct in this small
trusted scenario set, which makes them collision-resistant here without
claiming a collision-free naming scheme.

Task 6 established the Zephyr 3.7.0 pinned baseline on 2026-08-27. Task 7
reran the affected 3.7 roots and both focused coverage scenarios on
2026-08-28 after landing the 4.4-compatible fixes:

| File | Lines | Exec | Line cover | Branches | Taken | Branch cover |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `core/shared/platform/zephyr/platform_internal.h` | 7 | 7 | 100% | 0 | 0 | -- |
| `core/shared/platform/zephyr/zephyr_platform.c` | 51 | 49 | 96% | 8 | 8 | 100% |
| `core/shared/platform/zephyr/zephyr_thread.c` | 530 | 441 | 83% | 316 | 211 | 66% |
| `core/shared/platform/zephyr/zephyr_time.c` | 10 | 10 | 100% | 2 | 1 | 50% |
| Total | 598 | 507 | 84% | 326 | 220 | 67% |

Task 7 reran the affected ordinary roots on the pinned 3.7.0 baseline:

| Group | Suites passed | Testcases passed | Testcases skipped | Failed | Error |
| --- | ---: | ---: | ---: | ---: | ---: |
| `native_sim simple-file` | 1 | 1 | 0 | 0 | 0 |
| `native_sim simple-http` | 1 | 0 | 1 | 0 | 0 |
| `native_sim tests/platform_api` | 2 | 126 | 28 | 0 | 0 |
| `qemu_arc/qemu_arc_hs tests/platform_api` | 2 | 140 | 16 | 0 | 0 |
| `qemu_arc/qemu_arc_hs user-mode` | 2 | 2 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs user-mode-multi-thread` | 1 | 1 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs tests/runtime` | 2 | 14 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs tests/usermode_faults` | 5 | 5 | 0 | 0 | 0 |

The complete 11-entry matrix verdict below combines those fresh Task 7 reruns
with the three unaffected 2026-08-27 artifacts preserved from the initial full
run. All 11 `twister.json` files contributing to this table reported zero
null-status testcase records.

| Entry | Evidence source | Suites passed | Testcases passed | Testcases skipped | Failed | Error |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `native_sim simple` | preserved from the initial 2026-08-27 full run | 1 | 1 | 0 | 0 | 0 |
| `qemu_arc simple` | preserved from the initial 2026-08-27 full run | 1 | 1 | 0 | 0 | 0 |
| `native_sim simple-file` | fresh rerun on 2026-08-28 after the Task 7 fixes | 1 | 1 | 0 | 0 | 0 |
| `native_sim simple-http` | fresh rerun on 2026-08-28 after the Task 7 fixes | 1 | 0 | 1 | 0 | 0 |
| `qemu_arc user-mode` | fresh rerun on 2026-08-28 after the Task 7 fixes | 2 | 2 | 0 | 0 | 0 |
| `qemu_arc user-mode-multi-thread` | fresh rerun on 2026-08-28 after the Task 7 fixes | 1 | 1 | 0 | 0 | 0 |
| `native_sim tests/platform_api` | fresh rerun on 2026-08-28 after the Task 7 fixes | 2 | 126 | 28 | 0 | 0 |
| `qemu_arc tests/platform_api` | fresh rerun on 2026-08-28 after the Task 7 fixes | 2 | 140 | 16 | 0 | 0 |
| `native_sim tests/runtime` | preserved from the initial 2026-08-27 full run | 1 | 8 | 0 | 0 | 0 |
| `qemu_arc tests/runtime` | fresh rerun on 2026-08-28 after the Task 7 fixes | 2 | 14 | 0 | 0 | 0 |
| `qemu_arc tests/usermode_faults` | fresh rerun on 2026-08-28 after the Task 7 fixes | 5 | 5 | 0 | 0 | 0 |

| Platform group | Suites passed | Testcases passed | Testcases skipped | Failed | Error |
| --- | ---: | ---: | ---: | ---: | ---: |
| `native_sim` | 6 | 136 | 29 | 0 | 0 |
| `qemu_arc/qemu_arc_hs` | 13 | 163 | 16 | 0 | 0 |
| Zephyr matrix total | 19 | 299 | 45 | 0 | 0 |

The 2026-08-27 full-matrix run above stayed on the pinned Zephyr 3.7.0
baseline. Task 7 then reran only the previously affected roots and focused
coverage scenarios on 2026-08-28 in a disposable Zephyr 4.4.0 probe
environment rooted at `/tmp/wamr-zephyr-4.4-probe`; those compatibility reruns
do not change the repository pin.

Exact disposable 4.4.0 probe inventory:

- Zephyr `v4.4.0` (`684c9e8f32e4373a21098559f748f06915f950c9`)
- `west` 1.5.0
- Python 3.12.3
- `gcovr` 8.6
- Zephyr SDK 1.0.1
- `gperf` 3.1 at `/tmp/task7-gperf/usr/bin/gperf`

All commands below ran from `/home/tl/projects/wasm-micro-runtime`.

Exact affected 4.4 ordinary rerun commands:

```sh
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/simple-file --platform native_sim -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-simple-file-native_sim --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/simple-http --platform native_sim -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-simple-http-native_sim --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/tests/platform_api --platform native_sim -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-tests-platform_api-native_sim --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/tests/platform_api --platform qemu_arc/qemu_arc_hs -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-tests-platform_api-qemu_arc --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/user-mode --platform qemu_arc/qemu_arc_hs -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-user-mode-qemu_arc --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/user-mode-multi-thread --platform qemu_arc/qemu_arc_hs -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-user-mode-multi-thread-qemu_arc --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/tests/runtime --platform qemu_arc/qemu_arc_hs -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-tests-runtime-qemu_arc --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/tests/usermode_faults --platform qemu_arc/qemu_arc_hs -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --outdir /tmp/wamr-zephyr-4.4-probe/outputs/ordinary/twister-tests-usermode_faults-qemu_arc --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
```

Exact affected 4.4 focused coverage and aggregate commands:

```sh
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/tests/platform_api --platform native_sim -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --test wamr.zephyr.platform_api.kernel --coverage --coverage-basedir /home/tl/projects/wasm-micro-runtime --coverage-tool gcovr --coverage-formats html,xml --outdir /tmp/wamr-zephyr-4.4-probe/outputs/coverage/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 west twister -T product-mini/platforms/zephyr/tests/platform_api --platform native_sim -x EXTRA_ZEPHYR_MODULES=/home/tl/projects/wasm-micro-runtime --test wamr.zephyr.platform_api.kernel_stack_info --coverage --coverage-basedir /home/tl/projects/wasm-micro-runtime --coverage-tool gcovr --coverage-formats html,xml --outdir /tmp/wamr-zephyr-4.4-probe/outputs/coverage/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
env PATH=/tmp/task7-gperf/usr/bin:/tmp/wamr-zephyr-4.4-probe/.venv/bin:$PATH python3 product-mini/platforms/zephyr/coverage_report.py --root /home/tl/projects/wasm-micro-runtime --output-dir /tmp/wamr-zephyr-4.4-probe/outputs/coverage-zephyr-platform-aggregate-4.4 /tmp/wamr-zephyr-4.4-probe/outputs/coverage/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage.json /tmp/wamr-zephyr-4.4-probe/outputs/coverage/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/coverage.json
```

QEMU ARC remains behavioral userspace evidence rather than a coverage lane.
On 2026-08-28, Task 7 reran every previously affected Zephyr 4.4.0 root
against the official signed `v4.4.0` tag
(`684c9e8f32e4373a21098559f748f06915f950c9`) in
`/tmp/wamr-zephyr-4.4-probe`. Before rerunning the userspace roots, Task 7
installed `gperf` at `/tmp/task7-gperf/usr/bin/gperf` (`GNU gperf 3.1`) and
reran the blocked outdirs with `--clobber-output`; the rebuilt 4.4 userspace
`CMakeCache.txt` files now record
`GPERF:FILEPATH=/tmp/task7-gperf/usr/bin/gperf`. Remaining optional misses were
`CMAKE_C_COMPILER_CLANG_SCAN_DEPS`, `CMAKE_DLLTOOL`, `CMAKE_TAPI`, `IMGTOOL`,
`PAHOLE`, `PTY_INTERFACE`, and `PUNCOVER`, and none of them blocked these
roots.

The resolved upstream issue is a Zephyr 4.4.x condvar timeout regression. The
public `k_condvar_wait()` contract says the mutex is released while blocked and
re-acquired before the call returns. In Zephyr 4.4.0 through 4.4.2,
`kernel/condvar.c` reacquires the mutex only when `ret == 0`, so timeout
returns `-EAGAIN` with the mutex still unlocked. WAMR's
`os_cond_reltimedwait()` and userspace `os_cond_wait_user()` paths depended on
that public contract, so
`platform_sync.condition_timed_wait_returns` failed on `os_mutex_unlock()`
after timeout and both native focused coverage scenarios failed the same way.
Upstream commit `5c6c6837cc4026a70e670c265fc4d1e3b1f16379` fixes the
behavior after the 4.4 release line. Task 7 therefore adds a
`[4.4.0, 4.5.0)` workaround that relocks only on `-EAGAIN`. Zephyr 3.7.0
cannot double-lock because its `k_condvar_wait()` implementation reacquires
unconditionally before return; fixed 4.5+ releases cannot double-lock because
the helper compiles to a no-op outside the affected 4.4.x interval.

Task 7 also resolved three local 4.4 compatibility defects separately:

| Root | Local defect | Final fix |
| --- | --- | --- |
| `simple-file/native_sim` | obsolete `CONFIG_ETH_NATIVE_POSIX=n`; native runtime also needed heap and file-descriptor capacity | remove `CONFIG_ETH_NATIVE_POSIX=n`, add `CONFIG_HEAP_MEM_POOL_SIZE=1024`, add `CONFIG_ZVFS_OPEN_MAX=4` |
| `simple-http/native_sim` | native socket sample linked with unresolved `k_calloc` | add `CONFIG_HEAP_MEM_POOL_SIZE=1024` |
| `tests/platform_api/qemu_arc/qemu_arc_hs` | `test_common.c` used `os_self_thread()` without the public prototype | include `platform_api_vmcore.h` |

The `simple-file` fix is minimal: removing only `ETH_NATIVE_POSIX` still fails
with unresolved `k_calloc`, and adding heap without `CONFIG_ZVFS_OPEN_MAX=4`
still fails at `all file descriptor slots are in use (max = 0)`.

The final affected 4.4 rerun set passed cleanly:

| Root | Suites passed | Testcases passed | Testcases skipped | Testcases not run | Failed | Error |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `native_sim simple-file` | 1 | 1 | 0 | 0 | 0 | 0 |
| `native_sim simple-http` | 1 build-only | 0 | 0 | 1 | 0 | 0 |
| `native_sim tests/platform_api` | 2 | 126 | 28 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs tests/platform_api` | 2 | 140 | 16 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs user-mode` | 2 | 2 | 0 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs user-mode-multi-thread` | 1 | 1 | 0 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs tests/runtime` | 2 | 14 | 0 | 0 | 0 | 0 |
| `qemu_arc/qemu_arc_hs tests/usermode_faults` | 5 | 5 | 0 | 0 | 0 | 0 |

Affected 4.4 rerun total: 15 executed configurations passed, 1 build-only
configuration completed without failure, 289 testcases passed, 44 skipped, 1
`not run`, and 0 failed or errored.

The two 4.4 native coverage scenarios now pass as well:

| Scenario | Suites passed | Testcases passed | Testcases skipped | Failed | Error |
| --- | ---: | ---: | ---: | ---: | ---: |
| `wamr.zephyr.platform_api.kernel` | 1 | 63 | 14 | 0 | 0 |
| `wamr.zephyr.platform_api.kernel_stack_info` | 1 | 63 | 14 | 0 | 0 |

The separate 4.4 aggregate at
`/tmp/wamr-zephyr-4.4-probe/outputs/coverage-zephyr-platform-aggregate-4.4/`
records the post-fix production totals below:

| File | Lines | Exec | Line cover | Branches | Taken | Branch cover |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `core/shared/platform/zephyr/platform_internal.h` | 7 | 7 | 100% | 0 | 0 | -- |
| `core/shared/platform/zephyr/zephyr_platform.c` | 51 | 49 | 96% | 8 | 8 | 100% |
| `core/shared/platform/zephyr/zephyr_thread.c` | 532 | 443 | 83% | 318 | 213 | 67% |
| `core/shared/platform/zephyr/zephyr_time.c` | 10 | 10 | 100% | 2 | 1 | 50% |
| Total | 600 | 509 | 84% | 328 | 222 | 67% |

Zephyr 3.7.0 remains the supported and pinned baseline. The Zephyr 4.4.0
results are compatibility evidence and do not change the repository pin. No
WASI, AOT, or broader I/O expansion was added in Task 7. Task 6 still provided
the gcovr aggregate fix and the sync-pool permission fix for
`platform_sync.test_prepared_native_sync_objects_require_inherited_permissions`.

The pilot supports `native_sim` and `qemu_arc/qemu_arc_hs`. `native_sim` runs
the kernel scenarios only and is a fast host smoke target, not a userspace
isolation claim. On QEMU ARC, the platform API and runtime suites run their
kernel scenario and applicable userspace scenario; the fault suite runs its
QEMU-only userspace scenario. The test configurations deliberately cover the
interpreter with the global heap pool; they do not enable AOT or exercise
alternate allocation modes.

Some named contracts are expected to skip while port work is outstanding:

- On `native_sim`, the platform userspace scenario is filtered out; concurrent
  and repeated WAMR thread creation can block, and the CPU-time counter does
  not advance during the busy-work contract.
- On QEMU ARC, the corresponding repeated/concurrent thread cases can block.
  In userspace, `k_thread_runtime_stats_get()` reaches privileged
  `arch_irq_lock()`, so the CPU-time contracts are skipped. Synchronization
  APIs use the prepared native-object pool and their applicable behavioral
  cases run in both kernel and user contexts.

These are explicit, named skips that retain their test bodies; they are not
passing demonstrations. A QEMU ARC user-mode fault suite remains active and
verifies the five representative WAMR-specific boundaries described above.

Phase Two adds those five representative WAMR-specific fault cases and coverage
measurement. Comprehensive generic MPU, verifier, and illegal-pointer matrices
remain Zephyr-owned. Phase Three, not Phase Two, owns exhaustive platform API
expansion. Filesystem, sockets, AOT, alternate allocators, stress, and physical-
board testing remain lower-priority future work.

## Adding a new sample

1. Create a directory next to the existing samples with the usual Zephyr
   application layout: `CMakeLists.txt`, `prj.conf`, `src/`, and optionally
   `boards/<board-identifier>.conf`. Keep `CMakeLists.txt` to the usual
   Zephyr `find_package`, `project`, and `target_sources` calls;
   the runtime comes from the module, so nothing WAMR specific belongs there.
2. Select the runtime features with `CONFIG_WAMR_*` in `prj.conf`, as described
   in [Configuring the runtime](#configuring-the-runtime).
3. If the sample needs a Zephyr module that the workspace does not have yet —
   littlefs, mbedTLS, an HAL for a new SoC — add it to
   [west_lite.yml](./west_lite.yml). That manifest is deliberately minimal: it
   pulls Zephyr and only the modules the samples actually use, which keeps both
   the CI setup and the Docker image small. Copy the `name`, `revision` and
   `path` of the project from Zephyr's own `west.yml` so that the versions
   match:

   ```yaml
   - name: littlefs
     url: https://github.com/zephyrproject-rtos/littlefs
     revision: 408c16a909dd6cf128874a76f21c793798c9e423
     path: modules/fs/littlefs
   ```

   Existing workspaces need a `west update` afterwards, and the Docker image
   has to be rebuilt (`python3 build_and_run.py --build`).
4. Add a `sample.yaml` declaring the twister scenarios: the platforms the
   sample may run on and the `PASS:` line its console output must carry. Follow
   the exit code convention in
   [Reporting results](#reporting-results) so that a failure is visible in the
   exit status too.
5. Add a row to the [Samples](#samples) table and a `README.md` in the sample
   directory covering only what is specific to it.
6. If the sample runs on `native_sim` or QEMU, add it to the matrix in
   [.github/workflows/compilation_on_zephyr.yml](../../../.github/workflows/compilation_on_zephyr.yml)
   so that it is built and run by CI.

## Configuring the runtime

The runtime is configured through the `CONFIG_WAMR_*` Kconfig options defined
in [zephyr/Kconfig](../../../zephyr/Kconfig). Set them in the sample's
`prj.conf`:

```conf
CONFIG_WAMR=y
CONFIG_WAMR_INTERP=y
CONFIG_WAMR_AOT=y
CONFIG_WAMR_LIBC_BUILTIN=y
CONFIG_WAMR_GLOBAL_HEAP_POOL=y
CONFIG_WAMR_GLOBAL_HEAP_SIZE=131072
```

`CONFIG_WAMR=n` (the default) leaves the runtime out of the image entirely.

Each option maps onto the corresponding `WAMR_BUILD_*` CMake variable that the
regular WAMR build scripts use. Options can still be overridden on the CMake
command line (`-DWAMR_BUILD_AOT=0`), which is handy for one-off builds, but
`prj.conf` is the place to record a configuration.

### Exposing a new WAMR_BUILD_XYZ as CONFIG_WAMR_XYZ

Two edits are needed.

1. Declare the option in [zephyr/Kconfig](../../../zephyr/Kconfig), inside the
   `if WAMR` block. Mirror the runtime default and add the dependencies that
   the runtime itself requires:

   ```kconfig
   config WAMR_LIB_WASI_THREADS
	bool "wasi-threads library"
	depends on WAMR_LIBC_WASI
	help
	  Provide the wasi-threads library to WASM modules.
   ```

2. Map it in [zephyr/CMakeLists.txt](../../../zephyr/CMakeLists.txt) with the
   `wamr_option_from_kconfig` macro, which translates the undefined-when-off
   Kconfig boolean into the plain `0`/`1` the runtime expects:

   ```cmake
   wamr_option_from_kconfig (LIB_WASI_THREADS)
   ```

Non-boolean options are copied over directly, guarded by the boolean that
enables them — see how `CONFIG_WAMR_GLOBAL_HEAP_SIZE` becomes
`WAMR_BUILD_GLOBAL_HEAP_SIZE`. Keep the Kconfig name equal to the
`WAMR_BUILD_*` suffix so the mapping stays mechanical.
