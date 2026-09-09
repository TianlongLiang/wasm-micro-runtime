# WAMR Zephyr platform-API tests

## Scope and scenarios

`testcase.yaml` declares four focused platform-API scenarios. Twister owns the
verdict for each scenario:

| Scenario | Platforms and purpose |
| --- | --- |
| `wamr.zephyr.platform_api.kernel` | `native_sim` and QEMU ARC kernel-context platform API behavior. |
| `wamr.zephyr.platform_api.kernel_stack_info` | `native_sim` kernel behavior with `CONFIG_THREAD_STACK_INFO=y`. |
| `wamr.zephyr.platform_api.userspace` | QEMU ARC user-context platform API behavior. |
| `wamr.zephyr.platform_api.mocked_errors` | `native_sim` only; deterministic FFF injection of three difficult thread error paths. |

The mocked scenario enables the test-only
`CONFIG_WAMR_ZEPHYR_TEST_MOCKS` option and compiles only
`src/test_mocked_errors.c`. It is deliberately separate from the ordinary
kernel, stack-information, and userspace builds, whose normal source lists and
runtime behavior remain unchanged.

## Error-path classification

This suite tests WAMR cleanup and recovery where an otherwise-valid Zephyr or
WAMR operation can fail but ordinary Twister runs cannot reproduce that failure
reliably. It does not use a mock merely to make every branch appear covered.

| Class | Required treatment |
| --- | --- |
| Naturally reachable | Exercise it with an ordinary Ztest using public WAMR behavior. |
| Real but difficult or non-deterministic | Inject one controlled FFF failure and verify WAMR cleanup plus recovery. |
| Contractually impossible | Keep the native call and assert its invariant; do not mock it. |
| Out-of-contract misuse | Retain defensive rejection and use deterministic malformed-input tests only when they clarify the WAMR contract. |
| Uncertain across supported versions | Retain defensive behavior and document the uncertainty. |

Real difficult failures are documented results for valid objects, such as an
allocator returning `NULL` or `k_thread_join()` returning `-EDEADLK`; these are
the mock candidates. In contrast, a successful `k_thread_create()` returning a
different thread ID, `k_mutex_init()` or `k_condvar_init()` failing for a
validated object, or the immediately following `k_mutex_lock(..., K_FOREVER)`
failing are contract events/invariants, not recoverable runtime errors. The
production code therefore keeps those native calls and uses always-on
`bh_assert()` checks rather than synthesizing impossible failures. Pool-owner
mutation, termination during preparation, and simultaneous first preparation
remain misuse defenses, not mock targets.

## Mocked thread errors

The `platform_mocked_errors` suite injects exactly one failure per test, checks
the public WAMR result and cleanup, then creates and joins all available static
stack workers to prove capacity recovery:

- `test_native_thread_object_allocation_failure_recovers` fails the first
  WAMR-owned allocation for `struct k_thread`; no handle, later allocation, or
  native create is published.
- `test_thread_metadata_allocation_failure_frees_object_and_recovers` permits
  the thread object then fails metadata allocation; the earlier object is freed
  once and no native create occurs.
- `test_join_failure_releases_claim_for_retry` injects one documented
  `-EDEADLK` from `k_thread_join()`, verifies no return value or live metadata
  free, and retries the join successfully after WAMR releases its join claim.
  Both joins target the worker's native thread ID with `K_FOREVER`; successful
  retry frees each original allocation exactly once.

FFF normally delegates to the real operations. Its assertions establish WAMR
rollback, accounting, and lifecycle recovery; they do not prove that Zephyr's
allocator, scheduler, or `k_thread_join()` implementation is itself correct.
Teardown releases ownership only after a successful public join. If a regression
prevents cleanup, the after-hook assertion stops the suite while retaining the
owned handle and runtime; the next before hook also rejects runtime reset.

## Production seam boundary

The only mock-enabled production seams are compile-time macros in
`core/shared/platform/zephyr/zephyr_thread.c`:

```c
void *wamr_zephyr_thread_test_malloc(unsigned int size);
void wamr_zephyr_thread_test_free(void *ptr);
int wamr_zephyr_thread_test_join(k_tid_t thread, k_timeout_t timeout);
```

With `CONFIG_WAMR_ZEPHYR_TEST_MOCKS=y`, those narrowly named hooks let the
dedicated fixture inject the two allocation failures and the one join failure.
With the option disabled, the macros expand directly to `BH_MALLOC`, `BH_FREE`,
and `k_thread_join`; the fixture, fake state, and runtime function-pointer
dispatch are absent from normal builds. The test delegates call `os_malloc()`,
`os_free()`, and `k_thread_join()` by default.

The related invariant cleanup is also intentional and confined to the
contractually impossible cases: sync-pool management mutex initialization and
its `K_FOREVER` lock, each pool mutex initialization, and each condition
variable initialization execute before `bh_assert(result == 0)`. Native thread
creation asserts that the returned ID is the supplied thread object. The
unreachable sync-pool failed state and cleanup, plus native-create `fail3`
cleanup and `stack_to_free` bookkeeping, were removed. Real allocation,
capacity, join-claim rollback, owner validation, grant/revalidation/rollback,
and preparation-CAS failure handling remain intact.

## Running the suites

From `product-mini/platforms/zephyr`, run only the isolated mock scenario with:

```bash
python3 build_and_run.py --sim native_sim \
  --scenario wamr.zephyr.platform_api.mocked_errors tests/platform_api
```

Use `--no-docker` only in an already configured Zephyr workspace or CI
container. Explicitly selecting the ordinary kernel or userspace scenarios
keeps mocks disabled and their binaries on direct calls. An unfiltered
`native_sim` `tests/platform_api` invocation instead lets Twister discover all
native scenarios, including the separate `mocked_errors` scenario.

## Zephyr 4.4 focused probes

These compatibility probes run only ordinary `kernel` and isolated
`mocked_errors` on `native_sim`; they do not upgrade the official Zephyr 3.7.0
pin or replace the full pinned matrix. Prepare separate west workspaces with
the release's modules and Python dependencies installed. The recorded probes
used Python 3.12 and Zephyr SDK 1.0.1 at these exact Zephyr revisions:

| Release | Zephyr commit |
| --- | --- |
| 4.4.0 | `684c9e8f32e4373a21098559f748f06915f950c9` |
| 4.4.1 | `1f6485eca25431b5ff27ce9a754218c9e559bbbb` |

Run the following from each west workspace root with its Python environment
active. Set `WAMR_ROOT` to the checkout being tested and the SDK path to the
installed SDK. Use a fresh output directory for each run; preserve any earlier
reports before reusing its name.

```bash
export WAMR_ROOT=/absolute/path/to/wasm-micro-runtime
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk-1.0.1
git -C zephyr rev-parse HEAD
west twister \
  -T "$WAMR_ROOT/product-mini/platforms/zephyr/tests/platform_api" \
  -p native_sim -x "EXTRA_ZEPHYR_MODULES=$WAMR_ROOT" \
  -s wamr.zephyr.platform_api.kernel \
  -s wamr.zephyr.platform_api.mocked_errors \
  --outdir outputs/wamr-mocked-errors --inline-logs \
  --disable-warnings-as-errors --jobs 1
jq -e 'all(.testsuites[]; .status == "passed") and
  all(.testsuites[].testcases[]; .status == "passed" or .status == "skipped")' \
  outputs/wamr-mocked-errors/twister.json
```

Expected per release: 2/2 configurations pass, with 67 passed/19 skipped kernel
cases and 3 passed/0 skipped mocked cases. The two workspaces retain their own
`outputs/wamr-mocked-errors/twister.json` and build logs. Twister needs local
process/socket access; a Python multiprocessing listener permission failure is
an environment error before test execution.

The existing `bh_common.c` implicit `strtok_r` declaration and int-to-pointer
warnings occur on both 3.7 and 4.4. They indicate a declaration/ABI hazard if
that function executes, but all three retained 3.7 coverage traces show zero
executions of `bh_strtok_r` and its affected line. This series neither introduces
nor worsens the hazard, and it is not a blocker for these cleanup/recovery
tests. Track a separate declaration/configuration fix with an executed tokenizer
regression test. Unused-function/variable warnings are in unchanged ordinary
tests; the unused CMake `TC_NAME` warning is from a Twister-supplied variable.
Twister's zero warning-configuration summary does not mean compiler output is
warning-free.

## Aggregating informational coverage

Coverage is raw measurement, not a threshold or a reason to exclude source
from reporting. Run each native scenario independently at one source revision:

```bash
python3 build_and_run.py --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.kernel tests/platform_api
python3 build_and_run.py --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.kernel_stack_info tests/platform_api
python3 build_and_run.py --coverage --sim native_sim \
  --scenario wamr.zephyr.platform_api.mocked_errors tests/platform_api
```

From the repository root, aggregate the three raw traces without combining the
mocked build into either ordinary configuration:

```bash
python3 product-mini/platforms/zephyr/coverage_report.py \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage.json \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/coverage.json \
  product-mini/platforms/zephyr/build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-mocked-errors-e2d36566-coverage/coverage.json
```

`coverage_report.py` records their union in
`build/coverage-zephyr-platform-aggregate/`. QEMU ARC remains behavioral
userspace evidence, not a coverage input for this aggregate.

## Limits and future selection

This is not a general mandate to mock every Zephyr API. Consider another mock
only when all of these conditions hold:

1. Zephyr documents the failure as possible.
2. The branch owns WAMR-specific cleanup, resource state, or recovery.
3. Ordinary QEMU or native-simulation tests cannot trigger it deterministically.
4. Portable runtime tests on other platforms do not already provide equivalent evidence.

Zephyr-specific platform memory cleanup and partial platform-initialization
rollback are possible future candidates. File and socket ownership failures
are low priority and would not replace integration tests. Time calls,
deterministically unsupported APIs, MPU or kernel-object permission faults,
contractually impossible events, and portable-core paths already covered
elsewhere are non-candidates for this mock boundary.
