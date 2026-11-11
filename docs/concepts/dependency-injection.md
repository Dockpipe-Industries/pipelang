# Source dependency composition

PipeLang source projects can declare an explicit composition root in `Startup.pipe`.
The compiler checks the constructor graph and emits typed construction code. Services
keep ordinary constructor parameters; they never receive a universal service provider.
Manual `new` remains available, and applications do not have to use composition.

The Cloud access example keeps its application and domain classes in separate files:

```pipelang
module DockPipe.Cloud.Access.Composition;
import DockPipe.Cloud.Access.Application;

public composition Startup {
    scoped input SiteAccessDirectory;
    scoped SiteAuthorization = SiteAuthorization;
    export SiteAuthorization;
}
```

`SiteAccessDirectory` is an existing host interface supplied by the composition owner.
`SiteAuthorization` is the existing source application class with an explicit directory
constructor parameter. Its authorization policy remains in source domain classes.
Each request scope receives its directory capability explicitly; composition neither
invents an authenticated identity nor selects tenant storage from a registration name.

## Registrations and checks

A binding has the form `singleton IPolicy = Policy;`, `scoped IRequest = Request;`
or `transient UseCase = UseCase;`. The service and implementation must be compatible
checked types. Each implementation must have exactly one accessible constructor.
All dependencies must be registered explicitly. Missing dependencies, conflicting
bindings, cycles and singleton capture of a scoped dependency fail at compile time.
A transient captured by a singleton belongs to the root's lifetime.

Fallible acquisition uses an explicit instance factory. Its receiver is another
registered source service, not a provider or an implicitly created native owner:

```pipelang
module Example.Composition;
import Example.Resources;

public composition Startup {
    singleton input ResourcePort;
    singleton ResourceFactory = ResourceFactory;
    scoped IRequestResource = factory ResourceFactory.Create;
    cleanup IResourceCleanup.Close;
    export IRequestResource;
}
```

`ResourceFactory.Create` is a public synchronous nongeneric source method returning
`Result<Resource,string>`. `Resource` is a compatible concrete source class. The
receiver and all ordinary method parameters must have explicit bindings. At most
one direct `CancellationToken` parameter receives the resolution token. A singleton
factory may consume scoped call arguments when the resulting resource is scoped;
its own constructor dependencies still obey singleton lifetime checks.

Cleanup is a selected nominal contract, declared in its own source file:

```pipelang
module Example.Resources;

public source Interface IResourceCleanup {
    Result<void,string> Close(CancellationToken cleanupToken);
}
```

`Resource` implements this interface as well as `IRequestResource`. The compiler
selects that exact checked method for applicable constructed services. There is no
special method-name convention: a differently named method with this signature can
be selected explicitly. More than one applicable selected cleanup contract fails.
Borrowed inputs are never adopted for cleanup.

The factory owns partial acquisition until it returns a successful resource. Use
source `try/finally` to release an acquired handle when object initialization or the
factory result fails. Put acquisition inside the protected region. Cleanup after
request cancellation can pass `CancellationToken.None` directly to a host release
method. This general empty-token expression does not permit storing tokens in
fields, returning tokens or introducing token locals. A finally block has a bounded
cleanup reserve; a failed release still needs application reconciliation.

Factories assemble and acquire; they cannot mutate existing source-object fields,
including through helpers or stored collection operations. Only a checked
constructor may initialize its own fields. Transitive nontransactional shared-handle
mutations, waits, releases, locks and parallel work are refused. These restrictions
prevent a singleton factory from retaining scoped parameters or changing published
shared state during a failed construction. They do not promise semantic undo of
arbitrary host effects. Ordinary source methods outside the factory closure keep
their established behavior.

The compiler also checks all closed-program source subclasses assignable to a
factory's declared success class. A subtype cannot introduce selected cleanup that
the declared class does not expose, or match multiple selected cleanup contracts.
Return a cleanup-declaring concrete class or cleanup-declaring base. A base's
selected nominal cleanup can dispatch to an override. Interface and virtual factory
selectors undergo the same effect checks across their checked implementations.

`singleton input Clock;` and `scoped input RequestContext;` declare externally
supplied capabilities or owned value snapshots. These are explicit typed inputs,
not service lookups. Primitive input types are keys too; use distinct configuration
records when two inputs would otherwise both be `string`.

`include Shared;` combines independently valid source compositions. Includes dedupe
a shared origin through diamonds, reject conflicts and do not implicitly export
another composition's entrypoints. Open partial registration fragments and overrides
are not supported. Names follow the source file's direct module imports.

Runtime configuration can supply different typed host inputs or select between
separately checked compositions. Runtime registration mutation, unknown implementation
types and dynamically loaded source modules are not supported by this profile.

## Selecting construction

Source compositions always produce an inspectable `CompositionPlan.json`. Explicitly
select the factory when generating a native consumer:

```cmake
pipelang_add_model(site_access
    PROJECT_ROOT "${CLOUD_SITE_SOURCE_ROOT}"
    SOURCE_ROOT access
    ENTRY composition/Startup.pipe
    NAMESPACE cloud_site_access
    COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
```

This works with `COMPILATION HEADER_ONLY` or `COMPILATION COMPILED`. The equivalent
SDK flag is `--composition-factory`; it implies a required checked plan. The SDK
tracks `CompositionFactory.hpp` in its artifact receipt. CMake exposes its path as
`PIPELANG_COMPOSITION_FACTORY`, regenerates missing outputs and invalidates generation
when factory selection changes. Ordinary generation does not retain a stale factory.

Include `CompositionFactory.hpp`. Its typed native facade lives beneath
`<generated namespace>::composition::<source module>::Startup`. `Startup::Create`
accepts singleton inputs and returns a root. `root->CreateScope` accepts scoped inputs.
`ResolveSiteAuthorization` exists on a scope; an export requiring scoped dependencies
has no root accessor. There is no string-based resolution API. Fallible facade
boundaries return the existing `Result<T,string>` with Success, Cancelled or Failure.

Native interop also exposes checked `construction::Construct<SourceReference>` and
`construction::SourceMethods<SourceReference>` bridges. They require an existing
`ObjectExecution`, preserve the same domain checks and use checked constructor/method
identities. They are not a provider intended for application/domain injection.

## Construction and cleanup ownership

A composition owner has one source object domain and one task scope. Child contexts
share them. Singleton caches belong to that root; scoped caches belong to one child;
transients are constructed afresh and retained by their effective owning context.
Construction stages its graph, caches and retained instances before publication.
For owned resources it reserves tracking before acquisition and adopts each fresh
successful result before the final construction cancellation check. Failure or
cancellation unwinds newly adopted resources in reverse creation order while their
source fields and dependencies remain readable. It publishes no new cache;
previously published dependencies are preserved. Graph rollback cannot undo an
external effect.

Roots, scopes and source handles are owner-thread confined. Use independent owners
on independent threads; singleton means one instance per root, not a process-wide
thread-safe instance. Cross-thread use fails. Final owner references must be released
on their owning thread. Synchronous callback reentry is rejected.

Explicit `Close()` moves the context to Closing and prevents new resolution and
guarded calls. A root closes its descendants as well. It cancels the selected task
subtree, drains its source continuations and finally blocks, then invokes selected
resource cleanup in reverse actual creation order across the closing subtree.
Sibling scopes remain usable when only one child closes. Closed handles can retain
references until released but cannot invoke their services.

`Close()` returns `Result<void,string>`. It can report pending work while the
context remains Closing. `CloseAsync()` returns admission
`Result<Task<Result<void,string>>,string>`; `Poll()` on the owner thread advances
continuations and deferred close, while `Pending()` reports tasks in the context's
subtree. Task cancellation is cooperative. Native host work must honor cancellation
or provide its own bounded deadline; it cannot be force-killed.

Prefer explicit close so the caller can inspect its result. Releasing the final
native facade/context reference also requests close, drains its tasks and attempts
cleanup on the owner thread. Final owner destruction drains before destroying the
object domain. This fallback can wait for a host deadline. It is not a cross-thread
destructor or an unconditional guarantee that an external resource was released.

`CleanupStatus()` reports phase and pending, attempted, succeeded, failed, cancelled
and unknown counts. An attempted callback is never automatically replayed after
Failure, Cancelled or an exception: its external effect may be unknown. Budget
exhaustion before a callback starts leaves that entry pending; a later bounded
session can attempt it. Cleanup uses an independent token and shared cleanup budget,
so a cancelled request does not itself cancel disposal. Failed-construction unwind
preserves the original construction outcome and records cleanup diagnostics
separately. A context can be Closed with failed/unknown cleanup; this still releases
ordinary cached references when its typed Close, status or Poll observes Closed,
and requires application reconciliation. A retained wrapper closed through an
ancestor or CloseAsync can retain ordinary references until its own next
status/Poll/Close observation or release; guarded calls already refuse the Closed
context.

`ObserveCleanup()` returns an owner-free `CleanupSession`. Its `status()` and
completion `task()` remain available after final facade/executor destruction. It
cannot resolve services, start work, close or pump the owner, and observing does not
initiate close. Keep owner-thread discipline when reading its retained state.

Ordinary constructor bindings permit pure construction and transitive pure owned
host reads. Effectful acquisition belongs in an explicit factory that owns partial
failure. Borrowed host ports remain externally owned and are not disposed by DI.
Factories and selected cleanup methods are synchronous. Async acquisition and
async disposal are not inferred from the scoped service-method support below.

## Scoped asynchronous methods

Exported methods can use synchronous owned-value signatures or the existing source
`async Task<Result<T,string>>` form. An async native handle method returns admission
`Result<Task<Result<T,string>>,string>`; the task completes with the direct source
Result. Each operation retains its receiver and owned arguments without retaining
an owning composition facade in its coroutine frame. Context task groups preserve
one shared object domain and task scope, bounded concurrency, callback reentry
checks and subtree cancellation. Existing source `await` and finally lowering runs
the method; DI introduces no alternative async implementation.

For synchronous methods, direct source `CancellationToken` parameters pass through
unchanged; the separate optional boundary token controls admission and budget. Async
methods combine their direct source token parameters with the boundary token before
task admission, matching ordinary model async wrappers. A pre-cancelled combined
token refuses admission before starting the method. A returned authoritative source
outcome remains authoritative after work completes, including Success before late
cancellation. Construction has its separate final cancellation check before cache
publication.

Async cancellation remains cooperative through the existing awaited operations
and explicit source token checks. DI does not add cancellation checks to every
expression after an await. A successful host result alone does not promise a
successful method: subsequent source logic can still return Failure or Cancelled.

Borrowed source-object inputs, reference-bearing facade returns/arguments, typed
nonstring Result errors and conflicting generated public names are refused by the
factory profile. Ordinary source construction and checked graph plans remain valid
independently of these emission limits. Factory diagnostics retain source path and
offset. Native runtime cell/byte and compiler resource budgets still apply.

Graphs using factories or cleanup use the inspectable
`pipelang.composition-graph.v2` recipe schema. Graphs without either preserve v1;
async service methods alone do not change graph metadata version. These schema and
runtime contracts are separate from installed-consumer qualification: implementation
does not itself establish a native, sanitizer, browser or release proof.

See the [implementation record](../agents/tasks/dependency-injection-20261108.md)
for exact candidate identities, tests, performance evidence and platform limits.

## Reproducing native consumer checks

On Linux, set `PIPELANG_DI_SDK` to an installed SDK prefix,
`PIPELANG_DI_SDK_SHA256` to its exact manifest digest, and `PIPELANG_DI_PROOF` to a
fresh absolute directory outside the checkout. Run the declared
`workflows/di.check/config.yml` workflow through the selected DockPipe executable.
It checks ordinary and resource-owning compositions in header, compiled and
ASan/UBSan profiles under the existing containment limits. Set
`PIPELANG_DI_BENCHMARK=1` to include matched normal-profile manual/generated
allocation and timing measurements. Sanitizer timings are not benchmarks.
