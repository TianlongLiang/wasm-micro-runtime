# Zephyr Ztest PR Series Review Guide

This series is intended to merge in order. Each PR is green at its own head and
depends only on `main` plus earlier PRs; none requires a later PR to build or
run. The repository remains pinned to Zephyr 3.7.0.

## Series at a Glance

- PR 1 establishes the reusable Twister/Ztest pipeline and representative
  platform API and runtime suites.
- PR 2 introduces a WAMR-specific user-mode fault harness for memory domains,
  guest boundaries, expected faults, and shared runtime workflows.
- PR 3 implements the production Zephyr user-mode thread model with
  caller-provided thread and synchronization pools, opaque handles, lifecycle
  management, and a multithread sample.
- PR 4 expands representative platform coverage, aggregates coverage across
  configurations, corrects permission fixtures, and adds bounded Zephyr 4.4.x
  compatibility handling.

## PR 1: Establish the Twister/Ztest Pilot

Commit: `92e5e375 test: add Zephyr Ztest pilot coverage`

Purpose: replace demo-only validation with a small, repeatable Twister pipeline
for representative WAMR platform APIs and interpreter/pool runtime workflows.

Main changes:

- generalize `build_and_run.py` for samples and `tests/...` roots;
- add platform API suites for memory, synchronization, threads, and time;
- add kernel/user runtime lifecycle suites using small Wasm fixtures;
- run the new roots on `native_sim` and QEMU ARC in Zephyr CI.

Review focus: Twister scenario selection, kernel-versus-user test context, pool
heap assumptions, and the intentionally skipped cases marked with `FIXME`.

Supporting verification: 4 Python tests; platform API on `native_sim` and
QEMU ARC; runtime on `native_sim` and QEMU ARC. All selected configurations
passed.

## PR 2: Add WAMR-Specific User-Mode Fault Tests

Commit: `ee0fe954 test: add Zephyr user-mode fault coverage`

Depends on: PR 1's runner and runtime fixtures only.

Purpose: test faults caused by realistic WAMR user-mode boundaries without
duplicating Zephyr's general userspace/MPU test matrix.

Main changes:

- extract shared user-mode runtime startup and Wasm fixtures;
- move expected-fault support into a dedicated `usermode_faults` suite;
- cover WAMR domain access, guest boundary failures, and Wasm out-of-bounds
  containment;
- add optional native platform coverage measurement to CI.

Review focus: expected-fatal-test containment, memory-domain lifecycle, object
permissions, and cleanup after a user thread faults.

Supporting verification: 8 Python tests; focused coverage generation; both
runtime platforms; and the QEMU ARC fault suite. All selected configurations
passed.

## PR 3: Support WAMR-Created Threads in Zephyr User Mode

Commit: `57e22ece feat: add Zephyr user-mode thread pools`

Depends on: PRs 1 and 2 for test infrastructure; it has no dependency on PR 4.

Purpose: provide the production Zephyr platform implementation that lets WAMR
retain its POSIX-like `os_thread_create()` model in user mode without dynamic
`k_object_alloc()` support. The tests and sample verify and explain that
implementation; they are not the primary deliverable of this PR.

Main changes:

- add application-provided fixed thread, mutex, and condition-variable pools;
- use opaque WAMR thread handles and enforce slot lifecycle and generation
  checks;
- implement join/detach/recycling behavior and pool shutdown safeguards;
- add a user-mode pthread sample explaining object grants and the slot
  lifecycle;
- run synchronization APIs from both supervisor and WAMR-created user threads.

Review focus: `FREE -> RESERVED -> RUNNING -> EXITED -> JOINED -> FREE`, pool
permission preparation, handle validation, join/detach races, and separate pool
capacities.

Supporting verification: 8 Python tests; platform API on both simulators;
runtime and all five fault configurations on QEMU ARC; existing user mode; and
the new multithread sample. All selected configurations passed.

## PR 4: Expand Platform Coverage and Harden Compatibility

Commit subject: `test: expand Zephyr platform API coverage`

Depends on: PR 3. This is the final PR in the series.

Purpose: broaden representative Zephyr platform coverage and make the coverage
workflow useful for measurement without attempting exhaustive API coverage.

Main changes:

- add representative general platform and unsupported-API contract tests;
- test configuration mapping and stack-info variants;
- select individual Twister coverage scenarios and aggregate their traces;
- use collision-resistant scenario artifact names and atomic coverage output;
- correct synchronization-pool permission fixtures;
- retain Zephyr 3.7 compatibility while probing and accommodating Zephyr 4.4.

Problems resolved within this PR:

- readable scenario slugs could collide: append a stable short SHA-256 suffix;
- one artificial coverage build could not represent all configurations:
  aggregate traces from separate Twister scenarios;
- coverage publication could replace inputs or partial output: validate paths
  and publish the aggregate atomically;
- Zephyr 4.4.x returns from timed condition waits without reacquiring the
  mutex: relock only for `-EAGAIN` on `[4.4.0, 4.5.0)`. Upstream commit
  `5c6c6837cc4026a70e670c265fc4d1e3b1f16379` fixes later releases;
- Zephyr 4.4 sample builds need updated heap/file-descriptor settings and a
  public `os_self_thread()` declaration.

Review focus: scenario selection and naming, coverage path validation, the
bounded 4.4.x workaround, and confirmation that the official Zephyr pin is
unchanged.

Supporting verification: 29 Python tests; all 11 ordinary Zephyr 3.7 CI matrix
entries; both focused coverage scenarios and aggregation; plus targeted Zephyr
4.4.0 and 4.4.1 condition-variable runs. Zero configurations failed or errored.

## Suggested Review Order

Review and merge one PR at a time in the order above. For each PR, compare it
with its immediate parent rather than with `main`; this keeps later pool and
coverage changes out of earlier reviews. The preserved branches
`zephyr-ztest-pilot-integration` and `zephyr-ztest-pilot-rebased` retain the
fine-grained implementation history when more context is needed.
