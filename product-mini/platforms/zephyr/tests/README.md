# WAMR Zephyr tests

This directory contains focused tests for the WAMR Zephyr port. The suites are
demonstrations of WAMR integration behavior, not replacements for Zephyr's own
kernel, verifier, or MPU test matrices.

All current runtime scenarios use the interpreter and
`WAMR_BUILD_GLOBAL_HEAP_POOL`. AOT, alternate allocators, filesystem and socket
APIs, stress testing, and physical-board coverage are outside this test set.

## Test roots

| Directory | Scope |
| --- | --- |
| `platform_api/` | Representative allocation, aligned-allocation, time, thread, mutex, condition-variable, lifecycle, and unsupported named-semaphore platform APIs. |
| `runtime/` | Representative WAMR initialization, module loading, instantiation, execution, failure recovery, and repeated interpreter/pool-mode workflows. |
| `usermode_faults/` | WAMR-specific memory-domain and MPU boundaries, expected user faults, recovery, and the distinction between a Wasm trap and an MPU fault. |
| `common/` | Shared Wasm byte fixtures and runtime workflow helpers used by the runtime and fault suites. It is not a standalone Twister root. |
| `test_build_and_run.py` | Host-side unit tests for `build_and_run.py`. |

Each runnable root has a `testcase.yaml`. Twister is the source of the test
verdict and records individual Ztest results in its JSON and XML reports.

## Kernel and user contexts

`platform_api/testcase.yaml` defines two scenarios:

- `wamr.zephyr.platform_api.kernel` runs on `native_sim` and QEMU ARC;
- `wamr.zephyr.platform_api.userspace` runs on QEMU ARC with
  `CONFIG_USERSPACE=y`, `CONFIG_WAMR_TEST_USER_MODE=y`, and
  `CONFIG_DYNAMIC_OBJECTS=n`.

Tests declared with `WAMR_CONTEXT_TEST()` or `WAMR_CONTEXT_TEST_F()` compile as
ordinary `ZTEST`/`ZTEST_F` cases in the kernel scenario and as
`ZTEST_USER`/`ZTEST_USER_F` cases in the userspace scenario. This lets one test
body verify the same WAMR API with a supervisor caller and a user caller.

Some provisioning and protection cases intentionally remain supervisor
controlled:

- `platform_thread_pool.test_prepare_validates_and_preserves_pool` validates
  thread/stack descriptors and provisions registered kernel objects;
- `platform_thread_pool.test_self_thread_rejects_preinit_and_unmapped_callers`
  creates a raw thread outside the WAMR pool and verifies that no native
  `k_tid_t` escapes through `os_self_thread()`;
- `platform_sync_pool.test_prepare_validates_and_preserves_pool` validates
  capacities, native-object storage, overlap, ownership, and initialization;
- `platform_sync_pool.test_concurrent_prepare_is_idempotent_and_rejects_replacement`
  checks concurrent calls after the pool is already prepared;
- `platform_sync.test_prepared_native_sync_objects_are_user_accessible` grants
  native objects and starts a raw suspended `K_USER` probe;
- `platform_sync.test_condvar_wait_requires_a_grant` deliberately withholds a
  condition-variable grant and verifies the resulting user fault.

The preparation functions are embedding-time supervisor APIs. In particular,
`wamr_zephyr_sync_pool_prepare()` must be called by one serialized provisioner
for a suspended WAMR user root. Simultaneous first calls are unsupported. A
later sequential call is idempotent only for the identical descriptor and
owner.

### Internal and public synchronization

The Zephyr port uses two deliberately separate synchronization mechanisms:

- the statically stored internal thread-pool metadata lock is a `sys_mutex`
  under `CONFIG_USERSPACE`, so it can reside in WAMR-accessible application
  memory without per-thread kernel-object grants;
- user-visible `os_mutex_*` and `os_cond_*` values are opaque WAMR handles
  backed by application-provided, statically registered `k_mutex` and
  `k_condvar` pools.

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

Run the host-side wrapper tests from the repository root:

```sh
python3 -m unittest product-mini/platforms/zephyr/tests/test_build_and_run.py
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

Coverage is measurement only; no percentage is a pass threshold. The supported
coverage lane is the kernel scenario on `native_sim`:

```sh
python3 build_and_run.py --coverage --sim native_sim tests/platform_api
```

Use `--no-docker` for the same command inside a configured Zephyr environment.
The reports are written to:

```text
build/twister-tests-platform_api-native_sim-coverage/
├── coverage/index.html
├── coverage/coverage.xml
├── coverage.json
└── twister.json
```

The corresponding streamed log is
`build/logs/tests-platform_api-native_sim-coverage.log`.

On 2026-08-22, the focused `core/shared/platform/zephyr/` result was:

| Metric | Covered | Total | Coverage |
| --- | ---: | ---: | ---: |
| Lines | 438 | 589 | 74% |
| Branches | 209 | 324 | 64% |

Those percentages measure the native kernel scenario. They are not evidence of
userspace object permissions or MPU isolation. QEMU ARC provides that
behavioral evidence, but the pinned Zephyr 3.7/Twister environment does not
successfully post-process QEMU ARC coverage.

Zephyr 3.7 Twister cannot filter the generated report to only WAMR's Zephyr
platform directory. To derive the focused line view from the raw trace, run
from the repository root:

```sh
docker run --rm \
  -v "$PWD:/root/zephyrproject/modules/wasm-micro-runtime" \
  -w /root/zephyrproject/modules/wasm-micro-runtime wamr-zephyr \
  gcovr -r /root/zephyrproject/modules/wasm-micro-runtime \
  --filter 'core/shared/platform/zephyr/' \
  --add-tracefile product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-coverage/coverage.json \
  --txt-metric line --txt -
```

Use `--txt-metric branch` for the branch summary. The broader Zephyr platform
README contains CI-container setup and additional coverage details.
