# Source execution segments and host-selected work

Ordinary model public calls default to 65,536 source evaluation steps. The native
host may explicitly select a different positive finite allowance when constructing
the model; source code cannot raise/reset its own limit:

```cpp
pipelang::model::TaskOptions selected;
selected.evaluationStepsPerSegment = admittedSteps;
Application model(services, selected);
```

`evaluationStepsPerSegment` is appended after the existing task capacity and
initialization fields. `TaskOptions{}`, `TaskOptions{capacity}` and
`TaskOptions{capacity, initializationSteps}` retain their existing defaults.
The TaskScope validates the selection before authored initialization, owns it
immutably, and exposes its value only on the owner thread. Normal pure model
queries already require that owner; this does not restrict previously portable
numeric constexpr/native projections or introduce cross-thread model access.

Each public query/action/async root starts a fresh invocation budget. Nested
ordinary constructors, helpers, source interfaces, generic/virtual/static source
calls and parallel workers share the same budget; they do not each gain a new
allowance. Independently admitted roots own independent counters. DI
ExecutionOwner uses its supplied TaskOptions for ordinary invoke/construct/start
roots as well. Its independent failed-construction unwind/close cleanup roots
retain their existing defaults.

A genuine normal host/semaphore suspension starts a new execution segment. Its
resume restores the selected normal allowance; immediate awaits and directly
awaited source descendants do not refill it. Source call ancestry remains bounded
at 32 across suspensions. The selected allowance therefore bounds each continuation
segment, **not cumulative async operation work or wall time**. A long operation can
have many real suspensions. Task capacity, cancellation, native I/O deadlines and
external containment remain separate controls.

Cleanup has a separate 65,536-step reserve, independent of normal selection.
Nested cleanup and cleanup awaits share that reserve, and reset while inside
cleanup never replenishes it. Exhaustion is terminal to source recovery: ordinary
source catches cannot turn it into success. Pure roots throw
EvaluationLimitFailure; action/task boundaries return their existing Failure
transport. Cancellation retains its existing distinct outcome and precedence.
A failure rolls back the current ordinary-state transaction, not external native
side effects or earlier committed async segments. Increasing normal steps does
not alter publication/refresh points or suppress cleanup.

The admitted native value is any positive std::size_t, bounded by that counter's
arithmetic representation; 0 is refused. External configuration must validate sign,
integrality and representability before conversion. The runtime checks before
decrementing and never multiplies an allowance by worker count. A larger selection
is a deliberate host policy, not proof of feasible CPU/RSS or bounded native work.
Record the selected value with consumer proof and keep existing OS/job/unit memory,
task, time and artifact limits. No environment variable or global setter changes
runtime policy.

Initialization is independent: initializationSteps still defaults to 65,536 and
admits 1..1,048,576 for one-time root setup. EvaluationBudget::forInitialization
changes only its first normal allowance and retains the old reset-to 65,536 rule.
EvaluationBudget::forInvocation retains its selected normal reset seed. The legacy
EvaluationBudget::maximumSteps constant stays65,536, including its independent
atomic-contention retry use. String/collection/object ownership limits and compiler
admission profiles are unchanged. More ticks do not admit larger source values.

TaskScope/EvaluationBudget layouts and generated root budget creation change
together. Rebuild compiler/generated code, header-only or linked runtimes, and
consumers against one exact SDK manifest. Do not combine old compiled objects with
new headers. This is a C++ host SDK capability, not a new source syntax, compiler
resource-budget field or promise of a complete consumer policy migration.

`tests/native-models/ExecutionSegments-README.md` records focused fixtures and
selection names. New behavior requires those source/generated/runtime/DI and
regression checks to pass under unchanged containment; authoring alone is not
qualification.
