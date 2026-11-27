# Reusable PipeLang dependency injection

Status: source declarations, checked construction, owned synchronous resource
cleanup and scoped async service methods are locally qualified on Linux native,
ASan/UBSan and headless WASM. The owned-lifecycle completion objective is complete.
Before this work, ordinary constructor injection
existed without source registration or a lifetime container. Do not present either
constructor injection or graph plans alone as a completed DI container.

Latest qualified local candidate: SDK10, preserving the separately approved
collector follow-up. Earlier sections retain historical checkpoint evidence;
the final section records the current contract, hashes and qualification limits.

The approved first checkpoint adds project-source `public composition Startup`
declarations and a checked, inspectable plan. Registrations explicitly select
`singleton`, `scoped` or `transient`; borrowed inputs explicitly use
`singleton input IClock;` or `scoped input IRequest;`. `include Shared;` imports
another independently valid composition; included exports are not implicitly
re-exported. `export UseCase;` declares a composition entrypoint. There is no
automatic registration or unregistered construction. Every authored composition
and registration is validated, including registrations unreachable from exports.
Open registration fragments are outside this checkpoint.

The first phase owned source parsing, per-file lexical module binding, checked-AST
constructor/type/lifetime graph validation, and a versioned compiler JSON plan.
That graph-only checkpoint did not emit construction, caches, scopes or cleanup.
Ordinary constructors, interfaces and manual `new` retain their existing behavior.
Qualification uses a fresh snapshot derived from optimized SDK manifest
`74d5034dc5b927eae7848f91ad933982a824d51e06537bdd512c0ba5fc321bad`,
with the prior qualified runtime unchanged; unrelated numeric work remains intact.

Implement a reusable PipeLang module for typed dependency registration and
composition, including singleton, scoped and transient lifetimes. Validate missing
dependencies, cycles, duplicate/ambiguous registrations and invalid lifetime
capture. Application/domain classes keep explicit constructor dependencies; use
the container at composition roots, not as an injected universal service locator.
Use built-in Result success/cancellation/failure for fallible application-facing
operations and deliberate owned resource cleanup where supported.

For web applications, scopes must isolate request/tenant-specific services.
Singletons cannot capture request-bound identity, tenant/database handles or scoped
services. Establish ownership, concurrent resolution, cancellation, failed
construction and reverse dependency cleanup semantics explicitly. Do not let a
registration name select arbitrary tenant storage or grant authority.

Inspect actual language capabilities before choosing the implementation API.
The current ordinary-object documentation identifies generic classes/interfaces,
source downcasts and runtime reflection as unsupported. Do not work around a
proven general language gap using string service lookup, unchecked casts, copied
C++ application policy or special Cloud compiler opcodes. Prefer statically checked
composition suitable for native and WASM/AOT; introduce general compiler/runtime
capabilities only when necessary and qualify them independently. Keep the module
independent of Qt, HTTP, Cloud identities and storage implementations.

Keep one substantial type per file, cohesive domain/application/infrastructure
boundaries and readable authored code. Validate successful composition, interface
substitution, invalid graphs, singleton/scoped/transient identity, scope isolation,
construction failure/cancellation and cleanup/use-after-scope refusal. Preserve
existing constructor-injection fixtures and unrelated work. No deployment, live
service startup, Git publication or new paid dependencies is implied.

## Checked source plans, 2026-11-08

The contextual project-source grammar is:

```text
public composition Startup {
    singleton input IClock;
    scoped input TenantContext;
    singleton IPolicy = Policy;
    transient UseCase = UseCase;
    include Shared;
    export UseCase;
}
```

Referenced types must be declared and admitted through the originating file's
direct imports. Composition declarations have their own linked identities and
participate in the existing declaration/token/output resource budgets. The
planner consumes the normalized, checked AST, including ordinary implicit default
constructors, visibility, interface compatibility and established model source
facets. It never treats an independent native owner as a service reference.

This checkpoint supports nongeneric service bindings and synchronous constructors.
Each constructed service requires one accessible checked public constructor;
multiple public overloads are refused rather than selected heuristically. Inputs
are explicitly singleton or scoped. Plain primitive/Record/Enum inputs are
classified as future value snapshots; source-object and interface inputs are
classified as borrowed. Host interfaces are constructor-only inputs and cannot be
exported. No runtime instance is copied or created by this graph-only phase.

Successful project compiler responses with compositions add `composition_plans`:
an array of `pipelang.composition-graph.v1` objects marked `checkpoint: graph-only`.
Plans expose source identities/origins, lifetimes, dependencies, constructor
signatures, local exports, root/scope availability and deterministic recipe order.
The SDK strictly validates the schema/references/order before atomically publishing
`CompositionPlan.json` and its digest in the artifact receipt. Ordinary projects
omit it. CMake's explicit `COMPOSITION_PLAN` option requires that artifact and
exposes `PIPELANG_COMPOSITION_PLAN`; SDK CLI equivalent is
`--require-composition-plan`. No source scanning or optional missing byproduct is
used to infer a dependency.

Fresh final candidate: `/tmp/pipelang-composition-20261108/sdk-final`, manifest SHA256
`2a8c5e6a1ff66ab025d86ea2ab27117792c691478eb61eff581876d15ce436bb`.
It derives from the coherent optimized baseline; runtime model headers remain
byte-identical, and optional new lifecycle headers are not selected. Broad local
acceptance passed 96 graph assertions, 15 metadata-schema checks, 25 installed-SDK
artifact checks, 55 resource-budget checks, the existing 67 SDK/tooling cases and
419 extended compiler/view oracles. Three ordinary responses, including a
1,024-type closure and negative diagnostic, were byte-identical to the baseline.
The independently reviewed metadata correction then passed 18 schema checks and
28 installed-SDK checks in a fresh prefix: root/scope availability is derived from
the lifetime/dependency DAG rather than accepted as arbitrary boolean flags.
The compiler binary, graph planner, runtime and other SDK code were unchanged;
the complete earlier suites were not repeated for this verifier-only correction.

Evidence: `/tmp/pipelang-composition-20261108/REPORT.md`,
`proof-resume-6`, `proof-final` and `equivalence`. The initial O3 build peaked at
604,286,976 bytes; broad acceptance took 82.90 seconds and peaked at 452,493,312
bytes. The verifier correction took 8.97 seconds and peaked at 338,722,816 bytes.
These jobs had no memory events and unchanged caps. Earlier harness/diagnostic failures and the unavailable
Ninja configuration are retained. Actual incremental/rebuild artifact checks used
installed Unix Makefiles; Ninja, compiler ASan/UBSan, Windows/macOS and WASM
qualification are not claimed. Factory lowering, scope caches, cleanup, owned
construction failure/unwind and scope-task integration remain unfinished.

## Optional native lifecycle primitive, 2026-11-08

The new `sdk/pipeview/model/native/include/pipelang/di` headers contain the first
owner-thread-confined `ScopeState` bookkeeping primitive. Explicit scope-bound,
non-RAII tickets account for admitted construction, operations and child
lifetimes. Open/Closing/Closed is monotonic and outside ordinary graph rollback;
new admissions fail after Closing. Owned records retain construction-attempt
identity and compiler orchestration keys; borrowed instances create no cleanup
record. Cleanup claims advance once in reverse creation order only when generated
orchestration acknowledges completion. No callback, service registry or actual
service instance is stored in this primitive.

`ScopeLimits` configures activity and owned-record capacities, each checked from
1 through 4,096 to align this bookkeeping slice with the current owner domain's
object-cell bound. Default capacity is 128 for each ledger, with storage allocated
only as entries are recorded. Opaque stamps never wrap/reuse; exhaustion and
wrong-owner/stale/duplicate tickets fail with stable safe diagnostics. Destructors
perform no cleanup or owner/parent lifecycle mutations. Finishing an activity is
accounting, not permission to publish a service after Closing. Failure/cancellation
cleanup outcomes advance attempted-work progress without promising disposal
succeeded or permitting automatic replay.

The isolated declared workflow in `/tmp/pipelang-di-lifetime-20261108/workflow`
staged only these optional headers alongside checksum-verified installed SDK
`74d5034dc5b927eae7848f91ad933982a824d51e06537bdd512c0ba5fc321bad` runtime
headers. Normal and ASan/UBSan each passed 16,458 assertions, including actual
GraphModelTransaction rollback, capacity edges, technical stamp exhaustion,
owner refusal and exactly-once cleanup progress. Proof is
`/tmp/pipelang-di-lifetime-20261108/proof-1`; its receipt SHA256 is
`d506cb6e87fe460fd630fa287ad4c55a37f28ae9975663916723907ebe80e10b`.
The installed SDK and existing object/task headers were unchanged.

This is selected local native proof only. Integration of this lifecycle primitive
with compiler/source declarations,
failed-construction unwind, actual child closure/resource disposal, scoped task
draining, source-reference revocation, full DI/container behavior and WASM remain
unfinished. In particular, a cleanup claim is not a source callback invoked from
inside a native host call; future typed orchestration must execute source work
through the owner's permitted execution path.

## Optional shared execution boundary, 2026-11-08

`pipelang/di/ExecutionOwner.hpp` and `ScopeExecution.hpp` now provide a separately
qualified native synchronous execution seam. A shared owner contains exactly one
existing ObjectDomain/TaskScope, with task teardown preceding domain teardown.
Contexts retain their owner and parent without an owner-to-wrapper cycle. Shared
lifetime does not imply thread safety: all access and final owner release remain
on the constructing thread, matching the existing runtime's affinity contract.

`checkOpen` guards cache hits without starting a graph transaction; invocation
checks shared callback reentry, root/ancestor lifecycle, pre-cancellation and an
idle TaskScope, then uses the existing GraphTransaction/ObjectExecution path.
Generated orchestration is responsible for typed caches and no-throw publication.
Every child, including descendants, reserves a root admission ticket; nested
children also reserve their immediate parent's ticket. Thus configured root
activity capacity bounds all live child contexts. Explicit child closure completes
those tickets; root closure remains busy until all children/work finish, then
closes the whole domain. Child closure does not revoke escaped raw source refs.
Destructors do not close scopes or release admission tickets automatically.

Ordinary explicit source Result values retain their existing graph-commit
semantics inside the generic native invocation result. Generated public façades
must flatten that private boundary after commit/cache publication. A successful
callable outcome is not blindly relabelled Cancelled after return. The approved
pure-construction profile needs a final cancellation point within generated
construction orchestration before it returns to graph commit; source lowering
qualification owns that distinction.

The isolated declared workflow under `/tmp/pipelang-di-execution-20261108/workflow`
staged only optional DI headers alongside verified SDK manifest
`74d5034dc5b927eae7848f91ad933982a824d51e06537bdd512c0ba5fc321bad`.
Normal and ASan/UBSan each passed 38 shared-execution assertions and the 16,458
lifecycle assertions. Proof: `/tmp/pipelang-di-execution-20261108/proof-1`;
receipt SHA256 `74d4ffa54598e519e8ad0cad4aff279e363c5d4f82354d64a628b082cc0d4a7c`.
No existing object/task runtime or installed SDK bytes were changed.

This native proof does not establish generated cache lifetimes, source DI
integration, owned-resource disposal, failed-construction resource unwind,
asynchronous scope draining, child-reference revocation or WASM. In particular,
`recordInstance` may allocate; future resource-owning construction must pre-admit
bookkeeping or safely dispose an untracked instance on tracking failure. Failed
construction cleanup must run while provisional references are still usable,
before graph rollback, retaining earlier published dependencies. No native host
callback may reenter source to perform that cleanup.

## Synchronous source construction, 2026-11-09

Authoring and the supported contract are documented in
[Source dependency composition](../../concepts/dependency-injection.md).
The selected local candidate is
`/tmp/pipelang-composition-factory-20261108/sdk-6`, manifest SHA256
`5f426dd3504d98b6d481ec608fd9ea8d7771a764791637fdf4ac95110df47df8`.
It extends the coherent checked-plan candidate without selecting unrelated dirty
numeric work or changing installed defaults. Core model runtime headers remain
byte-identical to the prior qualified runtime; new optional `pipelang/di` headers
are inventoried alongside them.

Explicit `COMPOSITION_FACTORY` / `--composition-factory` generates
`CompositionFactory.hpp` from the checked Program and CompositionGraph. The SDK
requires the matching plan, tracks artifact hashes and factory selection in its
receipt/compiled-plan identity, removes stale output and declares required CMake
outputs. Header-only and compiled projects are supported. The compiler uses checked
constructor/method identities and type mappings, never a deserialized JSON recipe.

Typed root/scope accessors construct lazily with staged caches and retained
transients. Singleton dependencies use root context even when first requested in a
child. A single owner domain/task scope backs its children; native monotonic scope
state remains outside source rollback. Failed/cancelled construction cannot publish
new caches. Constructor callbacks check cancellation before commit. Public methods
preserve existing transaction/Result semantics and flatten string-error Results
after commit. Ordinary manual construction remains unchanged; checked native
Construct/SourceMethods bridges enable an equivalent manual baseline.

Owner wrappers are noncopyable/nonmovable. Handles remain copyable/movable, reject
moved-from use, and reject operations after explicit close. Wrong-thread use and
callback reentry fail before source work or lifecycle mutation. Scoped inputs are
owned value snapshots or explicitly borrowed host ports. No provider is injected
into application/domain code.

The bounded constructor profile admits ordinary transactional source work and pure
owned host reads. Transitive effectful host calls, shared-handle mutation/wait/release,
locks and parallel work refuse with source locations. Async/reference-bearing
facades, borrowed source-object inputs, nonstring Result errors and ambiguous
native facade names/signatures remain explicitly unsupported. No source resource
disposal, partial-acquisition unwind, fallible factory registration or scoped async
task draining is claimed. Explicit scope close is required, including failure paths.

Qualification evidence:

- `proof-1`: compiler/SDK build plus 96 graph assertions, 18 schema checks,
  47 installed-SDK ownership/CMake checks and 55 resource-budget contracts PASS;
  153.59 seconds, 612,507,648-byte peak, no memory events.
- `proof-6`: final constructor profile and maintained profile target each PASS
  46 assertions; maintained scope/execution targets PASS 16,458/43 assertions;
  47 installed SDK checks PASS. 12.16 seconds, 330,178,560-byte peak, no events.
- `/tmp/pipelang-di-integration-20261108/sdk-5-native-job.json`: HEADER_ONLY normal,
  COMPILED normal and COMPILED ASan/UBSan each PASS 11,134 assertions, plus independent
  owner threads and an actual namespace-shadow consumer. 152.95 seconds,
  742,686,720-byte peak, no events. Includes 2,200-scope churn beyond cumulative
  cell capacity, rollback/retry, close and host reentry, token/Result semantics,
  owner special-member static assertions and moved-from handles.
- `profile-equivalence/job.json`: all generated native header/compiled fixtures,
  namespace-shadow and real Cloud outputs byte-identical SDK5 to SDK6; all runtime
  headers byte-identical. This explicitly permits retaining SDK5 execution and
  timing evidence for the final profile-only refusal correction. 3.39 seconds,
  22,478,848-byte peak, no events.
- `/tmp/pipelang-cloud-di-20261108/proof-sdk5`: actual Cloud `Startup.pipe` normal
  and sanitizer consumers PASS 336 native/source policy comparisons each, fresh
  revocation, request isolation, failures/cancellation and closed-handle refusal.
  27.05 seconds, 482,844,672-byte peak, no events. Production route is unchanged.
- `/tmp/pipelang-cloud-di-20261108/wasm-sdk6-resume`: the same Cloud source composition
  PASS as headless WebAssembly under existing Emscripten 3.1.56 and Node 16.20.0,
  no pthread flag. Uses an isolated temporary cache and the unchanged SDK6 runtime.
  54.10 seconds, 450,064,384-byte peak, no events. This is no Qt/browser UI,
  asynchronous WASM or full Cloud frontend qualification.
- Required PipeLang repository check and whitespace checks PASS. Native execution
  uses the unchanged qualified runner/caps. Windows/macOS/ARM execution, new compiler
  sanitizer qualification, full dirty-checkout numeric work and publication remain
  outside this evidence. Earlier harness failures/interrupted lanes are retained.

The matched benchmark packet is
`/tmp/pipelang-di-integration-20261108/{REPORT.md,benchmark-summary.json,benchmark-sdk5.jsonl}`.
Seven alternating sample pairs use the same source graph, owner execution,
transactions, staging, guards and retention. Allocation counts/bytes match in both
lanes. Median generated/manual startup is 7.316/7.310 microseconds; cached singleton
resolution 59.5/77.6 ns and cached scoped resolution 68.6/101.5 ns, both zero allocation.
Guarded read-only method calls are about 10.3 microseconds and request one 65,536-byte
allocation in both lanes. This is shared collector scratch overhead, not a claimed
DI speedup. Source-cell/logical-byte and per-phase CPU counters were not measured.

A separate collector optimization proposal was prepared after that measurement.
Automatic approval review rejected expanding the owning GC edits without direct
user authorization. All exploratory object.hpp edits were restored exactly; SDK6
retains object.hpp SHA256
`fdc33bfeda56718ce522236f0fcfc3a20e27ea4589a4bf1af93a6750c9f00f07`.
The unapproved patch and concrete witness plan remain solely under
`/tmp/pipelang-root-set-gc-20261109/proposal/`. They have not been built or selected.
The user subsequently approved separate GC work; see the
[GC task](object-graph-gc-readonly-20261109.md) for its implementation and new-candidate
qualification. SDK6 remains the unchanged control. No GC improvement is claimed
until that candidate is measured. Owned-resource and scoped-task lifecycle work remains
separate from this qualified synchronous construction checkpoint.

## Approved collector follow-up

The user approved the separate collector work after reviewing the concrete
proposal. It is now implemented and qualified in SDK7, manifest
`092564bf9469f7d5b192cf181d782b48dc9602dc536d6fee008f4d64e1a1ba48`.
All source-object consumers were rebuilt for the native layout change. Native
header/compiled/sanitizer, real Cloud normal/sanitizer, and fresh headless WASM
consumers pass. The [GC task](object-graph-gc-readonly-20261109.md) records exact
proof and the retained unchanged SDK6 control.

Steady synthetic guarded reads improve about 36.7x (10.2 microseconds to 0.278
microseconds) with no allocation after the required pending sweep. The first
pending read still requests 65,536 bytes. ABBA measurements retain a 6–8% transient
construction regression; this is a specific read-path improvement, not an overall
DI speedup claim. The canonical authoring and lifecycle boundaries are unchanged.

## Completion objective: disposal and scoped async, 2026-11-09

objective_id: PipeLang-DI-owned-lifecycle
state: completed
execution_skill: dorkpipe-objective-execution
execution_authority: explicit_user_request_20261109
checkpoint_policy: automatic_within_objective
handoff_policy: user_requested_only

Finish owned-resource disposal and scoped async DI on the qualified SDK7 baseline.
Do not stop at a runtime primitive or graph-only checkpoint. Completion requires
actual source factory/cleanup/async service consumers, failed construction unwind
before graph discard, reverse cross-lifetime cleanup, no borrowed disposal, retained
primary failures/cancellation with separate cleanup outcomes, monotonic exactly-once
progress, scoped cancellation/draining before resources close, hostile reentry and
owner checks, native normal/sanitizer and suitable headless WASM evidence, and an
updated fair manual/generated benchmark. Preserve current GC temporal invariants,
explicit source wiring, type safety, no reflection/provider injection, resource
caps and unrelated dirty work. No commit, push, publication, installed default,
dependency pin, production route, live service or provisioning is authorized.

Use an explicit source cleanup selector resolving a nominal source interface and
checked method identity, with Result<void,string> and an independent cleanup token.
Use explicitly registered instance factories rather than adding a general static
method feature solely for DI. Factories retain responsibility for partial resource
acquisition until successful handoff. Construction and cleanup remain synchronous;
async factory/disposal extension is not implied. Async service tasks must be owned
and drained by their invoking scope. Native cancellation cannot force completion
of an uncooperative blocking host callback; do not falsely report such work closed.

### Owned lifecycle runtime evidence, 2026-11-09

The optional `pipelang/di` runtime now pre-admits typed resource reservations,
checks fresh same-transaction factory results, and journals ownership across
lifetimes. Failed construction unwinds unpublished resources in reverse adoption
order while the provisional graph remains live; prior published dependencies
remain owned. Cleanup uses an independent token and one bounded session budget.
A pre-claim budget refusal leaves uncalled work pending; a claimed failure or
cancellation is recorded as an unknown external outcome and is never replayed.
Closed with Failure does not claim every external resource was disposed.

Scopes share one ObjectDomain/TaskScope, with generic task groups for inherited
scope ownership. Closing cancels/drains the selected subtree before cleanup while
independent scopes remain usable. Coroutine frames retain their graph/budget and
owner-free scope state, avoiding executor cycles. Final facade/owner release uses
the same drain-then-cleanup policy. The owner-free `CleanupSession` observer retains
terminal results and numeric cleanup diagnostics after final release. Access and
final executor release remain owner-thread confined; native work must cooperate
with cancellation or enforce its own bounded deadline. Partial acquisition before
factory handoff remains the factory's responsibility. Borrowed inputs are not
disposed, and raw source-reference revocation is not newly promised.

Maintained native fixtures are `tests/native-di/ResourceJournalTests.cpp`,
`AsyncScopeExecutionTests.cpp`, `TaskGroupRuntimeTests.cpp` and
`OrdinaryExecutionAllocationTests.cpp`. Final normal and ASan/UBSan proof passed
38 journal and 36 async assertions plus exceptional group restoration and active
self-drain/shutdown refusal. Earlier affected generic task/synchronization/object
and GC regressions also pass normal/sanitizer, while the unchanged ScopeState
16,458 and updated recursive-close execution 43 assertions pass. Steady ordinary
invocation retains its stack transaction path: 1,000 guarded reads with a live
owned journal entry pass with allocations refused after the pending sweep.

Proof packet: `/tmp/pipelang-di-lifecycle-20261109`, including frozen source hashes,
`RUNTIME-REPORT.md` and `runtime-receipt.json`; final guard proof took 50.49 seconds,
peak 624,308,224 bytes, with no cgroup memory events and unchanged containment caps.
These native proofs use the verified SDK7 baseline plus selected runtime changes.
Generated source consumers, coherent installed SDK, WASM and matched benchmark
qualification remain executing under the completion objective above. This section
does not claim end-to-end completion or a new performance result.

## Owned lifecycle completion — SDK10, 2026-11-09

The completion objective is achieved locally. Source `factory Receiver.Create`
registrations return checked `Result<Concrete,string>` values; explicit nominal
`cleanup IResourceCleanup.Close;` selects `Result<void,string>` cleanup with an
independent token. Factories own partial acquisition until successful handoff.
New resources unwind before failed graph discard; prior caches survive. Cleanup
tracks reverse creation order across effective lifetimes, excludes borrowed
inputs, records failed/cancelled/unknown effects separately, and never replays an
attempted callback. Unclaimed work can resume in another bounded close session.

Scoped async methods use the existing source coroutine lowering and task scope.
Closing a scope cancels its subtree and drains continuations/finally before
resources close. Sibling scopes remain usable. Final owner release performs the
same safe drain; retained tasks and owner-free cleanup observers do not create
owner cycles. Root/scope/handle use and final release remain owner-thread confined.

Actual source tests exposed two defects that smaller runtime tests missed. C++
assignment evaluated an effectful RHS before the metered LHS and could lose an
acquired resource on cancellation before storing its Result. Synchronous lowering
now binds the checked LHS before the RHS, without an intervening cancellation
step. An independent fixture proves local and nested-record handoff, plus exactly
once source receiver evaluation. DI async execution also had an extra per-step
cancellation policy absent from ordinary async wrappers; it now uses ordinary
cooperative await/source-token semantics. Explicitly authored cancellation still
wins when the source chooses it. A host success alone does not promise that
subsequent source logic returns Success.

Candidate prefix: `/tmp/pipelang-di-lifecycle-20261109/sdk-10`.

- Manifest: `e0a9401762752e511ab7ed7bb3622bd0698daa12fbd50f0e77b6f305c39d22c2`.
- Model compiler: `11cf569da2a94ada9481ba7f9e6ae518de9f4bb9b28c06d36646a2875bdbd5a1`.
- SDK tool: `7ea12b6b1f986d912e4931a7123da5e5e682b649ef2d94bf82c445667f393dcc`.

The coherent candidate derives from qualified SDK7 source with owned compiler,
SDK and runtime deltas; unrelated dirty numeric work is excluded from its build.
This is selected qualification, not a full build of the mixed working checkout.

### Acceptance evidence

- `full-sdk-proof-2` under `/tmp/pipelang-di-lifecycle-20261109`: planner96,
  profile45, graph admission34 and installed SDK63 checks pass. Final lifecycle
  compiler42 checks pass in `terminal-proof-3`.
- `/tmp/pipelang-di-owned-lifecycle-20261109/sdk10-maintained`: declared
  `workflows/di.check` runs ordinary and lifecycle compositions in header,
  compiled and compiled ASan/UBSan profiles, three consumers each. All pass,
  including ordinary11134 and owned11145 integration assertions per profile,
  hostile acquisition/cleanup/budget/reentry cases and independent owner threads.
- `sdk10-final-drop`: final actual-source pending-task drop witness passes normal
  and ASan/UBSan against unchanged generated model libraries. Source finally
  precedes cleanup; retained task/observer do not retain the owner or host port.
- `sdk10-wasm`: actual PipeLang integration11145, scoped async46 and hostile28
  assertions pass under pinned Emscripten3.1.56/Node16.20.0. The cooperative local
  task path includes pending final drop. Native host-worker/foreign-thread cases
  run only in native/SAN; there is no WASM pthread claim.
- `terminal-proof-3`: modern compiler/model-view419 and compatibility172 oracle
  cases pass; lifecycle42, resource-budget55, ScopeState16458, execution43 and
  maintained journal/async/task-group/allocation tests pass. Assignment handoff12
  checks pass normal and ASan/UBSan. The stale64KiB source refusal became admission
  using the same bytes, matching the previously approved source-scaling policy;
  explicit resource budget refusals remain covered. Frozen inputs were preserved.
- `/tmp/pipelang-cloud-di-20261108/proof-sdk10`: existing Cloud policy336 cases
  each normal/SAN plus actual source Startup/request isolation/fresh authorization,
  Results and closed handles pass. `wasm-sdk10` freshly recompiles the newly
  generated Cloud composition and passes headless. No production route changed.
- `repository-check-2`: declared repository check and execution audit pass;
  no Go/Python sources or workflow toolchains were added.

The unchanged runner/caps apply: 2GiB job, 1GiB unit, 800MiB proactive stop,
zero swap and sequential heavy compilation. The six-profile native job took394.30s,
peak905,527,296 bytes. Sanitizer compilation recorded memory.high throttling
(539 aggregate high events), with no max/OOM events or raised limits. Final core,
assignment, Cloud and WASM receipts retain their own exact commands and accounting.
Initial SDK9 failing source fixtures and earlier proof packets remain preserved.

### Measured costs and limits

Seven alternating generated/manual lifecycle pairs pass348 trace/parity checks
before measurement. Both lanes use the same checked construction/cleanup bridges,
journal, ownership and task guarantees. Shared root startup requests about249KB
in19 allocations (generated249,240/manual249,192 bytes), medians7.605/7.692us.
Cached singleton/scoped resolution requests zero allocations, with generated
medians34.2/46.3ns. First owned singleton construction is57.029/53.120us;
owned transient26.124/26.412us; 17-resource scope disposal1.628/1.616us per
resource; local async admission13.487/13.416us; cancel/drain/dispose13.026/13.330us.
These are synthetic descriptive medians, not a general speedup claim. Generated
transient retention and diagnostic keys have measurable extra allocations;
the complete table reports requested bytes, not resident memory.

Detailed receipts, raw samples and limits:
`/tmp/pipelang-di-owned-lifecycle-20261109/REPORT.md`,
`final-summary.json`, `sdk10-lifecycle-benchmark.jsonl`, and
`/tmp/pipelang-di-lifecycle-20261109/RUNTIME-REPORT.md`.

Factories and disposal remain synchronous; general async acquisition/disposal is
not implied. Cancellation cannot force-kill an uncooperative native callback;
its port must honor cancellation or a bounded deadline. Failed cleanup can leave
an uncertain external effect requiring application reconciliation. Native ports
are synthetic lifecycle witnesses, not proof of every real file/socket driver.
Windows/macOS, browser UI, hosted/release and production deployment are unqualified
by these local runs. No commit, push, publication, SDK default/pin change, live
startup, provisioning or production/frontend migration was performed.
