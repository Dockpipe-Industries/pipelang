# Authored async testing contracts

The `.pipe` source registers unit/table cases and drives a nested `IAsyncTestCase`
runner through real suspended hooks. It owns scenario selection, sequencing,
assertions and expected outcomes. The native executable composes ports, releases
its initial observation gate, pumps the model and checks/reports the returned
source unit-suite envelope. It contains no testing framework or test-case runner.

`FixturePorts` supplies only a deterministic monotonic clock with source-armed
one-shot faults, clock advancement, cancellation of an explicitly supplied token,
and a bounded asynchronous observation gate. The source hook passes its remaining
cooperative budget to the native observation operation. The gate records worker
execution and suspends for one millisecond. Source assertions check that the host
operation actually ran asynchronously. The native 20-second pump watchdog is
physical fixture containment; it is separate from the source deadline contract.

The source suite covers:

- Typed assertion identity/messages, cancellation payloads and remaining budgets.
- Three typed table rows and declaration order through the source interface.
- Setup/body/cleanup assertion failures, ordinary exceptions, task transport
  failure/cancellation, deadline expiry and setup/body lifecycle admission.
- Shared setup/body deadline, fresh independent cleanup deadline and exactly one
  cleanup-hook attempt after setup, including setup failure and expired body work.
- Cleanup clock failure before the hook (expired fallback) and after the hook,
  preserving an independent typed cleanup assertion at the same time.
- Duplicate IDs, invalid budgets and the 128-case registry limit before hooks.
- Pre-cancelled entries retaining IDs without hooks; cancellation after a suspended
  body with independent mandatory cleanup; and a cancelled cleanup token leaving
  normal setup/body work unaffected.

This standalone project uses the installed SDK and an external checksum-pinned
source-library descriptor through `SOURCE_LIBRARIES`. It defaults to the source
library root exposed by the selected installed SDK. A parent qualification may
set `PIPELANG_TESTING_LIBRARY_ROOT` to an immutable staged library tree explicitly.
That override proves external import, not installed library completeness.

Use separate normal/sanitizer build directories with the exact installed candidate
SDK. Configure with `PIPELANG_TESTING_SANITIZERS=ON` for the sanitizer directory;
the existing SDK CMake policy propagates instrumentation to the generated facade,
all generated implementation units and the native fixture. Use Release
`-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG` as with the existing reusable-library
qualification. Build only `testing_async_contracts` sequentially. Run normal and
ASan/UBSan binaries through the unchanged contained unit helpers; Linux retains
LeakSanitizer, Windows/macOS retain their existing no-leak and runtime-DLL policies.

No native compiler/runtime execution has been performed for this source stage.
The existing synchronous `tests/testing` suite must also be rerun because this
library descriptor now inventories async sources, even when a consumer uses only
the original synchronous API. Terminal language-limit cleanup and native owner
shutdown remain compiler/runtime qualification obligations; finite language limits
cannot promise successful cleanup or a returned AsyncTestResult after exhaustion.
