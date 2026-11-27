# Developer foundations before further Cloud porting

Status: Byte iteration, synchronous typed-error transport, general Result
propagation and the common result interface locally qualified; broader foundation work remains executing. User-selected priority, 2026-11-11 (recorded 2026-11-12 UTC).

The owner asked to reduce generated/application boilerplate before expanding the
Billing port, including LINQ-style syntax, Result propagation, typed concurrency
ownership and SDK-based persistence. Preserve all existing Cloud work. Local
qualification only; no deployment, live migration, default SDK switch, consumer
pin update, commit or push is authorized by this task.

## Boundaries

PipeLang owns general language semantics, type checking, lowering and runtime
contracts. Framework SDKs own request contexts, data mapping and provider APIs.
Applications own domain invariants and transaction decisions. Existing database
libraries and migration tools provide persistence mechanisms. We are not building
an ORM, change tracker or migration engine as part of this foundation work.

Do not keep generating larger application ports around missing language features.
Implement a real general capability and qualify its installed/target behavior
before rewriting consumers. Do not patch generated C++, introduce application
opcodes into the compiler, conceal failed Results in successful records, or use
error-origin flags/strings to compensate for missing typed error transport.

## Current audit

- Ordinary foreach already supports owned collections. ByteBuffer is missing from
  the sequential sequence-element resolver, despite having an owned byte snapshot.
  It therefore forces manual index/At loops in Cloud validators.
- ByteBuffer.At exposes uint32, while native storage is uint8_t. Iteration must
  preserve language uint32, including overload resolution and bitwise operations.
- General callable/lambda values remain planned. LINQ query syntax and a general
  query-provider expression contract are not implemented by this task.
- Ordinary Result supports string, enum and owned-record errors. Synchronous model/
  host and exported composition boundaries now preserve typed errors, with separate
  invocation and domain outcomes. Native, sanitizer and WASM qualification passes;
  general propagation is now qualified. Async typed errors and functional
  composition combinators remain planned.
- Cloud has repeated IsCancelled/IsFailure branches and handwritten DTO shape
  checks. Generic propagation and generated codecs should replace mechanics
  while preserving error identity, cancellation precedence and domain validation.
- UI request IDs/view epochs protect stale responses; persisted revisions protect
  writes; mutation idempotency keys prevent duplicate effects. Their meaning and
  ownership differ. They must not become interchangeable string tokens.
- Current persistence uses libpq/PostgreSQL, explicit units of work and SQL files.
  Drogon is already a host dependency and offers mapping/transaction facilities.
  No PipeLang ORM or automatic schema-diff service exists.

## Delivery order and acceptance

1. **Sequential byte iteration — locally qualified.** Ordinary foreach over
   ByteBuffer binds uint32, snapshots its receiver once, retains break/continue/
   return behavior and consumes the existing evaluation budget. Local item changes
   do not mutate source storage. Mutation of the original buffer does not change
   the iteration snapshot. Parallel ByteBuffer iteration remains explicitly refused;
   existing parallel collection support must remain unaffected. Snapshot copying is
   existing owned-value behavior, not a claim of allocation-free iteration.
2. **Callable/lambda foundation — planned.** Define function types, contextual
   typing, generic inference, return/effect contracts and lexical capture ownership
   together. Qualify non-escaping and escaping lifetimes deliberately. No silent
   capture of scoped resources, accidental mutable cross-task cells or special
   application-only predicate syntax.
3. **Result composition/propagation — partly implemented.** Synchronous typed-error
   transport and general `try` expression propagation are locally qualified on
   native, sanitizers and WASM; Map/Bind combinators remain planned.
   This prerequisite does not depend on callable/lambda support. Preserve Success, Cancelled and
   Failure, exact error types/payloads, evaluation-once and cleanup order. Separate
   execution faults/resource exhaustion from domain errors. Extend typed model/host
   errors coherently; existing string-error ABI remains compatible. Expression
   propagation must return from the owning callable through finally/disposal,
   including inside loops, calls, short-circuit expressions and async boundaries.
4. **Collection operators and query syntax — planned.** Begin with Any/All,
   Where/Select and optional-returning lookups. Define ordering, short-circuiting,
   snapshot/deferred evaluation, effects, ownership, allocation and cancellation
   semantics before implementation. Materialization is explicit. from/where/select
   lowers through the same typed operators. Failable callbacks require explicit
   Result composition; a Result is never coerced into a predicate bool.
5. **Typed request/state contexts and codecs — planned.** Framework-owned validated
   contexts carry distinct request correlation, view/session epoch, persisted revision
   and idempotency types. Components receive and complete an admitted context rather
   than manually compare loosely related strings. Stale completion is rejected at
   the owning state boundary; genuine write conflicts stay explicit. Generate DTO/
   interop codecs from declared contracts while leaving domain invariants with their
   factories. Test cancellation, navigation, identity/tenant switches, reordered
   responses, duplicate mutation delivery and wrong-token-type rejection.
6. **Persistence SDK vertical slice — planned.** Reuse a maintained driver/provider
   behind typed query/projection, parameter, mapping and unit-of-work contracts.
   Preserve original database errors and unknown-commit outcomes. Prove one projected
   read, one conditional revision update, conflict, rollback, cancellation and
   uncertain commit. Domain reconstruction must respect invariants. Evaluate existing
   Drogon mapping before authoring equivalent machinery. A LINQ database provider
   receives checked/quoted query expressions, not opaque native delegates; unsupported
   translation is diagnosed without silently loading everything into memory.
7. **Migration tooling integration — planned.** Keep reviewed versioned SQL and
   qualify an existing open-source runner against current schema/history. Verify
   empty creation, previous-schema upgrade, ordering/history/integrity, failure and
   database-specific constraints/policies/triggers. Tool choice and historical
   migration adoption need evidence. No startup schema mutation or live migration
   is introduced here; generated model-diff migrations are not an initial promise.

Steps can be split for verification, but their semantics must remain coherent.
Do not publish a LINQ, ORM or ecosystem-complete claim from the first iteration step.

## Persistence, interop and licensing requirements

The default required ecosystem must permit proprietary/commercial applications
under their own licenses, without royalties, revenue caps or application-source
publication requirements. Required components must have suitable open-source
licenses. Preserve copyright/license/NOTICE obligations and provenance; this is
not a claim that there are literally no license conditions.

Current candidates: PostgreSQL/libpq (PostgreSQL License), Drogon (MIT), and a
separately qualified SQL migration tool such as golang-migrate (MIT). Flyway's
Apache-2.0 core is distinct from commercial extensions. Select only the necessary
open-source functionality after checking exact versions, dependencies and artifacts.
This is a candidate set, not an authorized integration or dependency-pin change.

PipeLang's optional native-dependency SDK already provides explicit C ABI integration
profiles. Extending a provider SDK must preserve ownership, cancellation, typed
failure and platform contracts, without adding managed runtimes to the compiler.
Custom JS/TS interop remains supported through explicit contracts.

Qt and other optional integrations have their own licensing/distribution obligations;
no blanket commercial-licensing guarantee is inferred for all adapters. Qualify the
specific modules and linking/distribution mode. This task does not relicense projects,
replace the selected UI backend or make commercial-only extensions mandatory.

Primary license references checked during the owner's design discussion:
- https://www.postgresql.org/about/licence/
- https://github.com/drogonframework/drogon/blob/master/LICENSE
- https://github.com/golang-migrate/migrate/blob/master/LICENSE
- https://www.apache.org/licenses/LICENSE-2.0
- https://doc.qt.io/qt-6.8/licensing.html

## Qualification and evidence

`workflows/foundation.check` creates an immutable source snapshot, rebuilds all
compiler-linked installed tools, and assembles an explicitly identified local SDK
candidate over an admitted SDK. Its manifest covers the resulting binaries/runtime
headers. Other unchanged tools retain their admitted provenance. This is a composite
candidate, not a full SDK rebuild, release or default installation switch.

Run the existing model/view oracle, byte and ordinary iteration/cleanup runtimes,
normal and ASan/UBSan/LSan profiles, refusal cases and actual WASM on emitted C++.
Keep logical and physical limits unchanged. Native/WASM proof does not establish
Windows/macOS, Qt-interactive or full-suite acceptance. Cloud bytes and current SDK
pins must remain unchanged; consumer cleanup follows exact producer qualification.

Proof estate: `/tmp/pipelang-foundation-20261112/`. Implementation status and exact
qualified scope are recorded there and updated below after terminal verification.


## First checkpoint evidence

2026-11-12: sequential ByteBuffer foreach passes the 423-case model/view oracle,
new wrong-element/shadow/parallel refusals, normal plus ASan/UBSan/LSan byte,
ordinary iteration and cleanup runtimes, and actual WASM. The same canonical
iteration fixture also passes installed SDK project/composition generation and
Result-facade execution on native and WASM. Snapshot, receiver-once, all 256 byte
values, uint32 overload/bitwise behavior and unchanged budget exhaustion are proved.

Both SDKs are explicitly composed local candidates with fresh manifests. Their
compiler-linked tools are rebuilt from the immutable current source snapshot;
unchanged other tools retain admitted baseline provenance. Current runtime headers
are included. This is not a full rebuild/release, consumer SDK-pin upgrade,
Windows/macOS proof or claim that general lambdas/LINQ are implemented.

Following this first checkpoint, callable/lambda contracts and lossless Result
composition remain before broader operator/query-provider and request-context implementation.
All Cloud working files and pending port slices remain byte-identical. Detailed
inputs, limits, acceptance receipts and proof boundaries are in
`/tmp/pipelang-foundation-20261112/REPORT.md`.

## Second checkpoint: synchronous typed host errors

2026-11-12: synchronous host methods, model actions and exported service facades
admit declared enum and deeply owned record errors without erasing their fields.
Source methods retain `Result<T,E>`. Newly admitted nonstring-error model actions
and composition handles expose `Result<Result<T,E>,string>` to native callers:
outer invocation faults/cancellation remain separate from the exact domain Result.
Existing string-error APIs and pure model-query signatures remain compatible.
No runtime Result representation or custom JS/TS interop was replaced.

Qualified through the 427-case model/view oracle, 49 composition-profile assertions,
six normal and ASan/UBSan/LSan runtime fixtures, and an installed-SDK source project
on native, sanitizers and actual WASM. Tests preserve structured/nested error
identity, enum and void outcomes, domain cancellation, thrown execution cancellation,
invalid UTF-8/enum refusal, caller-value validation, finally cleanup, transaction
rollback and scope/handle lifetime. Existing record ownership validation rejects
reference-bearing error records. Typed async errors remain explicitly refused.
The optional view profile retains its string-error contract.

At the second checkpoint, general propagation syntax, Map/Bind, lambdas,
LINQ/query operators, typed request contexts, DTO codecs and persistence SDK
integration remained planned. General propagation is qualified by the third
checkpoint below. No ORM,
provider integration, migration engine or consumer rewrite was introduced here.

Proof and candidate provenance: `/tmp/pipelang-typed-results-20261112/REPORT.md`.
Both candidates rebuild the four compiler-linked tools and install current runtime
headers over exact admitted SDKs; unchanged tools retain their baseline provenance.
Default SDK and consumer pins remain unchanged. No deployment, live migration,
commit or push occurred. Windows/macOS and full-suite qualification remain open.

## Third checkpoint: general Result propagation

2026-11-12: `try expression` consumes a Result in the owning Result-returning
callable. Success produces its payload; Failure and Cancelled return their exact
error type and payload even when the destination's successful payload type differs.
This is explicit compiler control flow, not an exception sentinel, generated-lambda
return, application helper convention or generated-C++ patch. Native typed-error
invocation envelopes remain distinct from the source/domain Result.

The checked lowering preserves evaluation-once, left-to-right calls/arithmetic,
lazy conditional and short-circuit arms, loop initialization/condition/iteration,
continue/break, foreach snapshots, assignable receiver identity, statement temporary
lifetimes, catch/finally behavior and transaction semantics. Transactional locations
are reacquired after await refresh; no borrowed state reference crosses suspension.
Pure helper propagation inside a worker returns from the helper without capturing
an owner. Direct propagation from parallel/finally bodies remains refused, matching
their existing return restrictions. Constructor/field initialization, non-Result
operands/returns, mismatched error types and discarded nonvoid payloads also refuse.
The existing ban on suspension in for iteration clauses remains explicit.

`try await` and propagation in await arguments use existing task ownership and
cancellation. Async errors remain string-only. Typed async error transport,
Map/Bind, callable/lambda values, LINQ operators, request contexts, codecs and
persistence SDK integration remain separate planned work. Custom JS/TS interop
and all Cloud source remain unchanged.

Qualification: 439 deterministic model/view admission/refusal cases, authored-token
diagnostics, 49 composition-profile assertions, eight normal and ASan/UBSan/LSan
runtime fixtures, a real gated native host suspension, and installed-SDK service
execution on native/sanitizers and actual WASM. WASM runs both the direct model and
source-object service facade, including local async calls and a typed synchronous
host port. Native worker/suspended-host proof does not claim WASM worker-thread or
new browser-adapter qualification. Windows/macOS and full-suite proof remain open.

The installed-project parser regression now has a module-level oracle: failed
speculative local-declaration parsing cannot leak a guessed type into expression
statements such as `try input;`. Project sanitizer compilation uses the producer's
established GCC memory controls, with unchanged sanitizer assertions and resource
caps. No language/runtime limit, default SDK, consumer pin or release state changed.

Exact candidate manifests, inputs, failed/retained attempts and completed/reaped
receipts: `/tmp/pipelang-result-propagation-20261112/REPORT.md`.

## Common result interface — locally qualified

The owner selected `IResult<E>` as a shared result boundary while retaining
`Result<T,E>` and its exact error object. This checkpoint precedes returning to
the callable/lambda foundation. Common status handling must not erase error
messages, codes, provenance or cancellation, borrow a dying result/model/scope,
or add a vtable to every typed Result.

The implementation uses a real owning type-erased interface object only at
conversion. Typed Result layout and constexpr behavior remain unchanged. It
retains and validates the full deeply owned typed value/error, charges existing
ownership limits, and shares immutable boxes on interface copies. Source exposes
status and Error, with payload access and `try` staying on the typed Result.
Qualification passes: 448 deterministic model/view cases, 49 composition-profile
assertions, nine normal and ASan/UBSan/LSan runtime fixtures, and installed SDK
service/direct-model execution on native, sanitizers and actual WASM. Tests retain
message/code/provenance, support heterogeneous payloads, preserve typed Value and
Result size/alignment/constexpr behavior, validate hidden successful payloads,
refuse empty/invalid/over-budget interfaces, and retain errors after model
destruction and service-scope closure.

The checked Source conversion owns deeply owned data and keeps the exact E;
source Value access, error-type mismatch, propagation through the payload-erased
interface, scoped-reference boxing, DTO fields and unqualified host serialization
are rejected. Existing JS/TS typed Result contracts remain unchanged. Native
Windows/macOS, browser-adapter qualification and full-suite/release proof remain
open. No claim of general user-declared value-interface inheritance is made.

Proof: `/tmp/pipelang-result-interface-20261112/REPORT.md`. The immutable native
and WASM SDK candidates rebuild the four compiler-linked tools and install current
runtime headers over admitted baselines; other tools retain their baseline
provenance. They are local composite candidates, not default installs or releases.

No callable/lambda, LINQ, ORM, consumer rewrite, pin/default SDK switch, deployment
or migration is implied by this interface checkpoint. Custom JS/TS Result interop
stays typed and unchanged; opaque interface serialization is explicitly refused.

## Context ownership and publication — locally qualified

2026-11-13: owner priority changed from further Billing porting to removing manual
presentation epochs/revisions/request matching. Source `ContextOwner` is an opaque,
private, scope-owned resource. Creation and replacement produce new immutable
invocation authority; invalidation, model shutdown and owner destruction revoke it.
A pure zero-argument getter exposes identity to the optional binding framework,
not a serializable value or numeric concurrency token. Source admission rejects
resource DTOs, containers, export parameters and worker capture.

The headless runtime captures authority at task admission and each owner resumption.
Owned-value and graph transactions validate the captured permit before publication.
Independent operation lanes select latest-wins or reject-busy admission, and retired
operations cannot acquire new authority from a later context. No context allocation
is added to ordinary scopes that never create one. Revocation observers, operation
receivers and captured target slots have explicit bounds and owner-thread rules.

The optional Qt framework binds fresh contextual models or shared controller state,
captures DTO targets before dispatch, resolves transport correlations within their
prototype and retires projections/model data when authority expires. Custom JS/TS
transport remains supported. UUID transport labels are opaque routing metadata;
application Source does not generate counters, compare epochs or propagate labels
through DTO methods. Projection failures require their own revocable publication
lifetime so a later successful projection starts with fresh private view state.

Database row versions, ticket versions and business idempotency remain application
contracts. Revoking UI publication does not undo an external effect or replace an
authoritative committed mutation receipt with cancellation. Source async effect
APIs must preserve that separation; these UI bindings do not claim durable-effect
rollback or database locking.

Local qualification passed through `workflows/context.check/{config,refresh,qt,wasm}.yml`
and the Cloud context workflows. Native-v3 qualifies the final core lifetime
implementation; native-v7, Qt-v7 and WASM-v7 qualify the current generated binding
layer and composite SDKs. Compiler/Source profiles, ordinary allocation regression,
normal and ASan/UBSan/LSan runtime fixtures and actual Source/WASM execution pass.
Cloud passes 27 normal and 27 bounded-profile sanitizer consumer suites, plus
597 actual Qt/WASM presentation and 108 Knowledge reader browser assertions.
The consumer sanitizer profile explicitly uses 32 MiB global/128 KiB per-thread
quarantine, with leak detection, assertions and containment unchanged; its shorter
temporal UAF history is recorded separately from the retained default-profile timeout.

Proof and exact candidate/component mapping:
`/tmp/view-context-ownership-20261113/REPORT.md`. All expected process trees are
reaped and input/artifact hashes verified. Candidates do not switch the default
SDK or consumer pins. Windows/macOS, hosted qualification and release remain open.
Root projection notifications are not lifetime loss; callers observe native
context retirement for private-state cleanup. The actual Cloud browser regression
verifies ordinary root updates retain accepted details and drafts while actual
revocation clears private state.

Permanent controller commands use explicit captured supervisor admission. Only a
current operation can capture its exact unbound root prototype. Admission is
checked immediately before the synchronous Source call; the root owns its state
transaction independently of its child view publication. A root can therefore
rotate child contexts and commit an access reset, with a domain operation outcome
and a cancelled old UI projection. Expired supervisors cannot enter a newer root.
Ordinary operation/DTO aliases retain strict captured-publication guards; converting
a supervisor to an ordinary operation alias does not retain supervisor semantics.
This is an owning-controller boundary, not an application epoch check or a database
transaction feature. Qt async methods remain outside this synchronous binding profile.
