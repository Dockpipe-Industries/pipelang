# Native source objects and interfaces

The C++20 ordinary-model compiler supports reference-identity class instances and
explicit source interfaces through `pipelang.native-model.v3`. Existing v1/v2
programs retain their model roles and generated contracts; they refuse object
construction and source-interface declarations. This guide describes the
implemented headless contract, independently of UI or release qualification.

## A model with a private object graph

```pipelang
public source Interface ICounter {
    pure int Read();
}

public Class Counter : ICounter {
    private int count;

    public Counter(int initial) {
        count = initial;
    }

    public pure int Read() => count;
}

public Class App {
    private ICounter counter = new Counter(4);
    public pure int Read() => counter.Read();
}
```

Compile it with an explicit exported model list:

```json
{
  "protocol": "pipelang.native-model.v3",
  "namespace": "example",
  "models": ["App"],
  "source": "public source Interface ICounter { pure int Read(); } public Class Counter : ICounter { private int count; public Counter(int initial) { count = initial; } public pure int Read() => count; } public Class App { private ICounter counter = new Counter(4); public pure int Read() => counter.Read(); }"
}
```

The source form accepts exactly `protocol`, `source`, `models` and optional
`namespace`; it cannot contain module/import declarations. The closed module form
instead takes `protocol`, `modules: [{name,source}]`, `entry`, `models` and optional
`namespace`. Its sources declare matching module names and explicit imports as
specified by the [composition contract](pipelang-native-model-composition.md).
Bare model names resolve in the entry module; qualified authored names select a
specific module's Class. Both forms require 1..128 distinct Class exports and
reject mixed/unknown request fields.

Selected Classes are SDK model roots; other Classes are ordinary source objects.
Roots cannot be constructed by source `new` or implement source interfaces in
this capability. This explicit distinction permits library objects to have
reference fields while exported model snapshots remain owned values.

## Instances, identity and dispatch

`new C(args)` selects an exact constructor, with the existing unique source-
interface upcast fallback. Defaults/authored field initializers run before that
constructor's body. Overloads retain separate checked initializer trees. A
successful constructor must initialize every non-null object-reference field;
runtime validation also checks references nested in supported containers.
`C?` or `I?` represents absence; a known nullable target admits `null` or a present value.
The internal object-domain optional codec and checked Value access are unchanged.
See [nullable values](pipelang-nullable-values.md).

In v3, method and constructor parameters may share a field name, including an
inherited field. An unqualified name selects the lexical parameter; `this.field`
selects the instance field. Lexical values also take precedence over same-name
constants and type-qualified constant receivers. Duplicate parameters and the
existing local/catch/iteration shadowing restrictions still refuse. Earlier
non-object profiles retain their parameter/field refusal contract.

Field declaration initializers retain class-member bindings: a same-name
constructor parameter does not capture a field or constant reference. Distinct
constructor inputs remain available to initializers, preserving the existing
initialization contract. Constructor bodies and explicit base arguments prefer
lexical parameters. In a derived object, an initializer can read an accessible
inherited field after base construction. Private access and the prohibition on
instance access in base arguments remain unchanged.

An exported native model accepts explicit `this.field`, `this.Method(...)` and
`this.Method<T>(...)` as selection of its fixed owner, through the same checked
field/service/local-call paths as implicit selection. This syntax does not create
a first-class native-owner reference: bare `this`/`base` values remain refused.
The project source-object facet retains ordinary reference and virtual-dispatch
semantics. Both facets preserve pure, worker and constructor-only service/shared
identity rules; parameter shadowing does not grant access to the hidden field.

Assignment and parameter passing preserve object identity. `this` denotes the
current ordinary instance. Public/private checks apply to fields and instance
methods; a private constructor is callable only from its declaring class.
References can form cycles through optional fields after construction. Equality
compares identity across concrete and source-interface views.

`public source Interface I` is distinct from the existing host-service
`public Interface I`. Source implementations use `Class C : I, J` and must match
public parameter/return types and pure/async contracts exactly. Source interfaces
admit pure queries, synchronous effects and existing async `Task<Result<...>>`
methods. Host interfaces retain constructor-injected native services: synchronous
`Result<T,E>` methods with string, enum or owned-record errors and [pure owned-value returns](pipelang-pure-host-values.md)
execute on the owner thread, while asynchronous `Task<Result<T,string>>` methods
retain host-worker awaits. Source classes cannot
implement that external boundary. See the [synchronous host-call contract](../../src/native/model-compiler/README.md#synchronous-injected-host-calls)
for callback reentry, transaction and lifetime rules.

A Class implicitly upcasts to its base classes and implemented source interfaces.
Overloads prefer an exact argument match, then one unique upcast match; ties
refuse. Interface calls dispatch through the checked closed set of concrete
implementations. Generic local methods can close over reference types. Explicit generic member
calls on concrete class receivers reuse closed specialization and ordinary member
selection, including inherited methods and imported public methods. Generic
virtual methods and generic interface-member dispatch remain unsupported. See the
[closed generic contract](pipelang-native-model-composition.md#exact-overloads-and-explicit-generics).

## Composing source interfaces

A source interface can inherit multiple source interfaces:

```pipelang
public source Interface IReadable {
    pure int Read();
}

public source Interface IEditable : IReadable {
    void Set(int value);
}

public Class Counter : IEditable {
    private int count;
    public pure int Read() => count;
    public void Set(int value) { count = value; }
}
```

`IEditable` contains both methods. A `Counter` reference converts to either
interface, and an `IEditable` reference converts to `IReadable`, preserving
identity. Conversions work in checked arguments, returns and assignments.
Exact overload matches still win; multiple
eligible interface conversions remain ambiguous. A base interface cannot
implicitly convert to a derived interface.

Identical diamond contracts deduplicate by method name and parameter types,
independently of parameter names. Inherited return, pure and async contracts must
agree exactly; a matching authored redeclaration is allowed. Conflicting inherited
or redeclared signatures, duplicate authored methods, repeated direct bases,
cycles and unknown/non-source-interface bases refuse. Implementations supply all
inherited public methods. Calls through either interface retain closed dispatch,
effect, runtime call-depth and work-budget checks, including async methods.

A closed source interface may have no concrete implementation in the selected
program. Calls in imported or unused methods are still checked against its
declared signature, purity, async and ownership contract. Their checked dispatch
set is empty, rather than unresolved or open to later implementations. This is
not dead-code elimination: the method and interface remain in generated output.
The generated interface tag accepts no concrete object, and its zero-case
dispatcher refuses an invalid receiver. No default result or abstract body is
executed. An empty list or absent nullable interface value remains valid, while
a required nonnullable interface reference has no valid inhabitant in this
compilation. A different consumer that supplies a concrete implementation is
checked and emitted against its own complete closed program.

Inheritance supports at most 32 interfaces on a path and 256 effective methods
per interface, with at most 65,536 effective method copies and 4 MiB of qualified
slot identities across inheritance expansion. Copies also consume the existing
AST/type clone budget. Existing declaration, expression and dispatch bounds still
apply.
Manifest v2 exposes effective flattened interface methods and each concrete
object's transitive implemented-interface list, without adding hierarchy fields.
Host interfaces, default methods and generic interfaces remain
outside this contract.

## Class inheritance

V3 ordinary classes support one base class, followed by source interfaces:

```pipelang
public Class Counter {
    protected int count;
    protected Counter(int initial) { count = initial; }
    public virtual pure int Read() => count;
}

public Class OffsetCounter : Counter {
    private int offset = 10;
    public OffsetCounter(int initial) : base(initial) {}
    public override pure int Read() => base.Read() + offset;
}
```

An omitted base initializer selects the exact parameterless base constructor.
Explicit arguments evaluate once, left to right, before base initialization and
cannot access the instance. After argument evaluation, initialization proceeds
base-first: each class's field initializers, then its constructor body, then the
next derived class. Constructors are not inherited; private base constructors
refuse, while protected constructors permit chaining. Existing constructor
overload and unique-upcast selection rules also apply to base initializers.

The complete chain initializes one allocation and one physical state. Base and
interface views retain its identity, dynamic type and graph transaction; base
field updates clone the complete dynamic state rather than slicing it. A failure
anywhere in construction restores graph writes and allocations through the outer
constructor savepoint. Shared effects retain their existing nonrollback behavior.
Required references across the entire hierarchy are checked after construction.

Methods are nonvirtual unless declared `virtual`. A matching inherited slot must
use `override` and must preserve its return/parameter types, visibility and
pure/async contract exactly. Overrides remain virtual. Return-type covariance,
member hiding, abstract/sealed classes and generic virtual methods are not part
of this capability. New overloads may use inherited names with different exact
parameter signatures. Constructors, private methods and exported root methods
cannot be virtual. Exported model roots cannot inherit or serve as base classes.

Public members are generally accessible; private members belong to their declaring
class. Protected members are accessible in that class and its descendants,
including through another base reference in that lexical class scope. Private
storage remains present in derived objects and available to base methods.
`base.member` selects the nearest inherited member and bypasses virtual dispatch
for that call. `base` cannot escape as an ordinary value. Unqualified instance
calls and `this.member` retain virtual dispatch, including calls made by base
methods and constructors. During construction, derived fields may still have
their default values; accessing an uninitialized required reference fails rather
than manufacturing a usable reference.

Class-to-base conversions work in arguments, assignments, returns, conditional
arms and iteration using the existing checked reference conversion. Interfaces
implemented by a base class are inherited. Calls through classes and interfaces
select the most-derived override, including directly awaited async calls. The
closed effect/call analysis includes every possible override of the selected
slot. Execution charges the selected implementation to the inherited work
budget and source call ancestry, including recursive virtual calls.

Hierarchy paths contain at most 32 classes, and the sum of authored members on a
path cannot exceed 256. Hierarchy metadata is bounded by 65,536 ancestor entries
and 4 MiB of identities; existing dispatch, call-depth, work, graph and process
limits also apply. Manifest v2 keeps its existing schema and exposes effective
public fields/methods and inherited implemented interfaces. It does not add
runtime reflection or hierarchy transport fields. V1/v2 continue refusing the
new syntax and keep their existing emitted bytes.

## Owned values and owner-confined references

Each model owns a traced object domain. References and reference-bearing values
must stay in that domain and on its owner thread; foreign-owner or expired
handles fail validation. List/Array/Dictionary/Queue/Stack, Optional, Result and
pair values may contain source references in owner code. Copying a container
copies its membership/value structure while its references still identify the
same instances.

Records remain recursively owned and cannot contain object references. Object
references also cannot become HashSet/object keys, concurrent-cell payloads,
host-interface transport or parallel-worker captures. Workers cannot dereference
`this` or object fields, even inside a Lock. Owned values and explicit supported
shared handles copied into locals before parallel work retain their existing
worker rules. Pure queries may read their owner's graph, but cannot mutate it,
allocate objects, acquire synchronization or observe shared-cell state.

Exported public model fields must remain recursively owned. Their `snapshot()`
does not contain live object aliases. Private root fields may hold the graph;
ordinary objects may expose public reference fields. Native model methods may
accept/return owner-bound reference values, including supported containers and
Result payloads. Such handles retain identity, rather than freezing the graph;
closing their owner makes later access fail.

## Transactions and async lifetime

Root owned state and ordinary object updates share one transactional publication
boundary. Object writes use checked immutable candidate versions, so every alias
sees the current transaction's update. An escaping fault/cancellation discards
the current segment's ordinary updates. Source catch/recovery retains working
updates as before. Constructor savepoints additionally restore constructor writes
and allocations on failure, even when the caller catches that failure.

Atomic, concurrent-collection and synchronization effects remain shared effects:
ordinary rollback does not undo them. Private shared-field identity is fixed
after construction. Several shared operations do not become a transaction by
placing them in an ordinary object method.

Object/interface async calls are directly awaited and retain the receiver and
reference arguments in owned coroutine frames. They share the model's task scope,
transaction and evaluation budget, rather than creating a scheduler per object.
Root-local async children likewise share the parent graph transaction. Await
boundaries commit the current segment and refresh against current owner state
when entering/resuming children, including exceptional resume. Actual suspension
controls budget renewal; an immediately ready wait cannot renew an exhausted
budget. Existing cancellation, lock/finally restrictions and bounded teardown
continue to apply.

Owner-local tracing follows live handles, stored edges, active transaction
versions and savepoints. It reclaims unreachable cycles within that domain.
Task frames are destroyed before model state/services and the domain closes.
This is bounded owner-local lifetime management, not process-wide garbage
collection.

An unchanged read-only transaction can skip a redundant trace after pending
reachability changes have been collected. Reference release, graph publication,
overlay replacement/discard, savepoint removal, rollback and temporary pressure
roots conservatively schedule the next required sweep. Validation and native root
publication still run. Explicit collection and allocation-pressure checks retain
their tracing behavior. The first pending read can allocate collection scratch;
warmed reads with no intervening reachability change avoid that allocation.
Supported owner-thread reclamation boundaries are preserved; this does not add
cross-thread reference access or destructor reentry support.

This runtime change affects native ObjectDomain layout. Use one exact SDK and
rebuild generated/source-object consumers together; unchanged generated source
text does not make old compiled runtime bodies ABI-compatible.

## Bounds and transport

The existing source/token/expression, declaration, specialization, container and
task bounds still apply. Additional object bounds are:

| Resource | Bound |
| --- | --- |
| Live object cells per domain | 4,096 |
| Stored graph reference edges, including retained versions | 16,384 |
| Logical graph bytes, including retained versions/savepoints | 1 MiB |
| Active graph transactions per domain | 33 |
| Nested runtime construction savepoints | 64 |
| Entered source call depth / execution-segment work | 32 / 65,536 |
| Dispatch annotation edges / identity bytes | 65,536 / 4 MiB |
| Added implicit receiver AST nodes | 65,536 |

Checks precede mutation publication. Logical graph accounting excludes allocator
overhead and is distinct from process containment and existing per-container or
shared-cell caps. Dispatch analysis includes every possible implementation for
effect, ownership, cleanup and synchronization admission. It does not treat the
union of all receiver implementations as an actual execution path. Recursive
source calls are admitted in v3 and checked against the same runtime limits.
The compiler retains per-body structural work admission and existing graph,
specialization and metadata limits. Earlier non-object profiles retain their
static recursive-call refusal.

Each entered source method or constructor adds one activation to immutable
ancestry. Interfaces, virtual dispatch, generic specializations, qualified pure
helpers, base constructors and worker helpers use the same accounting. Generated
transport/dispatch wrappers do not add source activations. Entry beyond 32 fails
before the authored body or initializers run. Constructor factories check before
allocating the object cell. Async factories check before allocating the child
coroutine frame; the entered ancestry lives in that frame across suspension.
Siblings and independent top-level tasks do not change one another's depth.

Source statements, expressions and callable entry consume the existing shared
65,536-step execution-segment budget. Real host/semaphore suspension retains the
existing segment reset behavior; immediately completed local awaits do not
reset work. Resetting work never resets call ancestry. Parallel workers inherit
the parent's depth value and share the existing atomic aggregate work counters;
they do not acquire owner-confined object execution state. Resource exhaustion
cannot be caught as an authored `Error`. Awaiting a child preserves that terminal
identity, while the native top-level task still publishes a Failure result.

`finally` retains its independent cleanup work allowance and the same 32-depth
limit. Cleanup is attempted once; it cannot promise successful arbitrary work
after exhaustion. At maximum depth an inline cleanup action can run, but another
source helper cannot enter until ancestry unwinds. No extra depth reserve is
granted. A cleanup fault during unwinding preserves the primary failure; forced
frame destruction retains its existing nonthrowing boundary. Modern asynchronous
finally follows the [awaited cleanup contract](pipelang-synchronization.md#awaited-finally-in-the-modern-profile).

Synchronous generated calls borrow their parent's callback/cancellation leases.
Async frames retain the required leases. Existing owner-thread, synchronous-host
reentry and native lifetime contracts remain in force. Native numeric/observation
projections omit runtime call guards only when their complete acyclic closure
independently proves depth at most 32 and bounded work. A recursive method uses
the ordinary generated source implementation and has no unguarded numeric export.
Generated code and runtime headers must be rebuilt and qualified as one exact SDK
candidate; this contract makes no mixed-version ABI or performance-parity claim.

V3 emits `pipelang.native-model-manifest.v2`, separating exported `classes`,
ordinary `objects`, source `sourceInterfaces` and existing host `interfaces`.
The current native view compiler and SDK view-binding helper explicitly refuse
this manifest until separately qualified. Existing v1/v2 manifest/header output
is preserved when the new capability is unused.

Frozen graph snapshots, default interface methods,
generic classes/interfaces, source downcasts and a global collector are not implemented. This capability does not
claim view integration, consumer deployment, cross-platform release or complete
C#/Java/.NET compatibility.

Modern object-enabled asynchronous methods support awaited finally with owned
pending return/fault state, inherited cleanup cancellation and bounded owner
draining. See [awaited finally](pipelang-synchronization.md#awaited-finally-in-the-modern-profile)
for the cancellation, publication, depth/work and native await ownership contract.

## Static source methods

The explicit v3 source-object profile supports class methods declared with
`public/private/protected static [pure|async]`. Call them through the declaring
type, for example `Assert.Equal<int>(expected, actual, message)`. Explicit generic
arguments use the existing closed specialization and resource budgets. Local
static calls can omit the type qualification. Instance-qualified static calls
are refused; locals and fields continue to shadow type names.

Static methods have no receiver and cannot read or write implicit instance fields,
refer to `this`/`base`, or call implicit instance methods. They can act on explicitly
passed source objects under the usual pure/effect rules. Static constructors,
fields, virtual/override slots and interface contracts are not admitted. Inherited
static members retain their declaring visibility; method-slot hiding is refused
under the existing exact inheritance rules.

Static calls do not construct an object or run its field initializers/constructor.
They share the caller's owner, graph transaction, cancellation, evaluation budget
and source call ancestry. Async static calls must be directly awaited by an async
method and retain the normal segment/cleanup rules. Parallel workers do not admit
static calls in this profile. Header and compiled generation use the same checked
targets. Earlier native-model profiles do not admit static declarations.

Static members appear in manifest method entries with `static: true` and are excluded
from instance composition exports and native model instance actions. They are source
class APIs, not independently scheduled native model entrypoints.
