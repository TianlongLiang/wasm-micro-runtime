# WAMR Zephyr tests

This directory contains focused tests for the WAMR Zephyr port. The suites are
demonstrations of WAMR integration behavior, not replacements for Zephyr's own
kernel, verifier, or MPU test matrices.

All current runtime scenarios use the interpreter and
`WAMR_BUILD_GLOBAL_HEAP_POOL`. AOT, alternate allocators, filesystem and socket
APIs, randomized/timing-dependent/load stress testing, and physical-board
coverage are outside this test set.

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

Measured final ordinary-root wall times are 13.64 s (`platform_api` native),
27.77 s (`platform_api` QEMU ARC), 7.08 s (`runtime` native), and 25.72 s
(`runtime` QEMU ARC). They are far below the two-to-three-minute placement
ceiling, so every deterministic case remains in its existing ordinary required
scenario; no dedicated scenario, Kconfig switch, or wrapper lane is needed.

The focused native coverage measurement runs the two commands in the
Informational coverage section sequentially, then aggregates their
`coverage.json` inputs. The final aggregate is 507/598 lines (84%) and
223/326 branches (68%), compared with the 507/598 lines (84%) and 220/326
branches (67%) baseline: three additional `zephyr_thread.c` branch outcomes
and no new executable lines. The kernel-only aggregate cannot enter the
QEMU-only userspace registered-thread-slot path; its direct saturation and
recovery test is the corresponding behavioral evidence. Allocator-failure
exits remain configuration/hardware-only, so the coverage gate correctly
added no memory test.

Fresh 2026-08-30 verification on the pinned Zephyr 3.7.0 image ran all eleven
ordinary entries plus host discovery. Host discovery passed 29/29 tests. The
eleven `twister.json` files contain 324 passed and 51 skipped testcase records,
with zero failed, error, or null statuses. Both focused coverage scenarios
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
| `platform_api/` | Representative allocation, aligned-allocation, time, thread, mutex, condition-variable, lifecycle, and unsupported named-semaphore platform APIs. |
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

`platform_api/testcase.yaml` defines three scenarios:

- `wamr.zephyr.platform_api.kernel` runs on `native_sim` and QEMU ARC;
- `wamr.zephyr.platform_api.kernel_stack_info` runs on `native_sim` with
  `CONFIG_THREAD_STACK_INFO=y`;
- `wamr.zephyr.platform_api.userspace` runs on QEMU ARC with
  `CONFIG_USERSPACE=y`, `CONFIG_WAMR_TEST_USER_MODE=y`, and
  `CONFIG_DYNAMIC_OBJECTS=n`.

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
includes both `test_build_and_run.py` and `test_coverage_report.py` (29 tests
in the current final evidence):

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
```

Use `--no-docker` in an already configured Zephyr workspace or CI container.
Then aggregate the two raw traces from the repository root:

```sh
python3 product-mini/platforms/zephyr/coverage_report.py \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage.json \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/coverage.json
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
build/coverage-zephyr-platform-aggregate/
├── index.html
├── coverage.xml
├── coverage.json
├── line-summary.txt
├── branch-summary.txt
└── inputs.txt
```

The `af729dc6` and `d286e15c` suffixes are the first eight hex characters of
the scenario SHA-256. They keep same-slug scenarios distinct in this small
trusted scenario set, which makes them collision-resistant here without
claiming a collision-free naming scheme.

The aggregate is union evidence from two real builds, not coverage from one
binary. QEMU ARC remains behavioral userspace evidence rather than coverage.
Hardware cache branches and impossible defensive branches are intentionally not
targeted.

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
