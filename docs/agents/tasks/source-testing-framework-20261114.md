# Authored testing framework and first host journey

Status: bounded reusable testing library and first Linux local-host product journey
locally qualified, 2026-11-16. F31/C7 remain incomplete.

## Ownership and API

The installed [PipeLang.Testing library](../../../sdk/libraries/testing/README.md)
owns the API and cooperative deadline contract. Cases, typed parameterization,
registration, sequencing, assertions and setup/body/cleanup outcomes are authored
in PipeLang. Synchronous consumers retain their API; asynchronous consumers
explicitly import `PipeLang.Testing.Async`. Native ports provide bounded effects,
observation and clocks. The native entrypoint composes ports, pumps the model and
consumes its typed result; it does not replace the source runner or scheduler.

The [inventory](../../concepts/pipelang-foundation.md),
[delivery ledger](../../concepts/pipelang-foundation-delivery.md) and
[C7 requirements](../../concepts/pipelang-foundation-contracts.md#c7--determinism-testing-replay-and-debugging)
retain broader obligations. This record links the canonical contracts rather than
duplicating the testing API.

## Local qualification

The exact selected final SDK passed the complete declared Linux test workflow with
all three subset selectors unset, explicit `PIPELANG_NATIVE_RUNTIME_LINKAGE=linked`
and `PIPELANG_CXX_RESOURCE_PROFILE=gnu-conservative-gc`. The complete workflow
retains its default suites, runtime cases, fixtures, original source assertions,
resource limits, normal optimization and full sanitizer debug information.

| Consumer | Qualified scope |
| --- | --- |
| [Synchronous tests](../../../tests/testing) | Unit/table results, registration validation, primary/cleanup dual failure and pre-cancellation. |
| [Asynchronous tests](../../../tests/testing-async) | Genuine suspension, nested interface runners, setup/body/cleanup failures, cooperative deadlines, independent body/cleanup tokens and separate cleanup-clock failure retention. |
| [Async-finally regression](../../../tests/native-models/AsyncFinallyTests.cpp) | All five generated modes, normal and full-debug ASan/UBSan/LSan: objects, project, compiled, probe and installed SDK runtime. |
| Installed linked runtime owner | Two models share one STATIC runtime; relocation and ten specific refusal contracts pass normal and full-debug sanitizer execution without private SDK headers. |

See the canonical [project build contract](../../concepts/pipelang-projects.md#opt-in-linked-native-runtime)
for explicit selection, immutable producer policy and executable link-domain
limits. The original 58-assertion linked project gate includes both runtime
compilations, archive/link and execution inside one unchanged 900-second workload.
Linux leak detection remains enabled. The selected source retains the baseline
compiler/generator; parked IR experiments are excluded.

Header-only remains the default. Its original full-debug project gate remains
unqualified, including with the GNU profile; the historical reduced-debug profile
result does not establish full-debug acceptance. The retained untuned GCC timeout
also remains a failure. No installed SDK/profile default or consumer pin changed.

## Scoped cancellation probes

The additive `IProbeTestCase` and `TestRunner.RunProbe`/`RunAllProbes` API keeps a
separate body probe token method-scoped. The normal token continues to govern
setup, body admission and cancellation outcomes; cleanup remains independent.
A genuinely pre-cancelled probe can therefore test cancellation handling without
causing the runner to skip the test. Existing synchronous APIs are preserved.

The exact immutable Testing library descriptor passed the original five
synchronous and twenty-five asynchronous cases, plus nine additional Source-owned
probe contracts, normally and under generated/native ASan/UBSan/LSan on Linux.
The retained Windows VM passed the original five synchronous and nine probe
contracts against its pinned installed SDK. Windows sanitizer and asynchronous
probe qualification are not established by that run. Both owning test lanes now
include the additive probe target. These are local, uncommitted results; no
installed SDK default or consumer pin changed.

## First Dockpipe host journey

The exact final SDK and explicit GNU profile passed normal and fully instrumented
producer/consumer ASan/UBSan/LSan Linux execution of both stable source-owned cases:
workflow failure followed by fresh recovery, and graceful cancellation after
observed readiness. The product's existing compiled consumer is retained.

Source owns sequencing, assertions and recovery; bounded native process/file
adapters supply effects. Both proofs record accepted SIGINT requests, product
finally cleanup and complete owned trees, with no forced/emergency disposal,
unreaped processes or infrastructure failures. The private CLI's general headless
Linux signal repair separately passed focused CLI/PTY/failure checks, infrastructure
race regression and Darwin/Windows compilation. It is not a published artifact.

## Open work and acceptance boundary

Property tests, constraint-preserving generators/shrinkers, seeds, contract/state-path
coverage, effect replay, schedule replay/exploration/shrinking and complete semantic/
debugger integration remain open. Stable IDs and repeatable campaigns do not
establish deterministic replay. Windows/macOS, VM, interactive and full-platform
qualification remain separate. The earlier guest campaign remains paused.

Follow the [validation policy](../core/validation.md) and installed SDK boundary.
Preserve failed attempts and original fixtures and limits. This is local bounded
qualification, without commit, publication, deployment or default/pin promotion.
