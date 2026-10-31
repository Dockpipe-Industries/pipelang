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
`Optional<C>` or `Optional<I>` represents absence. There is no source null literal.

Assignment and parameter passing preserve object identity. `this` denotes the
current ordinary instance. Public/private checks apply to fields and instance
methods; a private constructor is callable only from its declaring class.
References can form cycles through optional fields after construction. Equality
compares identity across concrete and source-interface views.

`public source Interface I` is distinct from the existing host-service
`public Interface I`. Source implementations use `Class C : I, J` and must match
public parameter/return types and pure/async contracts exactly. Source interfaces
admit pure queries, synchronous effects and existing async `Task<Result<...>>`
methods. Host interfaces retain injected typed services and host-worker awaits;
source classes cannot implement that external boundary.

A Class implicitly upcasts to its base classes and implemented source interfaces.
Overloads prefer an exact argument match, then one unique upcast match; ties
refuse. Interface calls dispatch through the checked closed set of concrete
implementations. Generic local methods can close over reference types, while
generic instance-member dispatch remains unsupported.

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
effect, recursion and work-budget checks, including async methods.

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
slot and charges the maximum alternative work; recursion still refuses.

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
| Closed call depth / expanded work | 32 / 65,536 |
| Dispatch annotation edges / identity bytes | 65,536 / 4 MiB |
| Added implicit receiver AST nodes | 65,536 |

Checks precede mutation publication. Logical graph accounting excludes allocator
overhead and is distinct from process containment and existing per-container or
shared-cell caps. Dispatch analysis includes every possible implementation,
uses the maximum alternative target cost, and refuses recursive call graphs.
Cyclic storage does not imply recursive methods are supported.

V3 emits `pipelang.native-model-manifest.v2`, separating exported `classes`,
ordinary `objects`, source `sourceInterfaces` and existing host `interfaces`.
The current native view compiler and SDK view-binding helper explicitly refuse
this manifest until separately qualified. Existing v1/v2 manifest/header output
is preserved when the new capability is unused.

Frozen graph snapshots, default interface methods,
generic classes/interfaces, source downcasts and a global collector are not implemented. This capability does not
claim view integration, consumer deployment, cross-platform release or complete
C#/Java/.NET compatibility.
