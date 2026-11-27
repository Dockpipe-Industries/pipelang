# Lightweight typed DI — proposal for review

Status: source-declaration direction approved; checked plans, construction, owned
synchronous cleanup and scoped async service methods are locally qualified.
The [implementation record](dependency-injection-20261108.md) owns current evidence
and limits. This proposal follows inspection of the
ordinary compiler/parser/checker/generator, object/task runtimes, source module
system, SDK manifests and existing Cloud constructor injection. Independent
challenge/evidence: `/tmp/pipelang-di-audit-20261108/{report,proposal-challenge}.md`.

## Approach

Generate typed composition code at build time. Reuse ordinary constructors,
source interfaces, exact overload/access checks, module-qualified identities,
Result outcomes and the existing owner-confined object domain. Manual construction
remains unchanged. Domain/application classes receive ordinary constructor
parameters and never a universal provider.

The owner approved optional PipeLang source declarations, resembling a Startup
file. The initial implementation lowers them to checked composition IR without
runtime string lookup. A separate descriptor authoring format is not part of
this approved slice.

Illustrative source form:

```text
public composition KnowledgeApplication {
    scoped IKnowledgeStore = SqliteKnowledgeStore;
    transient KnowledgeService = KnowledgeService;
}
```

The type names are compiler-resolved references. The final source design must
explicitly cover imported registration modules, borrowed root and scoped inputs,
exported application entrypoints and owned cleanup. Existing language constructs
remain sufficient for application classes and constructors; a registration block
would be a deliberate small extension because ordinary source currently lacks
type values and a suitable typed registration API. Do not add reflection or broad
generic language features solely to imitate another framework.

The compiler owns symbol binding, accessible constructor/factory selection,
assignability, graph validation and generated direct construction/cache access.
Missing/unregistered services, ambiguous bindings/constructors, cycles and
transitive singleton-to-scoped dependencies are compilation errors, with binding
and constructor locations. No automatic unregistered construction or last-wins
registration. Private constructors remain private. The current public manifest
omits constructor visibility, so composition uses the checked AST rather than
inferring access from that manifest. Emit an inspectable graph containing types,
lifetimes, dependencies and construction order, without runtime configuration
values, credentials or instance state.

## Lifetimes and ownership

A singleton belongs to one composition owner, not a hidden process-global cache.
Scopes share that owner's existing object domain; do not cross-inject independent
model roots. Each scope has its own scoped instances. A transient is newly
constructed for each request and owned by that request's effective scope/root.
A transient injected into a singleton is root-owned; any transitive scoped
capture is rejected. Borrowed external inputs declare their lifetime and are
never disposed by the container. Owned cleanup is explicit.

Generate lazy typed accessors: dependencies initialize in a deterministic checked
order when requested, and caches publish only successfully constructed instances.
A failed attempt unwinds newly owned resources in reverse creation order, retaining
previous successful instances. Factories own partial external acquisitions until
successful handoff; object-graph rollback cannot undo external effects.

Add a small reusable runtime lifetime primitive with monotonic Open/Closing/Closed
state outside transactional graph rollback. Close rejects new resolutions, drains
registered scope-owned operations, then runs owned cleanup exactly once in reverse
creation order. Request cancellation must not skip cleanup. A busy scope must not
silently release live resources. Preserve primary failure/cancellation and report
safe cleanup failures separately. Repeated close is idempotent.

Scope-owned object/work metadata remains optional and owner-confined. Prefer
closed-scope guards on DI-owned handles where needed to make async ownership safe;
do not weaken existing cross-domain checks or claim arbitrary escaped references
are universally revoked. The runtime memory-safety/resource-handle contracts stay
authoritative. DI lifetimes do not grant tenant authorization.

## Deliberate initial boundaries

Construction/factories are synchronous; fallible factories use existing Result
outcomes. Injected services can have async methods, with scope-owned operations
registered and drained before teardown. Async factory initialization is a separate
extension, not accidentally implied by task support. No implicit cross-thread
source-object sharing or synchronization: use separate owner roots or explicitly
share-safe host services. Runtime configuration can select among precompiled,
validated alternatives. Unknown registrations and arbitrary dynamically loaded
implementations are refused; no plugin loader, collection/keyed binding framework
or runtime registration mutation is included initially.

## Challenge and proof

The useful abstraction is checked wiring plus ownership. Source composition
declarations avoid unrelated reflection/type-value features while keeping
application constructor injection idiomatic.
Singleton does not imply thread safety. Zero-allocation promises would be false:
current source objects already allocate, and scope bookkeeping has a cost.
Universal revocation of every ordinary escaped object is stronger than normal DI
and must not be added without a demonstrated need. Nontransactional lifecycle
state is essential: rolling back a closed flag after native cleanup could revive
a partially disposed scope.

Implement incrementally after approval: checked graph/typed generation first,
then lifecycle/cleanup and scoped task integration, then real Cloud-style domain/
application examples and native/WASM qualification. Do not claim scoped DI complete
before its lifecycle tests pass. Test invalid graphs, private/overloaded factories,
cross-module injection, all lifetime identities, nested scopes, failures,
cancellation, exactly-once cleanup, concurrency refusals and shutdown with pending
work. Benchmark equivalent manual and generated graphs under identical compiler
flags, ownership and cleanup guarantees: allocations/bytes, startup, first and
cached resolution, transient construction and teardown. Report regressions.

The user approved the source-declaration implementation after reviewing this
proposal. Phase 1 covers parsing, checked graph validation and inspectable plans;
construction and lifecycle qualification follow as separate checkpoints.
