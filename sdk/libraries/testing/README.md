# PipeLang.Testing 0.1.0

A reusable authored PipeLang source library for unit and table tests. The original
synchronous API remains in `PipeLang.Testing`; asynchronous cases explicitly import
the optional `PipeLang.Testing.Async` module.
The implementation is ordinary `.pipe` source; the native consumer only invokes
its source entrypoint and checks or reports the returned values.

`Assert` provides receiver-free static `True`, `False`, `Equal<T>` and `NotEqual<T>`
methods returning `Result<void,TestFailure>`. Failures retain the existing
`assert.true`, `assert.false`, `assert.equal` and `assert.not_equal` codes and the
authored message. Existing instance-based `Assertions` remains compatible.
An `ITestCase` supplies an explicit stable `Id`, a readable `Name`, and typed
`Setup`, `Body` and `Cleanup` hooks. Parameterized tests are normal case instances
constructed from typed records; each instance supplies its own stable ID.

`TestRunner.RunAll` validates nonempty unique IDs and names before running any
hook, retains registration order, and admits at most 128 explicitly registered
cases per invocation. IDs and names must remain stable for the lifetime of a case.
This is a deliberate suite bound; split larger suites into explicit invocations.

Cancellation before setup returns a cancelled result without entering any hook.
After a setup attempt, cleanup is attempted exactly once through source
`try/finally`, including body failure or cancellation. Setup failure skips the
body. Cancellation requested after successful setup or body is recorded before
continuing. Hooks must honor their token during their own work. Cleanup receives
no cancelled token so partial setup can be released.

`TestResult` retains the primary setup/body outcome and failure separately from
cleanup. A cleanup failure or cancellation makes the final outcome Failed; it
does not rewrite a successful primary result or overwrite a primary failure.
`bodyRan` and `cleanupRan` distinguish skipped phases from completed ones.
`primaryPhase` identifies the setup/body phase last attempted, or NotRun when
cancelled before setup.

Unexpected ordinary source errors become `test.unexpected_error` failures.
Terminal language work/depth exhaustion is not recoverable as a TestResult:
cleanup is attempted within the existing language limits, and the terminal fault
reaches the containing model/task boundary. Finite limits cannot guarantee every
cleanup helper succeeds at an exhausted call depth.

The SDK installs this directory under `share/pipelang-model/libraries/testing`.
Consume its `library.json` through the generic, checksum-pinned
`SOURCE_LIBRARIES` reference contract. Sources remain external to the application;
do not copy them into each test project. `tests/testing/CMakeLists.txt` demonstrates
an installed consumer using `PipeLangModel_SOURCE_LIBRARY_DIRECTORY` and the
library descriptor hash. The descriptor inventories every source file by exact
SHA256; edit the inventory whenever library source changes.

## Scoped cancellation probes

`IProbeTestCase : ITestCase` adds `BodyWithProbe(normalToken, probeToken)`.
`TestRunner.RunProbe` and `RunAllProbes` retain the ordinary setup, cleanup,
exception conversion and separate primary/cleanup result contract. The latter
retains the same 128-case registration bound, stable unique IDs and declaration
order. Existing `ITestCase`, `Run` and `RunAll` behavior remains unchanged.

Only `normalToken` controls the runner's pre/post lifecycle cancellation checks.
`probeToken` is a body input: it may already be cancelled, or a test may request
its cancellation through its actual host effect, without preventing the test
from asserting that behavior. Setup receives the normal token and cleanup remains
independent. A body-authored failure/cancellation still takes precedence over a
later normal-token cancellation; cleanup failures retain their separate channel.

Tokens remain method parameters throughout the shared runner core and source
case. They must not be stored in fields, captured in case state, put into an owned
container or manufactured as boolean substitutes. An ordinary `Run` still calls
`Body(normalToken)` even when the object also implements `IProbeTestCase`; only
the explicit probe entrypoints select `BodyWithProbe`.

The additive `testing_probe_contracts` consumer uses actual native cancellation
sources with Source-owned sequencing and assertions. It does not add an async
probe API, native test scheduler or new evaluation/cancellation default.

## Optional asynchronous cases

Import `PipeLang.Testing.Async` explicitly in addition to the base module. Async
sources are separate module fragments, so a synchronous-only import does not
compile the async runner or require concrete async case implementations.
`IAsyncTestCase` supplies stable pure `Id`/`Name` methods and source async
`Setup`, `Body` and `Cleanup` hooks. Every hook accepts a `TestDeadline` and its
own `CancellationToken` and returns `Task<Result<TestStepResult,string>>`.

`TestStepResult` owns its `TestOutcome` and optional `TestFailure`. A successful
step has no failure; Failed/Cancelled steps own one. Use an instance of
`TestSteps.From` to convert an existing `Result<void,TestFailure>` assertion
without losing its code or message. The outer string Result is task/infrastructure
transport only. The runner converts unexpected ordinary errors and failed
transport into typed `test.unexpected_error`; cancelled transport becomes typed
`test.cancelled`. Domain assertion payloads remain unchanged.

Construct `AsyncTestRunner` with a host `TestClock` exposing synchronous
`Result<int,string> NowMilliseconds()`. The clock must be nonnegative and
monotonic; failure, cancellation, regression, negative readings and expiry
arithmetic overflow become typed `test.clock_error` failures.

`Run(test, bodyBudgetMilliseconds, cleanupBudgetMilliseconds, bodyToken,
cleanupToken)` requires positive budgets before setup. Setup and body share one
absolute deadline; the runner checks it and cancellation before/after each phase.
An expired deadline is a Failed outcome with code `test.deadline_exceeded`.
Authored phase failures take precedence over subsequent deadline/cancellation
checks so the original typed assertion is retained.

This deadline is cooperative. Hooks must inspect the passed deadline and token,
use `TestDeadlines.RemainingMilliseconds` with the same clock, and pass remaining
budgets into bounded native operations. The runner does not interrupt arbitrary
hooks or create a native scheduler, timer or semaphore. Physical process/unit
watchdogs remain independent containment.

Every setup attempt owns exactly one awaited cleanup-hook attempt in genuine
source `try/finally`. Cleanup gets a fresh independent deadline and the explicit
cleanup token; use `CancellationToken.None` for normal mandatory cleanup.
Cancellation of body work must not contaminate that token or its deadline.
Even an explicitly cancelled cleanup token does not suppress the cleanup call.
If the clock fails while creating the cleanup deadline, the hook still receives
`TestDeadline(0)`, which is already expired for the nonnegative clock contract.
The hook can still release local resources; it must not start unbounded work.
Terminal language exhaustion still cannot promise a returned result or successful
cleanup within an exhausted finite language limit.

`AsyncTestResult` composes the existing `TestResult test` and independent optional
`TestFailure cleanupClockFailure`. Primary setup/body outcomes are retained;
`test.cleanupOutcome` and `test.cleanupFailure` preserve the cleanup step. A clock
failure before/after cleanup uses the extra channel and cannot erase a simultaneous
typed cleanup assertion. Any unsuccessful cleanup or cleanup clock fault makes
`test.outcome` Failed. A later clock fault does not overwrite the first cleanup
clock fault. Pre-setup cancellation skips every hook and retains the stable ID.

`RunAll` validates positive budgets, at most 128 entries and nonempty unique stable
IDs/names for the entire registry before hooks. It snapshots IDs/names and retains
declaration order, including pre-cancelled entries. Cases must keep identities
stable for their lifetime. `tests/testing-async` demonstrates an external installed
consumer with source-owned lifecycle/parameter/cancellation assertions and narrow
native observation/fake-clock ports.

External-effect adapters, property creation/shrinking, seeds, replay and schedule
exploration remain separate follow-up contracts; this module does not claim those
F31/C7 capabilities.
