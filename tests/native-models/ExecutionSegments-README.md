# Execution segment contract fixtures

Source-only pending qualification. `ExecutionSegmentsTests.cpp` generates ordinary
object, project/header, project/compiled and DI factory variants from one authored
fixture. `execution_segments_source` checks default/selected/lower work limits,
nested static/interface work sharing, rollback/finally, immediate vs actual host
and semaphore suspension, cleanup reserve, cancellation, independent interleaved
roots and committed segment preservation. `execution_segments_di` invokes actual
generated source constructors/methods through DI invoke/construct/start roots.
`execution_segments_runtime` checks exact counters, arithmetic admission, shared
parallel limits, owner affinity and unchanged initialization/cleanup semantics.

Compiler target: pipelang-execution-segments-compiler-tests.
Fixture selectors: execution_segments_objects, execution_segments_project,
execution_segments_compiled, execution_segments_di_project,
execution_segments_di_compiled, execution_segments_runtime. Existing fixture lanes
run normal and ASan/UBSan consumers; parent controls sequential containment.
Runtime linkage can be header-only or the existing linked-runtime selection.

The host chooses evaluationStepsPerSegment through TaskOptions. Defaults remain
65536. A real normal continuation starts another selected segment, while awaited
source descendants and immediate completions share their current segment.
This is not a total async work or time limit. No consumer policy is implemented.
No tests/compiler invocation have run as part of authoring this fixture packet.
