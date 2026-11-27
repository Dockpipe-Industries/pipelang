# Native ordinary models and consumer architecture remediation

Objective: `pipelang-consumer-architecture-remediation-20261030`.
Authority: the owner requested addressing the existing audit before continuing the
compiler/UI roadmap, and confirmed ordinary async/await authoring.
State: completed at focused Linux compiler/headless/installed-SDK scope.
Declared root build/check/test workflows and compatibility checks passed.
Execution skill: `dorkpipe-objective-execution`.

## Language and ownership

The separately selected C++20 `pipelang-modelc` owns ordinary record/enum/class
semantics, constructors, pure properties, typed Results, checked signed64
arithmetic, lazy typed conditionals, private/public synchronous composition and
transactional action mutation. It reuses the native lexer. New behavior has no Go
semantic/runtime/emitter duplicate. Existing Go/Core/profile paths remain their
reference/default and keep their own admitted grammar and generated artifacts.
This is a coherent native language capability, not a new application-opcode list.
Its source, typing, ownership and resource contracts live with
`src/native/model-compiler/README.md` and `sdk/pipeview/model`.

The native view compiler resolves public fields, nested record paths, zero-arg
pure properties and typed methods against the native manifest. It generates the
schema, snapshots, checked event entrypoints, stable binding identities and
projection/inspection metadata. The optional ForgePipe bridge consumes exact
installed targets; headless model/runtime users acquire no renderer dependency.
Public snapshots exclude private state/services. Model/value methods cannot leak
services through records or generic Result payloads.

Counter now owns its entire Increment/Reset/edit behavior. Both settings consumers
own ordinary typed models. SettingsValue aliases the source-generated record;
C++ no longer copies its schema into variant maps or reconstructs save snapshots.
The storage codec retains format migration, file/symlink admission and atomic I/O.
Expected malformed text is refused at that owning boundary. Source methods own
validation, dirty/busy rules, statuses, failure detail and saved-baseline changes.

## Async/await and UI projection

Settings Save is one source async method. It captures an owned typed draft,
publishes pending state, awaits an injected typed storage service, and handles
Success/Cancelled/Failure before updating the saved baseline and visible detail.
The generic runtime owns creator-thread continuations, configurable bounded task
admission (hard cap 32), one FIFO host worker, cancellation, cleanup, stale-handle
safety and teardown joining. The application policy—not a Save opcode—prevents a
second save. Already committed host success remains success after late cancellation
or teardown; cancellation never promises external effect rollback.

Synchronous helpers share one outer transaction. Async transactions publish at
await boundaries and source returns, then refresh before owner resumption.
Host-admission refusals become typed awaited failures so authored cleanup runs.
Await callables and operations have explicit owned lifetimes and ownership
transfer. Generic local async composition, Optional, loops, inheritance and
exception/finally syntax remain refused; those features are separate work, not
claimed by this checkpoint.

ForgePipe's reusable adapter checks event/control/action/binding/payload provenance
and returns owned outcomes. Its atomic projection API validates all values and
enabled identities before mutation, preserves node addresses/layout/queued events,
and reconciles disabled focus. Authored pure availability/status/error bindings
feed the same projection used by rendering, dispatch and inspection. Empty event
batches restore presentation staging from the authoritative model. Sensitive
inspection values remain redacted unless explicitly exposed.

## Material composition

The additive native-only `pipelang.cpp.float64-calls.v1` / private v4 contract adds
same-owner typed calls to the existing binary64 conditional subset. It independently
checks signatures, forward references, graph cycles/depth and expanded work, and
evaluates arguments once left-to-right. Existing pilot/v2/v3 outputs remain closed.
ForgePipe composes its existing range/order predicates instead of duplicating them;
missing pinned native selection refuses at configure/generation. No new floating
arithmetic or solver/default behavior is introduced.

## Focused proof and remaining limits

Evidence estate: `/tmp/pipelang-audit-remediation-20261030/`.

- 1,024 independently oracled helper-call values plus native graph, ordering,
  laziness, transport/process and refusal cases passed.
- The unchanged installed baseline, rebuilt default and native helper SDKs match
  all 14,280 contact and 1,456 combine cases. Each rebuilt variant passes the root
  20 CPU CTests, and binding admission passes 13 Python cases. The generic UI
  projection CTest passes separately.
- Native model protocol/semantic/lifecycle fixtures and their ASAN/leak run pass,
  including conditional values, helper rollback, privacy, UTF-8, capacity/FIFO,
  owned captures, cancellation, host-admission failure, committed-success teardown
  and stale handles. The source frontend refuses 25 semantic and 4 protocol families.
- Actual model-to-view manifests interoperate. Native view/refusal, SDK manifest,
  output preservation and independent generated-adapter fixtures pass.
- Exact installed Counter, two-field settings and async persisted settings pass
  their 2+2+6 CTests. The persisted consumer additionally passes the codec/creation
  admission regression and inspection value/type/availability/redaction oracle.

Initial failures and provisional SDKs are retained. ASAN exposed an await-capture
use-after-free; explicit owned callable/awaiter lifetimes and transfer repaired it.
A compile-only reduction isolated lvalue-await lowering before the final normal
and sanitizer run. A real consumer's ordinary conditional expression drove the
native typed conditional capability. A cached CMake candidate was corrected using
fresh build directories and explicit exact package config paths. No compiler or
resource limit was switched to manufacture a pass.

All execution uses existing job.py/run.py limits: 1 GiB/unit, 800 MiB proactive,
zero swap, 2 GiB aggregate; the new estate is bounded at 8 GiB. This is focused Linux
headless/compiler/installed-SDK proof, not the previously incomplete full compiler
campaign, UI/GPU/physical input, performance, other-platform or release acceptance.
Original branches/indexes and unrelated bytes are preserved; MesaMoto is untouched.
There is no new commit, push, merge, publication or system installation.
