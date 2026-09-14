# WAMR Zephyr platform-API tests

## Final 2026-09-13 evidence

Fresh Zephyr 3.7 coverage on code revision `82ec4ca4` produces three independent
reports. Percentages below are computed from raw JSON counts and rounded to
two decimals; no percentage is a pass condition.

| Report | Baseline lines | Final lines | Baseline branches | Final branches |
| --- | ---: | ---: | ---: | ---: |
| Native aggregate | 531/608 (87.34%) | 549/613 (89.56%) | 228/332 (68.67%) | 235/328 (71.65%) |
| QEMU ARC kernel | 528/614 (85.99%) | 546/619 (88.21%) | 225/334 (67.37%) | 232/330 (70.30%) |
| QEMU ARC userspace | 772/932 (82.83%) | 784/907 (86.44%) | 379/588 (64.46%) | 388/578 (67.13%) |

Baselines are the retained pre-implementation measurements, not interim Task 2
or Task 5 results. Final branch numerators increased by 7/7/9 and denominators
changed by -4/-4/-10. Raw assertion-failure edges remain included. The legacy
rwlock implementation was restored for ordinary WASI compatibility, so its
uncovered write-lock branches remain; full rwlock correctness is deferred.

| Coverage input | JSON passed/skipped | JSON `execution_time` | Twister wall time | Raw lines | Raw branches |
| --- | ---: | ---: | ---: | ---: | ---: |
| Native kernel | 70/20 | 0.10 s | 7.20 s | 545/618 | 232/330 |
| Native stack info | 70/20 | 0.10 s | 6.01 s | 546/619 | 232/330 |
| Native mocked errors | 3/0 | 0.01 s | 5.77 s | 227/615 | 65/330 |
| QEMU ARC kernel | 71/19 | 2.96 s | 10.36 s | 546/619 | 232/330 |
| QEMU ARC userspace | 89/3 | 2.27 s | 14.48 s | 784/907 | 388/578 |

All five inputs passed with no failed or errored configurations. The three
native inputs alone are aggregated into
`build/coverage-zephyr-platform-aggregate/coverage.json`; `inputs.txt` names
them in kernel, stack-info, mocked-errors order. The unchanged aggregation
script was run inside the pinned `wamr-zephyr` container because host gcovr is
unavailable. Its existing platform-file filter removes inline vmcore
attribution from the native union; individual native traces and QEMU reports
remain raw. No new exclusion or threshold was introduced.

Paths below are relative to `product-mini/platforms/zephyr/`; each input tree
also contains `twister.json`, `coverage/index.html`, and `coverage/coverage.xml`:

- Native kernel: `build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage.json`.
- Native stack info: `build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-stack-info-d286e15c-coverage/coverage.json`.
- Native mocks: `build/twister-tests-platform_api-native_sim-wamr-zephyr-platform-api-mocked-errors-e2d36566-coverage/coverage.json`.
- QEMU kernel: `build/twister-tests-platform_api-qemu_arc-wamr-zephyr-platform-api-kernel-af729dc6-coverage/coverage.json`.
- QEMU userspace: `build/twister-tests-platform_api-qemu_arc-wamr-zephyr-platform-api-userspace-75456078-coverage/coverage.json`.

The native aggregate also contains `index.html`, `coverage.xml`, and text line
and branch summaries. Never merge QEMU with native or QEMU kernel with
userspace. The measurement commands below remain unchanged.

The remaining 93/98/190 untaken edges are informational. They include named
invariant failures, equivalent validation permutations, kernel/user-specific
paths, one-time preparation misuse and permission defenses, test-observation
guard failures, native/configuration-owned failures, and the deferred partial
rwlock subsystem. Existing public runtime recovery and the three narrow mocks
already cover the applicable rollback contracts; the mocked join trace still
records one failure and 13 successful joins, with claim-release lines executed
once. No new mock or coverage-only case was added to reach the former planning
goals of 72%/72%/70%. The new ordinary tests establish descriptor rejection
and recovery, invalid/stale detach safety, active-worker teardown deferral,
and deterministic out-of-order detached reaping; assertions name impossible
states without weakening public validation or the 4.4.x relock workaround.

The [complete matrix](../README.md#2026-09-13-final-meaningful-branch-coverage-evidence)
records all eleven ordinary 3.7 entries and both 4.4 probes, with exact case
counts, runtimes and limits. All ordinary entries remain below two minutes.

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

## One-time thread-pool validation

The supervisor-only `platform_thread_pool` descriptor test rejects null pool,
owner, thread storage, and stack storage, zero or oversized thread counts,
zero stack size, and a stride smaller than the stack size before valid pool
preparation. Successful preparation, identical re-preparation, rejection of a
replacement pool, and runtime initialization then prove the binding remains
usable. Before teardown, the same test creates the full configured
`BH_ZEPHYR_MPU_STACK_COUNT` capacity of real supervisor workers, retains all
opaque handles, and joins every worker. Cleanup precedes the recovery verdicts,
including partial creation. Descriptor mechanics remain plain `ZTEST` cases;
supervisor workers use the kernel-stack path, so this is not a userspace-grant
test.

`test_thread_pool_prepare_rejects_user_context_and_recovers` uses
`WAMR_CONTEXT_TEST`: on userspace it rejects preparation from the user caller,
then creates and joins a real worker using the supervisor-prepared fixture pool
to prove recovery. It skips in kernel-only scenarios. These are ordinary
validation tests, with no mocks or production instrumentation.

## Detach rejection and teardown recovery

`test_invalid_and_stale_detach_preserve_lifecycle` rejects null and already
joined opaque handles, then creates and joins a replacement worker to prove
that detach rejection preserves the thread lifecycle.

`test_active_worker_defers_thread_system_teardown` holds a worker at a
semaphore barrier while requesting thread-system teardown. The caller's
identity must remain intact. After releasing and joining the worker, it
destroys the runtime, verifies the caller mapping is gone, and completes a
second public runtime initialization/destruction cycle using the global heap
pool. It never destroys the runtime while the worker is active. Fixture
teardown avoids a duplicate destroy and checks that clean teardown removed
the caller mapping for every thread test.

Both cases use `WAMR_CONTEXT_TEST` in ordinary kernel and userspace scenarios,
with bounded semaphore guards, real lifecycle calls, and no mocks or new
production instrumentation.

## Out-of-order detached reaping

`test_reaper_skips_running_head_and_reclaims_later_exit` fills the four-slot
pool with independently semaphore-blocked workers A, C, D, and B before
allowing detached A to exit. A `CONFIG_ZTEST`-only completion observation
pauses detached B after list insertion and after releasing the pool lock.
The list is then `[B still running, A exited]`, while joinable C and D retain
their slots. A fifth create must scan past B and reclaim A to succeed.
After the replacement is joined, B is released and confirmed terminated;
another create/join triggers B's final reap before C and D are released and
joined. Detached handles are disowned immediately after successful detach.

The ordinary kernel and userspace context tests use bounded semaphore guards,
assert every worker and completion-hook release, and add no ordering sleeps
or mocks. Creating all four workers before A exits is essential because each
create itself invokes the reaper. A deliberate head-only reaper mutation must
make the fifth create fail; restoring the full scan must make both contexts
pass. The observation hook and its local state are absent from ordinary builds.

## Legacy rwlocks and libc-WASI compatibility

Zephyr's legacy partial rwlock behavior is retained solely for current
libc-WASI compatibility. Although this platform-API suite disables libc-WASI,
the ordinary `simple-file` sample requires successful rwlock initialization
for its WASI fd tables and uses the lock operations through libc-WASI wrappers.
Uniform rejection prevents module instantiation. Read locking and destruction
remain unimplemented; full rwlock correctness is deferred outside this phase.

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

The thread/synchronization invariant refactor also asserts bounded sync-pool
range multiplication (all three callers validate compile-time capacities and
pass `sizeof`), exactly-once locked generation insertion/removal, and continuity
of already-claimed synchronization slots. Pool binding is one-time; destroy
refuses active operations and condition waiters. Initial unprepared-pool,
null/stale-handle, ownership, and exhaustion errors remain recoverable, as do
pointer-addition overflow and overlapping pool ranges.

For valid objects, Zephyr 3.7/4.4 specify success for forever mutex locks,
ownership-validated unlocks, condition initialization and signal; broadcast
returns a nonnegative waiter count. Condition waits return success or timed
`-EAGAIN`. These native calls execute and their results are asserted, without
changing kernel versus userspace timeout return conventions or the bounded
4.4.x mutex-relock workaround. Exited-detach cleanup asserts successful forever
join after exclusive ownership of an EXITED generation. Normal public join
failure still releases its claim for retry, including the mocked-error case.
Dynamic kernel-stack state handling and the post-unlock detached-completion
test observation remain unchanged. Assertion-failure edges are not test goals.

The userspace scenario reserves 512 extra bytes for Zephyr's generated kernel
object read-only metadata. With the same 206 objects, a source-address change
made gperf's maximum hash grow from 252 to 277 between prebuilt and final links,
widening its association table from bytes to shorts. Final metadata needed
656 bytes but the default reservation was 416; the scenario-only setting
increased it to 912 and restored the unchanged behavioral tests. This is test
harness capacity for gperf's documented layout uncertainty, not a production
configuration change or a coverage exclusion.

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
`build/coverage-zephyr-platform-aggregate/`.

QEMU ARC can also produce separate architecture-specific kernel and userspace
reports:

```bash
python3 build_and_run.py --coverage --sim qemu_arc \
  --scenario wamr.zephyr.platform_api.kernel tests/platform_api
python3 build_and_run.py --coverage --sim qemu_arc \
  --scenario wamr.zephyr.platform_api.userspace tests/platform_api
```

Do not merge these with the native aggregate or with each other. The embedded
build filters compiler coverage to `core/shared/platform/zephyr/`, avoiding a
whole-image dump that exceeds Zephyr 3.7's default gcov heap. The Docker wrapper
selects the ARC SDK `gcov` tool. Inline code attributed to
`core/shared/platform/include/platform_api_vmcore.h` may also appear in the
report. For userspace coverage only, the wrapper raises
`CONFIG_PRIVILEGED_STACK_SIZE` from 1024 to 4096: gcov-expanded Zephyr syscall
frames overflowed the smaller privileged stack, whereas kernel-mode threads do
not use a userspace syscall-elevation stack. Ordinary non-coverage tests keep
their original configuration.

The GitHub Actions `coverage_measurement` job runs these QEMU commands only
when manually dispatched and uploads the kernel and userspace reports as
separate artifacts. It enforces no coverage percentage, but a build, test,
extraction, or reporting failure fails the requested measurement.

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
