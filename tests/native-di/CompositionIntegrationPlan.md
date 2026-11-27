# Native DI integration and equivalent-manual benchmark plan

Historical SDK5 checkpoint. The current factory, owned cleanup and scoped async
contracts and qualification are recorded in
[Source dependency composition](../../docs/concepts/dependency-injection.md) and
the [implementation record](../../docs/agents/tasks/dependency-injection-20261108.md).

The actual source fixture is under `tests/native-di/composition-fixture/src`. Installed SDK5 header-only and compiled native integration, compiled ASan/UBSan, two independent owner threads, and an equivalent guarded manual benchmark have passed. Exact inputs, raw results, receipts and remaining limits are recorded in `/tmp/pipelang-di-integration-20261108/REPORT.md` and `benchmark-summary.json`. This qualifies the bounded synchronous construction profile; external resource factories, async services and reverse cleanup remain separate checkpoints.

## Source fixture

The pure host `ConstructionPolicy` reads a fixed external fault mode. Out-of-band test tracing observes reads; it does not affect returned values. A source singleton `IIdentitySequence -> IdentitySequence` allocates IDs transactionally. The host may trigger boundary cancellation or adversarial callback reentry. Its labels and scope identities are synthetic test data. The ordinary source services use constructor dependencies across separate modules:

- `RootDependency`: transient recipe; when captured by `RootService`, its effective owner must be the root.
- `IRootService -> RootService`: singleton, retaining its dependency identity.
- `ScopeStamp`: explicitly scoped value snapshot, including synthetic context/tenant identifiers.
- `IRequestService -> RequestService`: scoped; exposes its own identity, scope stamp and root identity.
- `IUseCase -> UseCase`: transient; exposes identity and nested request/root observations.
- `IFaultyService -> FaultyService`: singleton with both a previously cached root and a fresh transient attempt dependency; fail-once/retry detects premature cache publication.

The fixture exports only synchronous methods returning owned primitive values. It does not expose raw ObjectRef values, a provider, borrowed host capabilities, async methods or dynamic service names.

`ConstructionPolicy.Read` uses Success or Failure in normal source execution and does not allocate IDs or acquire resources. Constructors cannot return Result. Distinct cancellation must be tested through the accepted resolver CancellationToken or preserved native OperationCancelled boundary; translating a Cancelled result into an Error message is not a cancellation implementation. Fallible source factory support is a separate checkpoint.

## Required integration assertions

1. Root construction and two scopes share one source ObjectDomain/TaskScope, rather than injecting independent native model owners.
2. Resolve the root singleton first from scope A. Closing A must not invalidate that singleton or its transient dependency. Scope B observes the same identities.
3. Two resolutions in one scope share IRequestService; different scopes use distinct instances and immutable scope stamps. Transients differ while observing the appropriate shared scoped/root dependencies.
4. The root has no resolver for an export whose graph requires scoped inputs. This is a typed API absence check, not a runtime string lookup failure.
5. Before failure, preserve a successful root instance. A failed FaultyService attempt publishes no cache. Retrying constructs again; a subsequent success is cached. Source graph live-count/logical-byte witnesses must reflect rollback; external probe observations are not described as reversible resources.
6. Boundary cancellation returns Cancelled, not Failure, and publishes no failed cache. Test already-cancelled admission and cancellation during a host callback if the accepted seam supports it.
7. Callback reentry fails safely without partial publication. Wrong-owner-thread admission/method invocation fails without source work. Keep final shared_ptr destruction on the owner thread unless a safe destruction contract is explicitly introduced.
8. Explicit close rejects future resolutions and guarded handle calls. Borrowed inputs remain externally owned. No arbitrary escaped-reference revocation, reverse service disposal or async draining claim follows unless those behaviors are implemented and exercised.

## Fair manual baseline

Manual composition must construct the same ordinary source types with the same dependencies, inputs, owner domain, graph transactions/savepoints, root-vs-scope allocation context, guards, cancellation, failed-attempt rollback, cache publication rule and lifetime retention. It must retain owned transients until scope close. Unsafe raw-new, independent native roots, private generated ordinals, omitted guards, early manual collection and direct borrowed-pointer calls are invalid baselines.

The compiler-owned manual bridge is `construction::Construct<SourceReference>(ObjectExecution&, args...)` with `construction::SourceMethods<SourceReference>::Method(ObjectExecution&, receiver, args...)`. The fixture baseline in `manual/` wires the same source types directly through these checked bridges, using the same ScopeExecution guards and staged no-throw cache publication. It does not wrap another source graph in a second generated container. Both bridges refuse native callback reentry; the final benchmark preflight exercises that boundary.

## Qualified measurement protocol

Use identical compiler/optimization/sanitizer profiles and one owner thread. Separate normal performance from sanitizer correctness. Predeclare a small deterministic operation sequence and fixed sample count; do not tune until desired timings appear.

Measure root startup, scope creation, first singleton/scoped resolution, cached resolution, bounded transient construction and explicit close. The accepted packet records wall time, native allocation count/requested bytes and source identity counts. Child CPU and source ObjectDomain live-cell/logical-byte metrics are not exposed by the public facade and are not claimed. Untimed correctness checks cover out-of-band policy-read attempts and method outcomes. Preallocate instrumentation outside timed regions, use the same observer in both lanes, and report observer costs. Cold initialization and warmed cached operations stay separate.

Transient samples use fresh scopes and an operation count below the same configured ledger/cell limits, with cleanup/close excluded or included identically. Count all owned objects and native bookkeeping; do not call the result allocation-free because the Source algorithm contains no explicit new. Publish machine-readable raw samples, source/tool hashes, exact workflow commands, failed/outlier samples and limits. Tiny synthetic wiring is not Cloud request throughput or a WASM result.

All builds/runs use declared contained workflows and unchanged caps. No overlapping heavy builds, services, publication or Git mutations are part of this task.

The final normal benchmark uses seven alternating sample pairs plus retained warmups. Cached resolution allocates zero in this fixture; guarded methods in both lanes request one 65536-byte runtime allocation per call. The raw packet preserves all samples and makes no broad throughput or tail-latency claim. Final SDK5 native lanes each pass 11134 integration assertions plus concurrency and namespace-shadow consumers. SDK4 header sanitizer evidence is retained as intermediate; SDK5 compiled sanitizer passes.
