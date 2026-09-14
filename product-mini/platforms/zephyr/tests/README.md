# WAMR Zephyr tests

This directory contains focused tests for the WAMR Zephyr port. The suites are
demonstrations of WAMR integration behavior, not replacements for Zephyr's own
kernel, verifier, or MPU test matrices.

All current runtime scenarios use the interpreter and
`WAMR_BUILD_GLOBAL_HEAP_POOL`. AOT, alternate allocators, filesystem and socket
APIs, randomized/timing-dependent/load stress testing, and physical-board
coverage are outside this test set.

## 2026-09-13 final meaningful-branch-coverage evidence

All evidence in this section was regenerated on code revision `82ec4ca4`,
after restoring legacy rwlock compatibility for the ordinary WASI samples.
Zephyr 3.7.0 remains the official pin. Host discovery passed 35/35 tests in
0.048 s. Every one of the eleven ordinary entries exited zero, with no failed
or errored configurations and no failed, error, or null testcase statuses.

| Ordinary entry | Passed/static-filter configurations | JSON passed/skipped cases | JSON `execution_time` per configuration | Twister wall time |
| --- | ---: | ---: | --- | ---: |
| `native_sim simple` | 1/0 | 1/0 | 2.60 s | 9.39 s |
| `qemu_arc simple` | 1/0 | 1/0 | 1.03 s | 11.03 s |
| `native_sim simple-file` | 1/0 | 1/0 | 2.61 s | 10.38 s |
| `native_sim simple-http` | 1/0 | 0/1 | 0.00 s | 7.72 s |
| `qemu_arc user-mode` | 2/0 | 2/0 | normal/prebuilt: 1.03/1.03 s | 34.48 s |
| `qemu_arc user-mode-multi-thread` | 1/0 | 1/0 | 1.05 s | 18.31 s |
| `native_sim tests/platform_api` | 3/1 | 143/40 | mocks/kernel/stack-info: 0.01/0.09/0.09 s | 18.37 s |
| `qemu_arc tests/platform_api` | 2/2 | 160/22 | kernel/userspace: 1.43/2.46 s | 30.50 s |
| `native_sim tests/runtime` | 1/1 | 8/0 | 0.01 s | 8.59 s |
| `qemu_arc tests/runtime` | 2/0 | 17/0 | kernel/userspace: 1.05/1.13 s | 30.99 s |
| `qemu_arc tests/usermode_faults` | 5/0 | 5/0 | 1.03 s each | 85.52 s |
| Total | 20/4 | 339/63 | — | — |

The 20 passed configurations include 19 executed and the unchanged HTTP
build-only configuration; its skipped case is not an HTTP runtime pass.
The case totals count records retained in `twister.json`, not Twister console
counts that also include statically filtered cases. Each report is
`../build/twister-<root-with-slashes-replaced-by-hyphens>-<sim>/twister.json`,
with the corresponding wrapper log in
`../build/logs/<root-with-slashes-replaced-by-hyphens>-<sim>.log`.
Run each table entry with `python3 ../build_and_run.py --sim <sim> <root>`
from this directory; the wrapper resolves roots relative to the Zephyr platform.

Against the 2026-09-07 matrix below, native/ARC platform-root wall time changed
18.35/27.10 s to 18.37/30.50 s; native/ARC runtime changed 6.91/24.52 s to
8.59/30.99 s. These are observed build-and-test timings, not performance
benchmarks; compatibility probes also ran on the host during this matrix.
Every ordinary entry remains below two minutes, so no dedicated stress lane
or scenario split is needed.

### Zephyr 4.4.0 and 4.4.1 probes

Both isolated platform-API probes passed 5/5 executed configurations with
three static filters and 303 passed/62 skipped testcase records. Exact tags:
`v4.4.0` (`684c9e8f32e4373a21098559f748f06915f950c9`) and
`v4.4.1` (`1f6485eca25431b5ff27ce9a754218c9e559bbbb`).

| Scenario/target | Passed/skipped | 4.4.0 `execution_time` | 4.4.1 `execution_time` |
| --- | ---: | ---: | ---: |
| Native mocked errors | 3/0 | 0.01 s | 0.01 s |
| Native kernel | 70/20 | 0.08 s | 0.08 s |
| Native stack info | 70/20 | 0.09 s | 0.08 s |
| ARC kernel | 71/19 | 1.43 s | 1.41 s |
| ARC userspace | 89/3 | 1.99 s | 2.03 s |
| Combined Twister wall time | — | 75.06 s | 69.44 s |

Reports are
`/tmp/wamr-zephyr-4.4-probe/<version>/outputs/meaningful-branch-coverage/twister.json`.
The disposable setup uses Python 3.12.3, west 1.5.0, SDK 1.0.1 ARC/x86 GNU
toolchains and SDK host tools, and gperf 3.1. Only disposable manifests were
created; the repository's manifest and pin did not change. From the repository
root, use this command for each `version` (`4.4.0` or `4.4.1`):

```bash
env PATH=/tmp/wamr-zephyr-4.4-probe/$version/.venv/bin:$PATH \
  ZEPHYR_BASE=/tmp/wamr-zephyr-4.4-probe/$version/zephyr \
  ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  ZEPHYR_SDK_INSTALL_DIR=/tmp/wamr-zephyr-4.4-probe/zephyr-sdk-1.0.1 \
  west twister -T product-mini/platforms/zephyr/tests/platform_api \
  -p native_sim -p qemu_arc/qemu_arc_hs \
  -x EXTRA_ZEPHYR_MODULES="$PWD" \
  --outdir /tmp/wamr-zephyr-4.4-probe/$version/outputs/meaningful-branch-coverage \
  --inline-logs --clobber-output --disable-warnings-as-errors --jobs 1
```

The timeout-mutex-reacquisition case passes in all four non-mocked scenarios
on both releases. Direct source reinspection confirms both upstream condvar
implementations still relock only after a zero wait result; the existing
`[4.4.0, 4.5.0)` timeout relock remains unchanged and necessary. The initial
sandboxed 4.4.0 attempt failed before builds at a local multiprocessing socket;
a manager-only reproducer confirmed the environment restriction and approved
identical execution passed. No product or harness correction was needed.
Both probes report zero failed/errored/Twister-warning configurations, but
existing `strtok_r` declaration/conversion, unused-helper and `TC_NAME` build
warnings remain. These probes are not a full 4.4 CI matrix or a pin upgrade.

## Deterministic lifecycle stress and current evidence

The platform and runtime roots include deterministic stress for WAMR thread and
synchronization lifecycles: slot reuse and saturation/recovery, competing
join/detach order, mutex/condition pool recovery, timed-wait mutex
reacquisition, and teardown with active synchronization.  They use named,
bounded iteration counts (normally 8--32) and semaphores/atomics to establish
each phase; finite waits are deadlock guards, never scheduling mechanisms.

Operational contracts use `WAMR_CONTEXT_TEST()`/`WAMR_CONTEXT_TEST_F()` so the
same body runs as a supervisor caller in the kernel scenarios and as a user
caller on QEMU ARC. Pool provisioning, object-permission, and deliberately
missing-grant checks remain supervisor-controlled plain `ZTEST` cases: they
exercise embedding infrastructure rather than a public user-call contract.

Historical ordinary-root wall times were 13.64 s (`platform_api` native),
27.77 s (`platform_api` QEMU ARC), 7.08 s (`runtime` native), and 25.72 s
(`runtime` QEMU ARC). They are far below the two-to-three-minute placement
ceiling, so every deterministic case remains in its existing ordinary required
scenario; no dedicated scenario, Kconfig switch, or wrapper lane is needed.

The focused native coverage measurement runs the three commands in the
Informational coverage section sequentially, then aggregates their separate
`coverage.json` inputs. The ordinary kernel and stack-information traces remain
raw configuration evidence; the third trace is the isolated mocked-error build
for documented but non-deterministic allocation and join failures. It measures
WAMR cleanup and recovery, not Zephyr correctness. The kernel-only aggregate
cannot enter the QEMU-only userspace registered-thread-slot path; its direct
saturation and recovery test is the corresponding behavioral evidence. The
focused [platform-API guide](platform_api/README.md) records the classification,
production seams, impossible-event invariants, and selection limits.

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
[focused probe guide](platform_api/README.md#zephyr-44-focused-probes).
This probe covers only the two native scenarios, not a new full 4.4 matrix or
pin upgrade.

### 2026-09-07 final Zephyr 3.7 evidence

The complete pinned-v3.7.0 matrix and host discovery were rerun at revision
`2744aaf5`. All eleven Twister reports have zero failed, error, or null
testcase statuses. The table reports Twister configuration pass/static-filter
counts, JSON testcase pass/skip records, and Twister wall time:

| Entry | Configurations | JSON testcases | Wall time |
| --- | ---: | ---: | ---: |
| `native_sim simple` | 1/0 | 1/0 | 9.46 s |
| `qemu_arc simple` | 1/0 | 1/0 | 9.80 s |
| `native_sim simple-file` | 1/0 | 1/0 | 10.66 s |
| `native_sim simple-http` | 1/0 | 0/1 | 8.21 s |
| `qemu_arc user-mode` | 2/0 | 2/0 | 34.37 s |
| `qemu_arc user-mode-multi-thread` | 1/0 | 1/0 | 17.56 s |
| `native_sim tests/platform_api` | 3/1 | 137/38 | 18.35 s |
| `qemu_arc tests/platform_api` | 2/2 | 153/21 | 27.10 s |
| `native_sim tests/runtime` | 1/1 | 8/0 | 6.91 s |
| `qemu_arc tests/runtime` | 2/0 | 17/0 | 24.52 s |
| `qemu_arc tests/usermode_faults` | 5/0 | 5/0 | 76.93 s |
| Total | 20 (19 executed + 1 build-only)/4 | 326/60 | — |

The 20 passed configurations consist of 19 executed configurations plus the
`simple-http` build-only configuration; four additional configurations were
statically filtered. The JSON testcase totals above remain 326 passed and 60
skipped.

Host discovery passed 33/33 tests in 0.042 s. Three independent coverage
traces also passed: kernel 67/19 in 7.22 s, kernel-stack-info 67/19 in
7.38 s, and mocked-errors 3/0 in 6.89 s (each value is JSON testcase
passed/skipped). Their raw inputs are listed, in that order, in
`build/coverage-zephyr-platform-aggregate/inputs.txt`; the aggregate contains
`index.html`, `coverage.xml`, `coverage.json`, `line-summary.txt`, and
`branch-summary.txt`.

The three-trace raw aggregate is 531/608 executed/total lines and 228/332
taken/total branches (87% and 68%, respectively); these are measurements, not
thresholds. `zephyr_thread.c` contributes 463/538 lines and 219/322 branches.
Against the prior 519/625-line and 224/338-branch aggregate, executed lines
rose by 12 and taken branches by 4 while the source totals fell by 17 lines
and 6 branches. The mocked trace takes the failure edges at
`zephyr_thread.c:862` (thread-object allocation), `:870` (thread-data
allocation), and `:1001` (join); the invariant cleanup removed unreachable
native-initializer and `k_thread_create()` recovery branches. No percentage is
a pass condition.

Fresh 2026-08-30 verification on the pinned Zephyr 3.7.0 image ran all eleven
ordinary entries plus host discovery. Host discovery passed 29/29 tests. The
eleven `twister.json` files contain 324 passed and 51 skipped testcase records,
with zero failed, error, or null statuses. The two historical focused coverage scenarios
passed 68 testcase records and skipped 16, with the same zero-status-failure
check.

Compatibility probes use official Zephyr `v4.4.0`
(`684c9e8f32e4373a21098559f748f06915f950c9`) and `v4.4.1`
(`1f6485ec`) in disposable `/tmp/wamr-zephyr-4.4-probe/{4.4.0,4.4.1}`
workspaces, with Python 3.12 and Zephyr SDK 1.0.1; the repository's 3.7.0 pin
is unchanged. For each version, `platform_api` passed on native_sim (136
passed/32 skipped) and QEMU ARC (152/18), while `runtime` passed on native_sim
(8/0) and QEMU ARC (17/0), all with zero failed, error, and null statuses.
The known 4.4.x `k_condvar_wait()` timeout regression remains handled by the
existing bounded `[4.4.0, 4.5.0)` `-EAGAIN` relock workaround. This probe also
found and fixed a separate test-only declaration regression: the userspace
runtime workflow now includes the public `platform_api_vmcore.h` declaration
for `os_mutex_*`; its 4.4.0 failing build was reproduced and its focused 9/9
scenario rerun passed.

## Test roots

| Directory | Scope |
| --- | --- |
| [`platform_api/`](platform_api/README.md) | Representative allocation, aligned-allocation, time, thread, mutex, condition-variable, lifecycle, and unsupported named-semaphore platform APIs; its focused guide also describes the isolated mocked-error boundary. |
| `runtime/` | Representative WAMR initialization, module loading, instantiation, execution, failure recovery, and repeated interpreter/pool-mode workflows. |
| `usermode_faults/` | WAMR-specific memory-domain and MPU boundaries, expected user faults, recovery, and the distinction between a Wasm trap and an MPU fault. |
| `common/` | Shared Wasm byte fixtures and runtime workflow helpers used by the runtime and fault suites. It is not a standalone Twister root. |
| `test_build_and_run.py` | Host-side unit tests for `build_and_run.py`. |
| `test_coverage_report.py` | Host-side unit tests for `coverage_report.py`. |

Each runnable root has a `testcase.yaml`. Twister is the source of the test
verdict and records individual Ztest results in its JSON and XML reports.

Within `platform_api/`, `src/test_platform.c` owns allocation, executable
mapping, memory-protection, scheduling, and platform-sentinel contracts.
`src/test_unsupported.c` owns the contracts that named semaphores, rwlocks, and
blocking-operation APIs remain unsupported and report errors safely.

## Kernel and user contexts

`platform_api/testcase.yaml` defines four scenarios:

- `wamr.zephyr.platform_api.kernel` runs on `native_sim` and QEMU ARC;
- `wamr.zephyr.platform_api.kernel_stack_info` runs on `native_sim` with
  `CONFIG_THREAD_STACK_INFO=y`;
- `wamr.zephyr.platform_api.userspace` runs on QEMU ARC with
  `CONFIG_USERSPACE=y`, `CONFIG_WAMR_TEST_USER_MODE=y`, and
  `CONFIG_DYNAMIC_OBJECTS=n`.
- `wamr.zephyr.platform_api.mocked_errors` runs only on `native_sim` with
  `CONFIG_WAMR_ZEPHYR_TEST_MOCKS=y`; it compiles only the dedicated FFF fixture
  and leaves the ordinary scenarios' direct production calls unchanged. See
  [the focused platform-API guide](platform_api/README.md) for its error-path
  classification, seams, and limits.

Run only the isolated mock scenario from `product-mini/platforms/zephyr` with:

```bash
python3 build_and_run.py --sim native_sim \
  --scenario wamr.zephyr.platform_api.mocked_errors tests/platform_api
```

The two kernel coverage scenarios use the same platform-API kernel contracts;
the stack-information scenario differs only by `CONFIG_THREAD_STACK_INFO` so
the aggregate captures that configuration branch without changing the test
purpose.

Tests declared with `WAMR_CONTEXT_TEST()` or `WAMR_CONTEXT_TEST_F()` compile as
ordinary `ZTEST`/`ZTEST_F` cases in the kernel scenario and as
`ZTEST_USER`/`ZTEST_USER_F` cases in the userspace scenario. This lets one test
body verify the same WAMR API with a supervisor caller and a user caller.

Some provisioning and protection cases intentionally remain supervisor
controlled:

- `platform_thread_pool.test_thread_pool_prepare_validates_and_preserves_pool`
  validates
  thread/stack descriptors and provisions registered kernel objects;
- `platform_thread_pool.test_self_thread_rejects_preinit_and_unmapped_callers`
  creates a raw thread outside the WAMR pool and verifies that no native
  `k_tid_t` escapes through `os_self_thread()`;
- `platform_sync_pool.test_sync_pool_prepare_validates_and_preserves_pool`
  validates
  capacities, native-object storage, overlap, ownership, and initialization;
- `platform_sync_pool.test_sync_pool_concurrent_prepare_preserves_owner`
  checks concurrent calls after the pool is already prepared;
- `platform_sync.test_prepared_native_sync_objects_require_inherited_permissions`
  proves the fixture regrants the prepared native objects to the current
  `ZTEST_USER` thread so `K_INHERIT_PERMS` children work while an unrelated raw
  `K_USER` thread still faults;
- `platform_sync.test_condvar_wait_requires_a_grant` deliberately withholds a
  condition-variable grant and verifies the resulting user fault.

The preparation functions are embedding-time supervisor APIs. In particular,
`wamr_zephyr_sync_pool_prepare()` must be called by one serialized provisioner
for a suspended WAMR user root. Simultaneous first calls are unsupported. A
later sequential call is idempotent only for the identical descriptor and
owner. Under `CONFIG_WAMR_TEST_USER_MODE`, the shared fixture pre-binds the
sync pool before `wasm_runtime_full_init()`, and supervisor-side `before`
hooks regrant only the current `ZTEST_USER` thread. That keeps the native
objects private while restoring the validation and concurrent-preparation
contracts inside the userspace image.

### Internal and public synchronization

The Zephyr port uses two deliberately separate synchronization mechanisms:

- the statically stored internal thread-pool metadata lock is a `sys_mutex`
  under `CONFIG_USERSPACE`, so it can reside in WAMR-accessible application
  memory without per-thread kernel-object grants;
- user-visible `os_mutex_*` and `os_cond_*` values are opaque WAMR handles
  backed by application-provided, statically registered `k_mutex` and
  `k_condvar` pools.

The prepared native sync-pool objects are not made public. Userspace fixtures
grant them to the specific current thread and rely on `K_INHERIT_PERMS` for
descendants; unrelated raw `K_USER` threads intentionally fault when a grant
is missing.

Kernel mode also implements WAMR conditions directly with Zephyr
`k_condvar`. It does not allocate semaphore-backed waiter nodes. Named
`os_sem_*` APIs remain unsupported.

## Running tests

From `product-mini/platforms/zephyr`, the default wrapper builds or reuses the
local `wamr-zephyr` Docker image:

```sh
python3 build_and_run.py --sim native_sim tests/platform_api
python3 build_and_run.py --sim qemu_arc tests/platform_api
python3 build_and_run.py --sim native_sim tests/runtime
python3 build_and_run.py --sim qemu_arc tests/runtime
python3 build_and_run.py --sim qemu_arc tests/usermode_faults
```

Inside an already configured Zephyr workspace or the CI container, add
`--no-docker`:

```sh
python3 build_and_run.py --no-docker --sim qemu_arc tests/platform_api
```

Run the complete host-side unit-test discovery from the repository root. This
includes both `test_build_and_run.py` and `test_coverage_report.py`:

```sh
python3 -m unittest discover -s product-mini/platforms/zephyr/tests \
  -p 'test*.py' -v
```

Ordinary artifacts are written below:

```text
product-mini/platforms/zephyr/build/
├── logs/<test-root>-<sim>.log
└── twister-<test-root>-<sim>/
    ├── twister.json
    ├── twister.xml
    └── <platform>/<scenario>/handler.log
```

The wrapper returns Twister's exit status. Use `twister.json` for scenario and
individual-case results rather than inferring success from console output.

## Informational coverage

Coverage is measurement only; no percentage is a pass threshold. Run the
ordinary checks above separately. From `product-mini/platforms/zephyr`, the
per-scenario coverage commands are:

```sh
python3 build_and_run.py --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.kernel tests/platform_api
python3 build_and_run.py --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.kernel_stack_info tests/platform_api
python3 build_and_run.py --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.mocked_errors tests/platform_api
python3 build_and_run.py --coverage --sim qemu_arc \
  --scenario wamr.zephyr.platform_api.kernel tests/platform_api
python3 build_and_run.py --coverage --sim qemu_arc \
  --scenario wamr.zephyr.platform_api.userspace tests/platform_api
```

GitHub Actions runs these five commands only in the manually dispatched
`coverage_measurement` job. Build, test, gcov extraction, and report failures
fail that manual job even though no coverage percentage is enforced. CI
uploads the native aggregate and the two QEMU reports as three independent
artifacts; ordinary push and pull-request jobs do not collect coverage.

Use `--no-docker` in an already configured Zephyr workspace or CI container.
Then aggregate the three raw traces from the repository root:

```sh
python3 product-mini/platforms/zephyr/coverage_report.py \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage.json \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/coverage.json \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-mocked-errors-e2d36566-coverage/coverage.json
```

The scenario artifacts are:

```text
build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/
├── coverage/
├── coverage.json
└── twister.json
build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/
├── coverage/
├── coverage.json
└── twister.json
build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-mocked-errors-e2d36566-coverage/
├── coverage/
├── coverage.json
└── twister.json
build/twister-tests-platform_api-qemu_arc-wamr-zephyr-platform-api-kernel-af729dc6-coverage/
├── coverage/
├── coverage.json
└── twister.json
build/twister-tests-platform_api-qemu_arc-wamr-zephyr-platform-api-userspace-75456078-coverage/
├── coverage/
├── coverage.json
└── twister.json
build/coverage-zephyr-platform-aggregate/
├── index.html
├── coverage.xml
├── coverage.json
├── line-summary.txt
├── branch-summary.txt
└── inputs.txt
```

The `af729dc6`, `d286e15c`, and `e2d36566` suffixes are the first eight hex
characters of the scenario SHA-256. They keep same-slug scenarios distinct in this small
trusted scenario set, which makes them collision-resistant here without
claiming a collision-free naming scheme.

The native aggregate is union evidence from three real builds, not coverage
from one binary. The mocked-errors trace stays separate from the kernel and
stack-information configurations until aggregation. Keep the QEMU ARC kernel
and userspace reports separate from that native aggregate and from each other:
their architecture and privilege-specific compiled branches are different.
Hardware cache branches and impossible defensive branches are intentionally
not targeted.

Embedded coverage is compiler-filtered to `core/shared/platform/zephyr/` so
the target records the production code being measured instead of the whole
Zephyr, WAMR, and test image. Whole-image probes exhausted Zephyr 3.7's default
16 KiB `CONFIG_COVERAGE_GCOV_HEAP_SIZE`; the focused dumps complete without
increasing it. Gcov may additionally attribute inline code to
`core/shared/platform/include/platform_api_vmcore.h`; that is the only file
outside the filter directory in the current reports. In the Docker workflow,
the wrapper also supplies the SDK's ARC `gcov` executable. A `--no-docker`
caller must provide a configured SDK/gcov environment.

The userspace coverage command alone adds
`CONFIG_PRIVILEGED_STACK_SIZE=4096`. This is not the WAMR worker stack. It is
the per-user-thread stack used while Zephyr handles syscalls. Full gcov builds
compile those paths at `-O0`, add counter updates, and disable inlining; the
default 1 KiB privileged stack overflowed during the concurrent synchronization
case and produced a PC-zero MPU instruction-fetch fault. Kernel-mode threads do
not cross that userspace syscall boundary, so the QEMU ARC kernel coverage run
works with the default privileged-stack setting.

Verified on Zephyr 3.7 on 2026-09-09:

| QEMU ARC report | Tests passed | Skipped | Lines | Branches |
| --- | ---: | ---: | ---: | ---: |
| Kernel | 68 | 18 | 528/614 (86.0%) | 225/334 (67.4%) |
| Userspace | 85 | 3 | 772/932 (82.8%) | 379/588 (64.5%) |

The host-side discovery suite passed 35/35 tests with these wrapper changes.

Historical pre-feature/pilot evidence: Task 6 established the Zephyr 3.7.0
pinned baseline on 2026-08-27. The earlier Task 7 pilot reran affected 3.7
roots and both focused coverage scenarios on 2026-08-28 after landing the
then-current 4.4-compatible fixes. These values are not the current final
verification summarized above:

| Coverage input | Suites passed | Testcases passed | Testcases skipped | Failed | Error |
| --- | ---: | ---: | ---: | ---: | ---: |
| `wamr.zephyr.platform_api.kernel` | 1 | 63 | 14 | 0 | 0 |
| `wamr.zephyr.platform_api.kernel_stack_info` | 1 | 63 | 14 | 0 | 0 |

The focused aggregate contains only `core/shared/platform/zephyr/` production
paths:

| File | Lines | Exec | Line cover | Branches | Taken | Branch cover |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `platform_internal.h` | 7 | 7 | 100% | 0 | 0 | -- |
| `zephyr_platform.c` | 51 | 49 | 96% | 8 | 8 | 100% |
| `zephyr_thread.c` | 530 | 441 | 83% | 316 | 211 | 66% |
| `zephyr_time.c` | 10 | 10 | 100% | 2 | 1 | 50% |
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

The historical pre-feature/pilot 11-entry Zephyr 3.7 matrix table below
combines those earlier reruns with the three unaffected 2026-08-27 artifacts
preserved from the initial full run. It is retained for provenance; use the
current 324/51 final verification above for this phase. All 11
`twister.json` files contributing to this older table reported zero
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

Task 6 still provided the gcovr aggregate fix and the sync-pool permission fix
for `platform_sync.test_prepared_native_sync_objects_require_inherited_permissions`.
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
| `platform_internal.h` | 7 | 7 | 100% | 0 | 0 | -- |
| `zephyr_platform.c` | 51 | 49 | 96% | 8 | 8 | 100% |
| `zephyr_thread.c` | 532 | 443 | 83% | 318 | 213 | 67% |
| `zephyr_time.c` | 10 | 10 | 100% | 2 | 1 | 50% |
| Total | 600 | 509 | 84% | 328 | 222 | 67% |

Zephyr 3.7.0 remains the supported and pinned baseline. The Zephyr 4.4.0
results are compatibility evidence and do not change the repository pin. No
WASI, AOT, or broader I/O expansion was added in Task 7. libc-WASI, clock,
filesystem, and sockets remain deferred to an optional phase.
