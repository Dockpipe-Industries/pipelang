# Direct native compilation

`pipelang build` produces machine-code executables by default. The build uses
the installed native object compiler and a checksum-pinned link SDK. Native
compile and link failures surface directly. Select `pipelang-modelc` for C++
model generation and its existing generated-header integration contracts.

The SDK's C++ model generator offers `COMPILATION COMPILED` for separately
compiled C++ translation units. The [older backend study](pipelang-native-backends.md)
records historical inputs from the retired frontend. Its proposed lowering is
not a capability of the current compiler.

## Verified scalar experiment

The experimental `pipelang-objectc` command uses ordinary project checking,
lowers a selected scalar method into typed blocks, and translates those
blocks to LLVM IR. LLVM emits ELF, COFF or Mach-O for the selected target. The
[native debug example](../../examples/native/README.md) exercises signed
comparison, Boolean locals, branching, assignment and integer returns.

The contained workflow traces the object compilation processes, repeats the
build from a second working directory, links and executes debug and release
objects, and checks failed compilation leaves existing output intact.
For both profiles it compares `Select.pipe` against the C++ backend and an
independent expectation over 1,045 inputs, including signed 64-bit limits,
values around the branch threshold, and a seeded random sample.
GDB stops at the authored `Select.pipe` line, reads locals and unwinds to the
source method. In a separate multi-file debug run, GDB and LLDB stopped at
the declaration of the imported `Remember<int>` method and showed its
incoming argument as `42`. The compiler leaves the machine prologue
unmapped and associates the declaration with an instruction after argument
initialization. A regression checks the argument at both the declaration
and the first body statement. The VS Code debug adapter can launch
the same executable and bind the breakpoint to the original example file.
Release objects retain source line tables without debug-profile
local-variable records.

The object driver accepts a single source file or an SDK source project.
Project mode uses the same source discovery, import closure, library
inventory, and digest verification as the C++ SDK path. It revalidates those
inputs before publishing the object. The checked frontend resolves each call
and the backend retains the defining fragment's path and line information.

Scalar lowering accepts a selected static pure method with integer ABI
parameters and return, including bounded, nonrecursive calls to other static
pure methods with the same ABI. Called methods can be defined in another
project fragment or a checksum-pinned source library. Bodies admit integer
and Boolean literals, comparisons, local storage, conditionals and returns.
Other signatures and operations fail during lowering. The object tool is an
opt-in build target. Selecting it for an SDK build includes the object
compiler in the installed inventory. On Linux and macOS the same selection
installs the prebuilt native application runtime archive. The
`PipeLangModel_OBJECTC` and `PipeLangModel_APPLICATION_RUNTIME_ARCHIVE`
CMake variables identify the installed artifacts when available.

The project proof steps into a pinned library on Linux and unwinds through
three authored files. It also verifies debug records for all six object
targets, deterministic Linux and Windows objects across separate trees,
and rejection of missing libraries, digest mismatches, and file changes
during emission. DockPipe linked and ran the same project on the physical
M6, where LLDB resolved the breakpoint in the library source.

The driver selects its host by default. An explicit `--target` selects the
destination independently of the host. The compiler validates the returned
object's format and architecture before publishing it. The source, typed IR and
scalar semantics are shared across targets.

## Bundled runtime experiment

The experimental `--runtime` mode lowers admitted synchronous and async
standalone entries to a versioned C ABI. A static entry can take an optional
leading `int` or `Array<string>` followed by up to two cancellation tokens.
An `int` entry parses one positional integer. An `Array<string>` entry
receives the process arguments in order, excluding the executable name.
Flag-like arguments reach the application as strings. Unicode process
arguments are converted to UTF-8 on Windows. A derived entry class can select
a public static method declared by its base.
When overloads are present, specify the parameter types. The debugger uses
the source file where the selected method is declared. An ordinary project
can call a public pure method by qualifying it with the name of a fieldless
exported model. The checked pure call and its local pure helpers execute
without constructing a receiver and share the normal source call-depth limit.
Synchronous entries can return `int`, `void`, or `Result<T,string>` where
`T` is `int` or `void`. Async entries return `Task<Result<T,string>>`.
The launcher prints successful integer results and leaves standard output
empty for successful void results. The bridge delegates collection operations
to the SDK.
Collection handles belong to the invocation. A failed
operation transfers control out of generated code and the runtime converts the
retained exception to a task result from its own coroutine frame.

Admitted async bodies emit continuation blocks for each local semaphore wait.
The native code yields a semaphore, timeout and continuation index. The SDK
`TaskScope` waits and invokes the selected continuation when work can resume.
Timed waits can use computed timeouts, and a suspended function retains
admitted locals, scalar objects and collection snapshots in its coroutine
frame. The SDK resets the evaluation budget only after a wait actually
suspends. A zero-time wait or an immediately available permit continues in
the same budget. Cancellation while waiting takes the SDK's cancelled outcome.

Semaphores can also pass by reference through synchronous and async calls
and live in invocation-owned receiver fields. The SDK owns the permit count
and suspension. The compiler retains a shared handle when a semaphore leaves
its local frame, while ordinary local waits retain their existing storage.
The shared handle arena is bounded to 4,096 entries per invocation, and
failed construction releases its new entries. The regression checks alias
identity, default fields, release counts, suspension, cancellation, and
4,200 local or failed-construction allocations.
Synchronous entries also use the SDK's blocking waits for timed and untimed
overloads, including explicit cancellation tokens. The frontend excludes
blocking waits from async, lock, and cleanup call closures.

Invocation-owned `Atomic<int>`, `Atomic<bool>`, `Atomic<float>`, and
`Atomic<float32>` cells use the SDK atomic operations. Aliases and async
arguments retain cell identity, including across suspension. Failed
construction releases cells it allocated. Updates to cells supplied by the
caller retain their effects. The conformance fixture checks overflow without
mutation, signed zero and NaN bit patterns in compare-exchange, cancellation,
and 4,200 failed constructions. A continuation can reuse a cell when all
uses of its local aliases stay within their declarations' lifetimes. The fixture
also checks 4,200 successive short-lived cells.

Native `lock` statements use the SDK's recursive mutex. A lock can be
forwarded by reference or stored in an invocation-owned field. Machine code
releases the guard on fallthrough, return, loop transfer, and exceptions.
The shared frontend rejects suspension while a source lock is held. The
regression exercises reentry through a synchronous call, a nested
`finally`, and cancellation after release.

Native `parallel foreach` emits a machine-code callback and uses the SDK's
bounded scheduler. Workers own their writable storage. The compiler transfers
owned sequence items and captured values, and the runtime shares the underlying
cells for captured atomics, locks, and semaphores. The regression checks records,
dictionary pairs, captured lists and text, reentrant locks, permit counts,
worker cleanup, and the lowest source-index failure. Workers call checked
stateless model and object helpers using receiver-free entry points. The
worker keeps its parent's source call depth. The same object helper can
also run normally with its receiver. Ordinary object state reaches workers
through explicit checked captures.

Invocation-owned concurrent queues, stacks, and dictionaries use the SDK's
shared cells. A worker can mutate a captured handle, and an async child can use
the same handle after suspension. Conditional dictionary updates compare the
typed values. Snapshots preserve owned values and iteration order across
concurrent mutations. Operations that return a removed or inserted value
prepare that return value before publishing the mutation. The queue, stack,
and dictionary fixtures passed in both profiles under Linux ARM64 QEMU and
Windows x86-64 Wine. On Linux x86-64, both profiles passed with
ThreadSanitizer instrumenting the generated PipeLang object and the SDK
bridge built from the same source revision. The race control was detected.
The system ThreadSanitizer reported an invalid unlock in plain C++
`std::recursive_timed_mutex::try_lock_for` on this GCC/glibc toolchain.
A runtime with `pthread_mutex_clocklock` interception passed recursive
locking, contention, and cancellation with the SDK lock. To run the
hosted graph worker, suspension, and cancellation fixture under that
runtime, set `PIPELANG_NATIVE_DEBUG_TSAN_ARCHIVE` to an absolute archive
path and pin its digest in `PIPELANG_NATIVE_DEBUG_TSAN_ARCHIVE_SHA256`.
The workflow checks the digest and runs a deliberate race control before
it tests both native profiles.

Hosted models and their constructor-owned children can retain atomic, lock,
semaphore, and concurrent collection fields for the lifetime of the binding.
Explicit initializers and default fields use the same SDK cells as
invocation-local handles. Worker captures and async calls preserve shared cell
identity. Changes to those cells survive invocation failure and cancellation.
The differential fixture tests four successive calls, mixed worker captures,
an actual semaphore wait, and persistent collection contents. Both native
profiles also passed the fixture under Linux ARM64 QEMU and Windows x86-64
Wine.
The public constructor arguments, invocation arguments, and results still
admit only their checked owned shapes and existing host bindings.

The multi-await fixture consumes an immediate permit, then suspends twice
while iterating a snapshot. It checks that an owned object, a result value, a
mutated parameter, and the loop state survive both resumes. It also tests
cancellation during the second suspension and during an indefinite wait.
Debug builds mirror live values into stack slots for debugger access and save
those slots before yielding. Release builds access the durable storage
directly. The standalone fixture ran on Linux x86-64, Windows x64, and the
physical M6. GDB and Windows LLDB stopped at the source line after the second
resume and read retained PipeLang locals.

The runtime path also admits closed scalar classes with integer and Boolean
fields. It emits their constructors and statically bound pure instance methods
as native functions. Function-local objects use bounded frame storage and can
only be used as direct receivers. A static launcher lowers its own statements
and can await closed static and instance methods with up to 32 integer or
cancellation-token arguments. Each child has a separate continuation frame
and shares the root's task cancellation and evaluation budget. The SDK holds source frames on an explicit
stack so immediately completed child calls do not grow the machine call stack.
The receiver remains owned by its original frame. A runtime fault rolls back
receiver changes in the current segment and preserves writes committed at
earlier await boundaries. The experimental v16 ABI requires a matching
compiled object and runtime archive. Later runtime revisions support host
and interface awaits.

The instance proof constructs state before the timed wait, reads that state
after resumption, creates independent local objects, and checks pending
cancellation. It ran on Linux x86-64, physical Windows x64, and the physical
M6. Linux GDB showed both the resumed async method and the called instance
method in their authored PipeLang file. The workflow also checks that object
aliases and non-scalar fields are rejected before publishing an object.

The nested-call fixture checks that authored failure and cancellation cross
multiple statically bound calls. It also checks recovery from a checked
arithmetic fault, including retained error bytes across a later suspension
and restored receiver state. An immediate-wait loop checks that fast awaits
cannot replenish the invocation's evaluation budget. The Linux workflow runs
that fixture with the SDK bridge instrumented by ASan and UBSan.
The same fixture ran on Windows x64 and the physical M6. Linux GDB and Windows
LLDB stopped in `AsyncCalls.pipe` after the nested wait and displayed retained
arguments. Suspended source callers remain in the SDK's explicit frame stack.
Showing that logical call stack in the debugger still requires debugger support.

Native async `try/finally` lowers cleanup and pending transfers into
continuation blocks. Return, normal fallthrough, break and continue pass
through each exited cleanup region. A fault or cancellation takes the
region's unwind continuation, including terminal budget exhaustion in a
child call. Cleanup uses the SDK task cancellation shield and the same
bounded cleanup allowance across suspension. A cleanup fault preserves an
existing primary fault and replaces an otherwise normal exit. The fixture
checks two real suspensions, nested cleanup, recoverable faults, terminal
faults and cancellation against independent expectations and the C++ backend.
The standalone build also passed on the Windows x64 worker and the physical
M6. GDB and Windows LLDB stopped inside the resumed cleanup and inspected its
retained locals. The macOS proof packaged a separate dSYM with the executable's
UUID, and LLDB resolved the same PipeLang source breakpoint from that bundle.
The executable and dSYM payload matched byte for byte across independent
build directories on the M6.
Live LLDB launch still stalled on the M6, where Developer Tools mode is disabled.
The native async path also supports `throw Error(message)`, `catch (Error)`
and lexical rethrow. An authored error or checked-operation failure can enter a
handler, and its message stays valid across a later await. Cancellation and
terminal evaluation exhaustion bypass error handlers and still run intervening
cleanup. An authored `Result.Failure` stays a return value. The standalone
fixture ran on Linux, Windows x64 and the M6. Linux GDB and Windows LLDB stopped
after the handler's await and read both its retained error and recovery result.
The M6 resolved the same breakpoint from its separate dSYM. Synchronous
fallible calls still require lowering.

The native launcher can supply independent body and cleanup tokens as explicit
source arguments. Closed static and instance async calls preserve those tokens
through suspension. `CancellationToken.None` and `IsCancellationRequested`
work in source, and a local semaphore wait combines only its selected token with
the current task cancellation. Entering `finally` shields task cancellation
while preserving the identity and state of explicit arguments. The standalone
launcher owns the token values for the invocation. Consumer host bindings for
constructing and invoking model instances remain separate work.
The native fixture passed on Linux x86-64, Windows x64 and the physical M6.
Linux debug and release builds agreed with the C++ backend and passed the
instrumented SDK tests. Linux GDB and Windows LLDB stopped on the cleanup
source line and showed distinct retained token arguments. The M6 resolved
the same breakpoint from a byte-reproducible dSYM.

Native async functions can construct, store, inspect, and return
`Result<T,string>` for admitted inline values. These include integers,
Booleans, text, errors, and closed records composed of admitted values.
Async child methods can also return `Result<void,string>` when they have no
success payload. `Task` error types follow the shared checker's string contract.
Success, failure, and cancellation carry distinct tags across the runtime
boundary. The compiler assigns each layout a frame extent and reserves the
storage needed for a child result before execution. Payloads survive returns
through awaited cleanup and are copied to the caller before the callee frame
is destroyed. UTF-8 error text retains its bytes, including empty payloads,
and `Value` still reports the SDK's checked failure on an unsuccessful result.
The standalone launcher takes an integer result. Inline records can carry
`List<T>`, arrays, optional values, and enums. The host result boundary
rejects invocation-owned interface and class references.

The result proof checks exact output bytes and exit status against independent
expectations on Linux, Windows x64, and the physical M6. The C++ reference
backend is exercised with the same source and expectations. GDB reads the
failed result's tag and error length at the original PipeLang line.

The structured-result fixture adds a nested record with text and Boolean
fields and a nested `Result`. It reads the payload after awaited cleanup and
again after another call reuses the caller's result storage. It also checks
failure, cancellation, checked access and byte-exact text. The standalone
fixture passed on Linux x86-64, Windows x64 and the M6. Linux debug and release
matched the C++ reference, and the fixture passed with the SDK bridge
instrumented by ASan and UBSan. GDB and Windows LLDB read nested record fields
after resumption. The M6 resolved the source breakpoint from a reproducible
dSYM, with live debugger launch still unverified.

Native lists use an invocation-owned backing store and represent each value as
an immutable prefix. Reading a field or copying a list preserves its contents.
Appending at the tip extends the backing store, while appending to an earlier
version forks its prefix. This allows returned records and nested lists to
retain their data after the producing coroutine exits. The SDK's element and
byte limits apply recursively. The invocation currently retains list history
until it exits and caps that storage at 32 MiB of accounted data.
The owned-list fixture passed on Linux x86-64, Windows x64 and the physical M6.
Linux debug and release matched the C++ reference, including the aggregate
element limit, and passed with the SDK bridge instrumented by ASan and UBSan.
GDB and Windows LLDB showed different retained lengths for the original and
appended lists. LLDB on the M6 resolved the source line from a reproducible dSYM.

Native enums use the ordinals assigned by the shared checker. Optional values
carry a presence tag and a payload. Checked `.Value` access reports the SDK
error when the payload is absent. Optional conversions also apply to awaited
results, and structural equality compares the contents of admitted records,
lists and optional values. The debug information describes enum members and
the optional payload fields at their original PipeLang source locations.
The fixture passed on Linux x86-64, Windows x64 and the physical M6. Linux
debug and release matched the C++ reference and passed SDK ASan and UBSan
checks. GDB and Windows LLDB stopped at the PipeLang line and displayed the
enum member, optional presence flag and record payload after resumption. On
the M6, LLDB resolved the breakpoint from a byte-reproducible dSYM.

Native bodies can construct and project `Result<T,E>` and `Result<void,E>`
values with admitted record and enum errors. The native layout carries the
actual error type and omits storage for a void success payload. Successful
results and parameterless cancellations use the error type's default,
including checked record field initializers. Equality and list quotas account
for the typed error and include a success payload only when present.
The fixture ran on Linux x86-64, Windows x64 and the physical M6. Linux debug
and release matched the C++ reference and passed the instrumented SDK tests.
GDB and Windows LLDB showed the retained error and its nested void result
after resumption. LLDB on the M6 resolved the source breakpoint from a
byte-reproducible dSYM.

Native conversion of a typed result to `IResult<E>` preserves its outcome,
typed error, and identity. Copies compare equal and independent conversions
get distinct identities. The representation carries the checked ownership
cost of the opaque result through collections and suspension. Hosted
receivers can return and retain these values across calls, including calls
on separate bindings, and rollback restores the original identity.
Construction validates non-nullable result fields before publishing a receiver.
The Linux x86-64 debug and release fixtures agreed with the C++ backend on
these cases, including an oversized result, cancellation, and uninitialized
construction. LLVM AddressSanitizer instrumentation in the generated
PipeLang objects passed for both profiles, and the heap-overflow control
failed as expected.

Within a runtime application, closed synchronous pure methods can accept and
return admitted typed values. They use native call frames and share the
invocation's evaluation budget and list ownership. The compiler bounds the
explicit storage in each synchronous call chain. A synchronous call does not
suspend or commit the caller's current segment. Faults enter the owning
`catch` or `finally`, and an unhandled fault returns control to the caller's
fault path. Source-level debuggers can show the synchronous callee and its
PipeLang caller as separate frames.
The Linux fixture matched the C++ reference in debug and release and passed
with the SDK bridge instrumented by ASan and UBSan. GDB and Windows LLDB read
the record and Boolean arguments at the source line and showed the PipeLang
caller in the backtrace. The executable ran on the M6, where LLDB resolved
the same source breakpoint from a reproducible dSYM. An additional native
project imports `PipeLang.Testing` and runs `Assert.True` and generic
`Assert.Equal<int>`, including their typed failure paths.

The native backend lowers `try` on `Result<T,E>` expressions and
`Result<void,E>` statements. It evaluates the operand once, projects the
success payload on the proven success branch, and rebuilds failure and
cancellation with the original typed error in the enclosing result type.
Propagation uses the same cleanup path as a source return. When an operand
suspends, native lowering retains already evaluated receivers and operands
in the continuation frame so evaluation order survives resumption. The
testing SDK project uses `try Assert.True` and `try Assert.Equal<int>` in a
synchronous method with a typed error return.
Linux debug and release matched the C++ reference and passed SDK sanitizer
checks. GDB and Windows LLDB stopped inside the authored `finally` after a
child returned failure. The physical M6 ran the standalone fixture and LLDB
resolved the same breakpoint from a byte-reproducible dSYM.

An explicitly selected `--host-symbol` emits a named descriptor for a public
async instance entry and its source constructor. The SDK's experimental
`HostedInstance` API constructs the receiver, copies invocation arguments and
starts the generated code in the host's `TaskScope`. The binding owns
source invocations on its creation thread. Calls from a different thread
fail before executing source. The task retains the
receiver until completion and a shared instance admits one invocation at a
time. A fault rolls back the active receiver segment. Earlier awaits and
ordinary result returns keep their committed writes. The SDK separates its
standalone launcher from the runtime so a host can link the same runtime archive
and supply its own entry point.
The hosted fixture passed on Linux x86-64, Windows x64 and the physical M6.
The Linux fixture linked as a position-independent executable and passed
SDK and host ASan and UBSan checks. GDB stopped in the original PipeLang
method after resumption. LLDB on the M6 resolved the hosted source breakpoint,
and the same SDK sources also linked and ran a standalone app on the M6.

The hosted boundary admits integers, Booleans, text, records of those values, and
bound host services as constructor parameters. The host uses
`HostedValue::fromRecord` for a nominal record and provides its fields by name.
Construction checks the record type, field types, UTF-8, and storage bounds.
The instance copies input text and stores supported record values inline, so the
host can release its constructor arguments as soon as binding returns.
Hosted entry arguments can be integers and cancellation tokens. Receiver fields
can hold the supported constructor values and ordinary closed source objects
constructed inside the hosted model's constructor call graph. The instance owns that graph,
with room for up to 128 objects in addition to its root receiver. A nested
constructor can keep a reference to an existing object in the same graph.
Construction checks initialized receiver references before publishing the instance.
Tasks pin the graph during suspension. Compiled receiver storage carries the
word count and type tag for the checked class. Hosted and invocation arenas
allocate the class's field words plus its header and enforce a cumulative
storage cap. A segment checkpoints each receiver's complete field span before
its first write. Nested synchronous calls share the active segment, including
when they write to different receivers. Unhandled faults roll back those
writes together. A fault caught within the same segment leaves its writes
available to the handler. Larger checkpoints reuse storage in the coroutine
frame after suspension.
Hosted and invocation constructors can update an existing receiver in their
graph. A failed nested constructor restores those changes to its savepoint.
Runtime
text assignments to a hosted receiver retain their own bytes. The receiver
checkpoint restores both the field and its storage owner when a segment
fails. An invocation keeps earlier text available to locals that borrowed
it before a replacement, including across awaits.

The constructor-record and nested-object fixture includes 24-word and 23-word
receivers. It passed on Linux x86-64 in debug and release, with ASan and UBSan
on the Linux SDK and host. The C++
reference agreed on shared object identity, isolation between instances,
rollback across receivers, and cleanup on an early void return. GDB traced
the PipeLang call stack from `Session.Run` through `Port.Adjust` to
`Client.Adjust`. The runtime symbols identify the receiver ABI revision,
so older objects require recompilation. Repeated
debug and release emissions matched byte-for-byte for Linux x86-64, Windows
x64, and macOS ARM64 objects. The Windows executable passed under Wine and
on the physical Windows x64 host. On the physical M6, the relocated SDK built
and ran both native profiles. LLDB stopped in `Port.Adjust` and `Client.Adjust`
at the original `.pipe` lines, showed `amount = 10` in both frames, and traced
the call back to `Session.Run`.
`startValue` projects admitted success values from `Task<Result<T,string>>`
and synchronous methods into immutable host-owned values before the invocation
releases its storage. A synchronous entry can return an ordinary value,
`void`, or a `Result`. The compiler adapts the return at the hosted boundary,
and the method runs in one receiver segment without a source-level async
wrapper.
A hosted entry with a typed error exports the complete source result as a
`HostedValue`. Success, failure, and cancellation keep their typed values.
Execution faults still use the host invocation's string error.
It preserves named record fields, enum names and ordinals, list contents,
optional presence, nested results, and UTF-8 text. The projection accounts for
copied storage and checks nesting depth. A projection failure rolls back the
current receiver segment. Hosts with an integer result can use `start` to
receive the scalar directly.

`startWithValues` also accepts checked owned arguments for hosted methods.
Records, collections, nullable values, and primitive values are packed into
the invocation's storage before any PipeLang code runs. The runtime checks
their nominal types and retains their storage across suspension. Integer
and cancellation arguments keep their existing API and descriptor format
when an entry uses only those types. The typed-input regression passes a
record with a nested list through a timed await, stores the list across
invocations, and rejects a record with the wrong nominal type.

Repeat `--host-method` to expose additional public methods of the same class
alongside the default `--entry`. `startMethod` selects one by name on the
same bound receiver. Synchronous and async calls share the receiver's
invocation lease. The regression reads and clears the same stored list
through different methods and rejects a competing method while the async
invocation is suspended.
It runs in both profiles under Linux ARM64 QEMU and Windows x64 Wine.
ARM64 LLDB stops at the original source after suspension and reads the
request's nested list count. Repeated objects match on all six targets.
A Linux x86-64 two-thread test also checks receiver exclusivity with
independent task scopes. The C++ SDK bridge passes ThreadSanitizer with
address randomization disabled for that test process. The emitted PipeLang
object uses its ordinary machine code in this check.

Synchronous host calls use the same checked layouts to pass owned values
in both directions. Direct integer, Boolean and string arguments still use
the corresponding C++ variant alternatives. Structured arguments are copied
into immutable `HostedValue` instances, which the host may retain. The
runtime checks the shape and nominal type of a returned value before making
it available to source code, and retains borrowed text and collection data
through the invocation. Service failure and cancellation keep their distinct
outcomes. Awaited host calls use the SDK's existing worker scheduler. The
owner thread copies arguments and commits the current receiver segment before
suspending. The worker receives the combined cancellation token and only
uses host-owned values. The owner thread checks and packs the result on
resumption. Linux fixtures exercise successful round trips, typed failures,
worker exceptions, and cancellation after the worker starts.
The hosted report fixture passed after task and receiver destruction on Linux
x86-64, Windows x64, and the physical M6. Linux debug and release passed,
including the SDK and host sanitizer checks. GDB inspected the source list and
captured error before projection, and LLDB resolved the source breakpoint on
the M6. A separate limit check confirmed that projection rejects a record with
repeated shared list prefixes once the copied storage exceeds its allowance.

`HostedInstance::bind` verifies the nominal constructor interface before it
publishes the receiver. A suspended invocation retains its service adapters.
Hosted field initializers, constructors, and their ordinary helper calls
consume one initialization budget. The binding overload accepts an
explicit allowance within the SDK's standard initialization limits.
Budget exhaustion, checked arithmetic failure, and revocation during a
synchronous host call prevent the receiver from being published. Effects
the host has already performed remain observable.
Generated synchronous calls identify an operation by its checked signature
and validate integer, Boolean, and UTF-8 string arguments and results across
the bridge. They use the SDK's synchronous host-call guard and exception
normalization. Fallible calls preserve `Result<T,string>` failure and
cancellation as distinct outcomes. A caller can bind a `PipeLang.Testing.Async.TestClock`
adapter and call `NowMilliseconds()` after resumption.
The fixture passed on Linux x86-64, Windows x64 and the physical M6. The
Linux native debug and release builds agreed with the C++ reference on
success, failure, cancellation and exception recovery. GDB and Windows
LLDB stopped on the authored clock call after resumption, and LLDB on the M6
resolved the source breakpoint.

Synchronous hosted invocations check caller cancellation at source evaluation
steps. A successful acquisition stored in a local or nested record remains
available to its `finally` block when the next step observes cancellation.
Cleanup keeps its independent cancellation and evaluation allowance, and
revoked invocation authority retains the `context.stale` outcome. The original
assignment-order fixture exercises both protected assignments and receiver
evaluation order with the C++ reference and both native profiles.

A hosted method table can expose up to 256 methods, matching the source
compiler’s class member limit. Direct bindings and checked composition
exports enforce the same bound before object publication.

A synchronous service with an owned typed error returns the complete
`Result<T,E>` as a `HostedValue`. PipeLang checks its nominal shape before
packing the tag, success payload, or typed error. An adapter-level failure
becomes an execution fault. An async service can carry a typed result inside
its task's string-error result. The fixture checks success, failure,
cancellation, transport failure, and rejection of a response with the wrong
nominal type across the two call paths.
The DockPipe target-report contract now lowers its typed
`FileStreams.CaptureSnapshot` call, retained `DriverSourcePins` receiver,
`PlanPolicy` set field, the `QualificationTarget` class value carried
inside a `Result`, and the fixed and dynamic driver-claim arrays. The
target-report suite and its row projection emit native objects and run
with the scoped file and JSON services on Linux x86-64. Debug and release
produce the same report as the C++ reference. The four cases include
one pass, one missing observation and two rejected observations.
Both backends reject the report when an evidence file's bytes no longer
match the receipt, before emitting report content.
The separate atomic run-report contract emits native objects and runs with
DockPipe's private staging, runner identity, and atomic replacement adapters
on Linux x86-64. The original `Begin`, `Append`, `Finish`, and `Close`
methods run as separate native calls on the same bound instance.
Both native profiles check qualified, incomplete, failed,
and interrupted reports, as well as preservation of the previous report
when runner identity changes or the recheck returns cancellation.
GDB stopped inside `QualificationRunReport.Seal` at the original PipeLang
line and inspected its required and passed counts. The debug and release
report contents match the original C++ contract after normalizing the
observed runner executable path and digest.
The same report and fresh-lane source closures emit debug and release
objects for all six targets. The report compilation trace contains only
`pipelang-objectc` and LLVM's `llc`. Execution with the scoped report and
lane adapters is verified on Linux x86-64.

The fresh and retained lane contract entries also compile and run directly
in debug and release on Linux x86-64. The fresh cases launch DockPipe's
synthetic protocol fixture as an actual scoped child, verify its request
and receipt, and reject passing coverage from a nonzero driver exit. The
retained cases revalidate the stored evidence and exercise metadata limits,
request tampering, and an incomplete process record without rewriting the
artifacts. Both native profiles and the original C++ probe produce matching
request, process, log, and evidence bytes for these fixtures.

A separate native composition also runs the production preflight, target
selection, and fresh and retained execution methods in both profiles.
It transfers the checked `PreparedQualificationRun` and
`QualificationLaneInput` records between native objects. The fresh invocation
observes the driver-start event after the request is committed and before the
child starts. The host calls the original `Open`, `Run`, and `Close` methods
on each bound model. Native event projection and journaling retain the
driver-start event. Retained cases leave the journal and prior artifacts
unchanged.

Native runtime bodies support concatenation, integer formatting, prefix
checks, byte queries, and UTF-8 length, slice, and search. The native and
C++ backends use the same SDK validation and offset checks. A native slice
borrows its source bytes, whose lifetime covers the invocation. Concatenation,
formatting, and text returned by a host callback retain their bytes in a
bounded invocation pool that accounts for string capacity and allocation
metadata. Exception text has its own allowance. The regression fixture
passes text through a child call and an await, rejects offsets in the middle
of a UTF-8 scalar, tests the one-MiB per-string limit, and retains an earlier
slice while generating 8,192 unique row labels.

Direct native runtime bodies lower `while`, `for`, and `foreach` with
lexical scopes and explicit `break` and `continue` targets. Iterating an
admitted list captures its prefix, and iterating an array takes a snapshot.
Writes to the original collection leave the current iteration's elements and
length intact. Loop bodies use
the SDK evaluation budget and cancellation checks. Signed addition,
subtraction, multiplication, division, and remainder use the same checked
SDK operations as the C++ backend. The workflow compares native debug and
release outcomes with the C++ reference, including overflow, divide-by-zero,
and an infinite loop stopped by the evaluation limit. The loop fixture
passed on Windows x64 and the physical M6. GDB stopped at the authored
nested loop and read both iteration variables. A differential test checks the
minimum successful evaluation budget for twenty-eight hosted source paths
against the C++ SDK. The cases cover calls, loops, assignments, cleanup,
immediate child completion, genuine nested suspension, source interface
iteration and identity, snapshots of arrays, lists, sets, queues, stacks and
dictionaries, and optional element conversions for dictionaries and byte
buffers. A separate constructor regression compares outcomes and host callback
sequences at every initialization budget through success for twenty-two source
paths. Precise budget parity across other call graphs still needs qualification.

Windows runtime objects use the LLVM-MinGW target triple selected by the
bundled SDK. This lets generated code with large stack frames use the stack
probe supplied by that SDK. The workflow links with relative object and SDK
paths and a fixed PDB source root. Independent staging directories produced
byte-identical executables and PDBs for the synchronous call fixture. The
paired Windows host ran that executable and stopped at the authored PipeLang
line using the generated PDB.

The proof builds a static runtime archive during SDK staging. Application
compilation and linking consume the staged inputs. The
[suspending collections example](../../examples/native/SuspendingCollections.pipe)
passed from a standalone Linux x86-64 ELF and on the native Windows x64
worker, with only Windows system DLL imports in the PE. The macOS ARM64
application linked against the native SDK and passed suspension, resumption
and cancellation on the physical M6. Its Mach-O imports were checked against
the macOS system library paths. The run used DockPipe's existing scoped Files
and Terminal grants. The runtime ABI and packaged application path remain
experimental.

On physical Windows x64, the native launcher built a two-file project and
the bundled app ran with only Windows system DLL dependencies. An injected
LLVM failure preserved the executable and its PDB. LLDB loaded that exact
PDB, stopped at `Entry.pipe:6`, and read `scenario = 1`. It then stopped
after the await at `SuspendingCollections.pipe:12`, read `index = 1` and
`acquired = false`, unwound through the task scheduler, stepped to line 13,
and continued to a successful exit. CodeView uses a stable source root that
the debugger maps to the original files. The PE records the PDB basename.
An earlier LLDB launch stalled during macOS debugger authorization. The
current physical M6 run used the existing DockPipe Review terminal to verify
the staged archive and link the native object from the declared workflow.
LLDB stopped on line 12, displayed `index = 1` and `acquired = false`,
stepped to line 13, and continued to exit 0.

## Current pipeline audit

| Boundary | Current owner and finding | Required change |
| --- | --- | --- |
| Source and semantic analysis | The module linker and checker resolve imports, closed calls and concrete types. Checked nodes retain expression types, resolution, source offsets and selected call identities. The editor projects this information without invoking the emitter. | Preserve the checked frontend and retain original source and scope identities in lowered instructions. Profile the editor's separate source indexing before consolidating any of its work. |
| Semantic lowering | The C++ generator currently implements effect-sensitive lowering as it emits source. Among other operations it inserts checked arithmetic calls, sequences operand evaluation, scopes ownership transfers, and routes transfers through asynchronous cleanup. | Move the meaning of these operations into shared, verifiable control flow and effect contracts. Encoding them a second time in a native emitter risks observable differences. |
| Runtime | The SDK models values, object graphs, cancellation, task scheduling and cleanup through C++ types, templates and coroutine support. Those are implementation mechanisms as well as compatibility interfaces. | Specify a versioned native runtime boundary and ownership rules. Reuse prebuilt implementations behind that boundary when they preserve semantics. Keep generated-code layouts and the debugger representation verifiable independently of the reference backend. |
| Artifacts and build | The model compiler supplies C++ source and manifests. SDK tools admit the compiler and publish generated artifacts, then CMake consumes those sources. Even the SDK's compiled-project mode depends on C++ compilation. | Carry typed native objects and executable metadata through the SDK, retaining its tool identity, dependency tracking, bounded output and publish-on-success rules. Audit the final driver and distribution for any transitive C++ compiler invocation. |
| Source debugging | The editor can resolve names, call targets and byte offsets. Existing program debug information identifies generated C++ locations. | Preserve paths, lexical scopes, inline sites and variable locations from checked PipeLang through optimization and machine-code emission. Qualify the debugger and runtime unwind behavior separately from the language server. |

## Compiler boundaries

1. The frontend owns parsing, source and library resolution, type and effect
   checking, generic specialization, and diagnostics. C++ emission and editor
   semantic analysis use the same checked-program entry point.
2. A target-independent typed IR must make control flow, value and storage
   types, checked operations, ownership transfers, borrows, cleanup, and
   asynchronous suspension and cancellation explicit. Instructions and values
   retain authored source locations and stable identities after lowering.
   The checked syntax tree provides the input to this stage.
3. Validation and optimization operate on that shared IR. Verify before and
   after transformations. Constant folding, dead-code elimination, inlining,
   and escape analysis must preserve the effects and failure ordering of the
   source program. Removal of bounds, null, ownership or overflow checks
   requires a proof in the IR. Inspect allocations, copies, boxing, reference
   counting and runtime calls on both optimized and unoptimized output.
4. Target-specific lowering owns the calling convention, object layout at the
   ABI boundary, stack alignment, register allocation, unwind information,
   relocations and instruction selection. Target-dependent choices must not
   change PipeLang value semantics. Evaluate a distributable code generation
   and linker toolchain against the supported platform matrix before pinning
   it in the SDK. The eventual consumer toolchain may use prebuilt runtime
   libraries and shipped code generation tools.
5. The driver owns target admission, artifact emission and linking. It reports
   target, stage, source location when available, and an actionable diagnostic
   on failure. It fails the build without retrying through C++. Publish output
   only after all stages succeed. Existing source and project configurations
   must remain valid. Explicit C++ generation stays available for integration
   callers that depend on its headers and symbols.

Separate debug and release profiles. Debug artifacts must have PipeLang file
and line mappings, named frames, local-variable locations and usable unwind
information. Release optimizations must preserve the source-level information
needed for stack traces. Qualify actual source breakpoints, stepping, locals,
and exception or failure stack traces with an editor client. The LSP retains
the shared frontend for editor queries, which need no code generation.

For a standalone executable, pass `--source-trace` to the native build and
again when running the executable to print the authored stack for an
unhandled runtime fault. When the entry takes `Array<string>`, a build
with source tracing prints unhandled fault stacks automatically. The trace
follows a fault propagated through an
await, includes synchronous callers, and retains distinct provenance for
failures with the same message. Normal invocation keeps the existing error
output and exit status. The regression compares exact debug and release
traces on Linux x86-64, Linux ARM64 under QEMU, and Windows x64 under Wine.

## Consumer examples

Consumer examples must use PipeLang for authored application code and the
standard native build and debug commands. Keep one explicitly named C++
interoperability example to demonstrate the reference backend and generated
headers. C++ ABI and differential-testing fixtures belong with verification.
The current [settings showcase](../../examples/settings/README.md) uses a
handwritten C++ host as well as a PipeLang model. Its storage, lifecycle and
inspection behavior must be preserved when replacing the host for the
normal example.

## Target matrix

These targets are planned. Production qualification is required independently
for each row. Existing SDK host qualification covers the current toolchain,
whose model code still uses the C++ backend.

| Target | Object / executable | Debug information | Direct native status |
| --- | --- | --- | --- |
| Linux x86-64 | ELF / ELF | DWARF | Native scalar execution and source debugging verified |
| Linux ARM64 | ELF / ELF | DWARF | Bundled runtime app ran in both profiles and hit a live PipeLang breakpoint under QEMU user mode |
| Windows x86-64 | COFF / PE | CodeView and PDB | Native launcher build, standalone execution, and live debugging across two source files verified on a physical Dell XPS 15 9520 |
| Windows ARM64 | COFF / PE | CodeView and PDB | Bundled runtime executable and PDB linked, source breakpoint resolved |
| macOS x86-64 | Mach-O / Mach-O | DWARF and dSYM | Debug and release executable and dSYM publication verified with a synthetic cross-link SDK |
| macOS ARM64 | Mach-O / Mach-O | DWARF and dSYM | AppleClang SDK build, archive relocation, standalone execution, and live debugging across two source files verified on the physical M6 |

The workflow verified repeatable object bytes, architecture, symbol and debug
records on all six rows. Both Windows PDBs include the source filename and
locals. The current Windows and M6 fixtures hit the authored breakpoint,
read both locals, stepped to the next source line, and exited successfully.
The remaining platform and production acceptance checks still apply.

Keep the IR independent of these formats and calling conventions so future
embedded and WebAssembly targets can lower the same language semantics. Target
selection must report unsupported targets and capabilities explicitly.

The `--executable` driver selects x86-64 or ARM64 for Linux, Windows, and macOS
and links using a checksum-pinned SDK for that architecture. Linux builds
produce a static ELF. The Linux ARM64 proof runs async suspension, resumption,
bounds and cancellation under QEMU with an empty environment, compares repeat
builds byte-for-byte, and checks that the x64 link SDK is rejected before
publication. LLDB hits the authored breakpoint and reads a local in the
running ARM64 process. The opt-in LLVM-MinGW CMake build includes the native
Windows runtime archive for x64 and ARM64. The staged link SDK pairs that
archive with its matching startup files and static libraries. Windows builds
emit a PE and a PDB from
the same link. Debug and release PDBs carry PipeLang source lines, and the
debug profile includes local variables. The proof compares repeated PE
and PDB outputs across staging directories and checks that a failed link
preserves the last published pair. The Windows ARM64 application proof
checks that the x64 link SDK is rejected and the ARM64 image imports only
Windows system libraries. Execution on Windows ARM64 hardware remains pending.
macOS builds use the selected local Apple system SDK and emit a matching dSYM
alongside the app. On both architectures, the Linux cross-link fixture verifies
matching Mach-O and dSYM UUIDs, source line tables, reproducible output, and
preservation of the previous pair when dSYM generation fails. The installed
launcher also produced matching UUIDs and preserved the previous pair with
the same synthetic SDKs and an empty compiler search path. On the physical M6,
the normal AppleClang toolchain built and installed the ARM64 SDK. The installed
manifest and archive were verified before and after extraction into a relocated
SDK. The relocated launcher compiled a two-file `.pipe` project using the local
Apple system SDK and checksum-verified LLVM 22.1.8 `llc`. The standalone app
returned 42 after suspension and reported the expected bounds error for the
failing input.
An injected code-generator failure preserved the executable and matching dSYM.
LLDB stopped at `Entry.pipe:6` and `SuspendingCollections.pipe:12`, showed
`scenario = 1`, `index = 1`, and `acquired = false`, then stepped to line 13
and exited successfully. A second two-file project on the M6 imported the
packaged `PipeLang.Testing` source library by its manifest digest. Both build
profiles returned 42 after suspension, reported the expected typed assertion
failure for the failing input, and produced matching executable and dSYM UUIDs.
The M6 steps used the repository's CMake build and SDK packaging tools in bounded
Review terminal sessions. The full declared macOS build and release workflows
have additional assertions beyond these sessions.

## Native executables in CMake

The installed `pipelang build` command forwards source, target, and link
options to the SDK's native compiler. `--backend native` remains an explicit
selection of the same path. The build requires the
target's link SDK and its manifest digest, just like `pipelang-objectc
--executable`. The legacy `pipelang compile` command continues to
produce workflow artifacts. `pipelang-modelc` selects the C++ model
generator.

The SDK exposes a `NativeExecutable` component in its CMake package. Its build
targets use the same native compiler and pinned link SDK as the command
line. A source-only CMake project can use `LANGUAGES NONE`.

```cmake
cmake_minimum_required(VERSION 3.21.1)
project(Standalone LANGUAGES NONE)
find_package(PipeLangModel CONFIG REQUIRED COMPONENTS NativeExecutable)

pipelang_add_native_executable(standalone
    SOURCE Standalone.pipe
    ENTRY Standalone.App.Run
    TARGET linux-x64
    PROFILE debug
    OUTPUT bin/standalone
    LLC "${PIPELANG_LLC}"
    LINKER "${PIPELANG_LINKER}"
    LINK_SDK "${PIPELANG_LINK_SDK}"
    LINK_SDK_SHA256 "${PIPELANG_LINK_SDK_SHA256}")
```

Set the LLVM and SDK variables to absolute paths and pin the
SDK's `link-sdk.json` SHA-256 with `PIPELANG_LINK_SDK_SHA256`.
The rule uses `pipelang-objectc` in the link SDK when present. A link SDK
containing only runtime inputs uses the compiler from the installed PipeLangModel
package. Set `OBJECT_COMPILER` to select a specific compiler explicitly.
For a multi-file project, supply `PROJECT_ROOT` and `PROJECT_ENTRY`.
`SOURCE_ROOT` defaults to `src`. Use
`SOURCE_LIBRARIES` for a pinned SDK reference file. The same tool
that admits project sources supplies CMake's dependency list. For a model
written against the modular source-set contract, use `SOURCE_SET` in
CMake or `--source-set` on the command line. The descriptor selects the
same ordinary or object model profile as the C++ generator, and each
authored module keeps its relative source path in debug information. For a
single-file ordinary model, use `MODEL` with `pipelang_add_native_object` or
`--model` with `--host-symbol`. That input uses the same single-file
language profile as the C++ model generator. To select the single-file object
model profile, add `MODEL_EXPORTS` to the CMake rule or repeat `--model-export`
with the selected class names on the object compiler command line. The
entry must belong to one of those classes, and it retains the original
source path and lines for debugging. Set `PROJECT_TOOL` to override the
installed `pipelang-sdk-tool` path.
Windows builds also publish a PDB. macOS builds publish a dSYM and
accept `MACOS_SYSROOT`, `MACOS_SDK_VERSION`, and `DSYMUTIL`.
On the physical M6, the relocated SDK built a CMake project with a pinned
`PipeLang.Testing` import. An unchanged build skipped compilation. A change to
the imported project module rebuilt the app. An invalid source edit and an LLVM
tool change followed by a failed build both kept the last executable and its
matching dSYM. Removing the generated DWARF file triggered a rebuild.
Before publishing a replacement, the native linker checks the existing dSYM
against the newly generated bundle. Unexpected members stop publication and
leave the existing executable and symbols intact.

The default hosted model profile checks the SDK’s portable
binary32 environment at binding and invocation entry and after value-returning
synchronous host calls. It reports an incompatible rounding or subnormal mode
before continuing source evaluation. Environment observation retains its
checked exemption, and an explicitly selected active-host profile uses the
caller’s floating-point controls.

Before polling a pending portable binary32 task, the SDK checks the owner’s
floating-point controls again. If the check fails, pending tasks and wait
registrations remain intact. Restore the portable controls and retry the poll.
Scope shutdown can still drain pending tasks for cleanup.

On Linux x64, `NATIVE_NUMERIC_ENVIRONMENT active-host` selects active-host
floating behavior for native CMake executables and hosted objects. The
command-line spelling is `--native-numeric-environment active-host`.
For a direct numeric C ABI object, use `NUMERIC_SYMBOL` with that environment
in `pipelang_add_native_object`, or pass `--numeric-symbol` to
`pipelang-objectc`. Select a public pure method from a fieldless model.
Match the C declaration to the source signature using `int64_t`,
`uint32_t`, `double`, and `float` for PipeLang `int`, `uint32`,
`float`, and `float32`, respectively.

A numeric record parameter takes a pointer to a caller-owned C struct.
A numeric record result appends a pointer to writable caller-owned storage and
returns `void`. Declare the fields in the PipeLang declaration's order using
the same C type mapping. A nested record is a nested C struct by value, with
ordinary C alignment and padding. Pass valid, naturally aligned pointers.
Input and output may share storage when their layouts match.

Use the `NativeObject` component and `pipelang_add_native_object` to
compile a PipeLang source file or project for a host application. A
compile-only CMake project can use `LANGUAGES NONE`. The rule checks
the pinned link SDK before invoking the object compiler and tracks the
source and library dependencies discovered by the SDK. Supply
`HOST_SYMBOL` and any additional `HOST_METHODS` for an object that
exports a hosted binding. Read the generated path from the target's
`PIPELANG_NATIVE_OBJECT` property when linking that object with the
application and its matching PipeLang runtime archive.

In the Linux x64 debug profile, GDB steps from a hosted method into
the original declaration of a synchronous helper. The workflow checks the
source argument and receiver identity at that stop, and checks the argument
again at a breakpoint in the helper body.

An isolated Linux VS Code session using LLDB DAP also hit the original
`Entry.pipe` breakpoint, stepped into `Numbers.pipe`, and inspected the
arguments in both files. A separate session stopped at an authored
division by zero and displayed the PipeLang fault stack through the
suspended caller. The extension's [debugging setup](../../tooling/vscode-extensions/pipelang-language-support/README.md#debug-native-executables)
shows how to map the original source directory.

The installed launcher also linked a standalone Linux app from two source
files and a checksum-pinned copy of PipeLang.Testing. Both profiles
preserved typed assertion messages. GDB displayed the original app and SDK
source lines with their source paths mapped, including a project directory
containing spaces. A changed SDK descriptor produced a digest error and left
the prior executable intact.

DockPipe 0.6.41 also ran the archived SDK from a private host workflow.
The workflow compiled the multi-file app, ran it with the bundled testing
library, and observed its typed failure. A forced LLVM failure stopped the
workflow before the run step and preserved the last published executable.

## Acceptance and benchmark evidence

Hosted integer calls project their return values through the same
invocation coroutine as other hosted results. Removing the extra
coroutine reduced allocations from 17,600 to 15,200 across 800
receiver-graph reads. In 41 paired Linux x86-64 samples, median graph
time was 1.106 ms against 1.209 ms for the previous runtime and
0.175 ms for the C++ reference. The separate snapshot median was
0.189 ms against 0.190 ms for the previous runtime.

- Run the complete language and standard-library suites through both backends
  on the same checked sources. Compare stdout and stderr where specified,
  return values, exit status, diagnostics, failure and cancellation identity,
  effect order, cleanup, ownership, and safety outcomes. Include independent
  expected results, ABI interoperation, seeded randomized programs and
  regressions. Neither backend is the sole correctness oracle.
- Test actual linked executables on every target admitted above. Check symbols,
  ABI calls, unwind data and resource ownership. Exercise invalid memory
  accesses and concurrency under applicable analysis tools and give each tool
  a failing control so an ineffective instrumented build cannot pass. Run the
  complete DockPipe qualification harness using native artifacts.
- Require deterministic artifacts for equivalent inputs with pinned identities.
  Verify repeat builds from different directories, with debug path remapping
  and timestamp policy recorded. Test normal CLI, SDK, editor and DockPipe
  entry points, including an environment with no C++ compiler and deliberate
  backend failures that cannot leave a published artifact.
- Capture the C++ baseline before optimization. Compare cold compiler
  processes, warm and incremental builds, peak compiler memory, executable
  size including its runtime dependencies, runtime throughput and latency,
  allocation counts and bytes, process startup, CPU time and memory. Include
  small, allocation-heavy, concurrent and real SDK workloads. Separate
  frontend, backend and linker time and report end-to-end costs. Record host,
  target, fixture and tool hashes, flags, cache policy, sample counts,
  distributions, resource caps, and failed runs. Use the same workload and
  resource limits for both backends.

Switch the production default only when the advertised language and integration
contracts, platform qualification, differential and independent conformance,
debugging and benchmark evidence are complete. The results below cover the
specified fixtures and environments.

The invocation runtime admits closed class inheritance. Native receiver
layouts place base fields before derived fields, and constructors initialize
the base state first. Synchronous and async dispatch use the frontend's
checked method targets and the receiver's concrete type. The inheritance
regression covers a three-level hierarchy, inherited source-interface
implementations, explicit base calls, nullable base references, and the
typed failure from dereferencing an absent receiver. An ordinary inherited
class can also be bound directly as a hosted receiver. Its constructor
records the concrete type for synchronous and async dispatch through base
and interface references. The hosted regression passed in both profiles
on Linux x86-64, Linux ARM64 under QEMU, and Windows x64 under Wine.
When an ordinary method can return or retain its receiver, native callers
keep that object in the invocation arena. Reference equality compares object
identity. The regression exercises separate receivers reused at one allocation
site and aliases returned through synchronous, recursive, and suspended calls.
Raw scalar object
exports still require the scalar receiver subset.
Each invocation-scoped constructor opens a savepoint after its arguments
are evaluated. A failed initializer restores writes to existing receivers
and releases the receivers allocated by that construction. A nested
constructor's success remains subject to the enclosing constructor and
invocation rollback. The regression checks nested state changes, retained
text and list values, 4,200 caught failures, and a later invocation
failure against the C++ reference.
Raw scalar object exports reject recursive synchronous calls. A native
invocation supports direct and mutual synchronous recursion under the
SDK's 32-frame source ancestry limit. The compiler checks worst-case stack
use before emission. The fixture compares local collection iteration,
cleanup, cancellation and limit failures with the C++ reference. Direct
and mutual async recursion reuse emitted continuation descriptors and
enforce the same source ancestry limit without growing the machine stack.

The DockPipe product harness uses a multi-fragment project, installed source
libraries, and a generated SDK bridge. The native driver now admits and
checks its `Entry.pipe` closure with the installed `PipeLang.Testing` sources
and materialized `Dockpipe.SDK`. The hosted path selects the actual
`Dockpipe.Product.Harness.Run` instance method. It lowers construction with
the `HarnessInputs` record and the source-built `ProcessPort`. It admits
`List<IAsyncTestCase>`, lowers the 29-word receiver of `HostWorkflowCase`,
and lowers the synchronous `AsyncTestRunner.ValidateBudgets` call and
`HashSet<string>` ID registration. Lowering also passes the earlier call-graph
limit, the path concatenation in `HostWorkflowCase.VerifyOwner`, and the
structured `FileObservation` result of `FilePort.Observe`. It now lowers
the awaited `Transport.Observe` call in `Dockpipe.SDK.Client` and the
`artifactRoot` string assignment in `HostWorkflowCase`. The coroutine
ABI stores Boolean arguments as bytes and restores them as Boolean values.
Receiver allocation provenance determines whether a constructor can retain
an argument. Direct calls can reuse fixed stack slots for scoped receivers.
The receiver fixture constructs a fieldless local in 256 loop iterations
and checks receiver access after a suspension. It also passes a contextual
local and an inline constructed value into a retaining constructor.

On Linux x86-64, the unchanged product source passed the failure/recovery
and graceful-cancellation cases and the failed-start custody control in
native debug, native release, and the C++ reference. The test used the
existing DockPipe service implementations with a temporary adapter for the
native hosted ABI. All three runs used DockPipe built from revision
`0f19c9e68`, and their case outcomes, process exit codes, interruption
flags and cleanup results matched. The installed DockPipe 0.6.38
reproduced the same graceful-cancellation failure with both backends.
The product harness also passed on Linux ARM64 in a full-system QEMU guest.
Native debug, native release and the C++ reference matched case outcomes,
exit codes, interruption flags and cleanup evidence. The guest kernel
provided the child-subreaper state required by DockPipe's process custody
checks. The product harness emitted debug and release objects for Windows
x64 and both macOS architectures. The Windows ARM64 object linked with
DockPipe's Windows adapter.
GDB stopped at the `HostWorkflowCase.pipe` assignment after the native
host call resumed, printed the returned observation and its artifact path,
and continued through a passing journey.

The native graph budget counts emitted operations per reachable method and
calling convention. Shared callees count once toward that budget, while
metered methods charge each execution through the invocation budget.
Unmetered helpers keep a static expanded-work bound, and direct constructor
calls from metered code charge their conservative cost before allocation.
The compiler keeps the source call depth and synchronous stack limits.
The graph fixture emits 40 helpers, runs at the depth boundary, and checks
runtime exhaustion as well as compile-time refusal for excessive depth and
unmetered work.

DockPipe's original driver-claims suite passed all nine cases in native debug,
native release and the C++ reference, including the full 512-feature,
8,192-claim case. Case identities, outcomes, cleanup flags and final JSON
document counts agreed. The earlier native arena rejected that case at its
receiver cap. The fixed-array regression also allocates, mutates and reads
600 independent receivers in both native profiles and the C++ reference.
The original driver-source and run-admission suites passed 13 and five cases,
respectively, in both native profiles using scoped file and JSON services.
The C++ reference agreed on case identities, outcomes, cleanup and release
of all JSON documents.
The original report projection matched its golden JSON bytes and released
every borrowed JSON document in both profiles.

The original DockPipe coverage and event-log suites passed all 11 and six
cases respectively in both native profiles. Both reports matched the C++
reference byte for byte, as did the event-log files. The separate event-output
contract ran its original suite, journal and projection on both backends.
Fresh and resumed journals matched after removing the recorded event times.
Warnings and journal custody matched in the warning-write failure case.
Eight input-custody cases, three lane-request cases, the staged-report rows,
and four run-input cases also passed in both native profiles. The proofs
compare retained files and exact report or request bytes. Run-input runner
paths and digests refer to each executing binary and are checked independently.
The evidence and lane-receipt reports matched the C++ case details across
21 and 30 cases respectively, including the 8,192-row receipt and its
document cleanup. The legacy C++ harness prints a Boolean in its schema
field. The comparison checks that field separately.

The native backend supports invocation-owned `HashSet` values for `int`,
`uint32`, Boolean, UTF-8 text, and enum keys. A set supports insertion, removal,
membership, clearing, count, equality, ordered iteration, and conversion to a
list. Mutations produce a new version for the destination local or parameter,
while earlier values keep their keys and insertion order. The SDK owns the
hash index and validates each key. Set versions use a bounded invocation
arena. Hosted receivers retain immutable set values in fields, including
fields inside records. Constructor arguments can supply sets through the
hosted value API. Receiver checkpoints restore both the value and its
retained storage on failure. The fixture verifies bulk construction with
1,101 keys, record replacement, alias isolation, suspension, and rollback.

The set fixture agrees with the C++ reference on duplicate detection,
unordered equality, insertion order, and copies across suspension. GDB
stopped at the original PipeLang line after the await and read the retained
set count. Debug and release object emissions matched byte-for-byte on Linux
x86-64, Windows x64, and macOS ARM64 in repeated builds.

Hosted receiver storage retains reference-free collection values and owned
payloads across invocations. Inline record fields retain their values at
their own offsets so replacing a whole record and assigning one member use
the same storage. The runtime checks the stored value's shape, charges its
retained bytes, and restores the previous snapshot on segment rollback.
The persistent report fixture checks a constructor-supplied list, nested
list and dictionary values, mixed text and set fields, and isolation between
receiver instances against the C++ reference. The invocation reuses the
packed value for repeated reads of the same snapshot. Importing a new
snapshot and projecting a replacement each copy the value's contents.

Native `Queue<T>` and `Stack<T>` support invocation-owned values, including
record members, snapshots, equality, iteration, and transfer through hosted
calls. Queue iteration and hosted values expose the oldest item first. Stack
iteration and hosted values expose the top item first. Both sequences share
the SDK's element and owned-byte admission rules. Native queue compaction
copies the active range when the discarded prefix outweighs it and leaves
earlier copies valid. A suspended invocation retains its sequence values.

The sequence fixture exercises queue rotation across 4,200 updates, stack
order, and empty-operation outcomes against the C++ reference. GDB reads
both counts at the PipeLang source line after a timed wait. Both native
profiles run standalone and hosted sequence operations under Linux ARM64
QEMU and Windows x64 Wine. Windows ARM64 PE and PDB files and both macOS
object formats were inspected locally.

Invocation-owned native dictionaries use the same SDK map as the C++
reference for key validation, insertion order, equality, and ownership
limits. They support lookups, optional results, insertions, replacements,
removals, clearing, and ordered keys, values, and entries. Native iteration
reads directly from the saved version. A service can return a dictionary
in one import, with a single published version. The fixture checks a
1,026-entry service return, all supported key representations, NaN value
equality, and the aggregate byte limit.
The standalone and hosted fixtures passed on Linux ARM64 under QEMU and
Windows x64 under Wine. GDB and ARM64 LLDB inspected the saved dictionary
and payload after suspension at the authored PipeLang source line. Windows
ARM64 PE and PDB files and both macOS object formats were inspected locally.
Repeated Dictionary object emission matched byte for byte on all six targets
in debug and release.

Native `uint32` values stay zero-extended in invocation storage. Checked
arithmetic, shifts, and narrowing from `int` use the SDK's shared numeric
operations. Wrapping and bitwise operations lower to LLVM integer instructions.
The fixture checks values at both unsigned boundaries, the fault messages
reported to callers, and preservation across suspension, records, lists,
sets, and nullable values. A seeded test generates 24 arithmetic sequences
and compares debug, release, and C++ reference results with independently
calculated values. The hosted ABI keeps `uint32` distinct from `int` for
construction, service calls, and result projection. A hosted invocation
also verifies rollback of a receiver field after arithmetic overflow.
GDB reads the retained maximum value as `uint32` at the original PipeLang
line. The standalone app ran in both profiles on Linux x86-64, under
Linux ARM64 QEMU, and under Windows x64 Wine. Windows ARM64 PE and PDB
emission passed inspection.

Direct native `float` and `float32` arithmetic uses constrained LLVM
operations, preserving IEEE comparison behavior and the order of floating
operations. Float values keep their bit patterns across suspension, owned
records and lists, and hosted service calls. The SDK supplies math and
checked conversion operations. On Linux the link SDK stages the static
math archive alongside its other link inputs.

The floating fixture covers signed zero, subnormals, NaN payloads,
infinities, rounding, math builtins, and conversion faults. The list
equality test includes a NaN inside a record and verifies that a shared
list allocation does not bypass comparison of its values. Native debug and
release matched the C++ reference for this fixture. GDB on Linux x64 and
LLDB on emulated Linux ARM64 displayed the original `float32` locals after
suspension. Synchronous `FloatingEnvironment.Observe()` reads the same SDK
facts as the C++ reference. The Linux x86-64 fixture checks all four portable
rounding modes and verifies the call preserves the caller's rounding mode and
exception flags. Both native profiles also passed under Linux ARM64 QEMU and
Windows x86-64 Wine.

The explicit Linux x64 active-host option lowers `float32.FromFloat` using
the active hardware rounding and denormal controls. It preserves the
hardware exception flags and canonicalizes NaN bits after conversion.
Hosted arithmetic matched the maintained C++ reference in 2,944 cases per
profile, including independent x87 and SSE controls.

Direct numeric C ABI objects use the checked SDK numeric profile to admit
bounded pure calls. The primitive projection covers floating arithmetic,
comparison, narrowing, widening, signed integer to floating-point conversion,
and direct calls between admitted methods. The object exports its chosen
symbol and links independently of the PipeLang application runtime.
The regression compares values, exception flags, and active controls in
debug and release. It also calls and returns nested numeric records through a
plain C translation unit and checks calls with shared input and output storage.
Record fields can contain the four numeric primitives or another nonempty
numeric record. Other aggregate signatures, numeric builtins, and
active-host targets still need qualification.

Direct native `ByteBuffer` supports byte reads, slices, search, UTF-8
conversion, append, snapshot iteration, and owned transport across calls.
Invocation-local buffers keep bounded views over their storage. Appending at
the end of a buffer can reuse its allocation while earlier copies keep their
original lengths. Hosted values own their bytes and receiver fields retain
older values as needed for rollback and suspended calls.

For synchronous host results inside a loop, native lowering reuses byte
storage when checked uses prove that the successful payload stays local.
Escaped payloads and the failure and cancellation values retain their
ordinary lifetimes. The regression streams changing chunks across the
existing byte storage cap, checks both typed and string error results,
and verifies retained aliases after suspension and later reads. The
maintained SHA-256 suite ran the original 512 MiB streaming case through
the real provider with the same result as the C++ reference.

The original CryptoStream, FileStreams, atomic replacement, exclusive
output, and existing-file append fixture projects imported the pinned SDK
source descriptors. On Linux x86-64, debug and release matched the C++
reference reports for all 80 source cases. The runs shared the original
file fixture implementations and the same crypto provider.

The fixture checks self-append, copy isolation, the one-megabyte size limit,
invalid UTF-8, and asynchronous transport of embedded zeros and non-UTF-8
bytes. The separate JSON fixture imports the pinned `PipeLang.Json` SDK and
uses its actual reader to decode split UTF-16. It verifies that the original
input bytes survive and that valid, invalid, and cancelled documents release
their storage. The test also covers named record initialization, local
record updates, and passing a cancellation token into a synchronous service.

Direct native methods can replace a receiver-owned record or write its
individual fields. The runtime stages new text and byte storage before
publishing a replacement and tracks the previous state in the receiver's
rollback checkpoint. Byte-buffer operations also work through fields of
receivers and local records. The hosted receiver fixture checks that an old
record snapshot survives suspension, text and bytes survive the invocation
that wrote them, and failure restores writes across two receivers.

Ordinary class references can travel through contextual and async calls,
results, and invocation-owned collections. Native lowering promotes
locally constructed receivers when their references are used as values.
Standalone receivers live until the invocation's last frame exits.
Hosted receivers can retain class and interface references through
optional, list, dictionary, queue, and stack fields. The runtime validates
reference ownership before publishing a segment and traces reachable
receivers after each hosted call. Initial hosted construction records
the same source-reference roots as invocation writes. Dropping a root
releases unreachable receivers, including cycles and receivers created
in the binding constructor. Earlier hosted graph objects keep their
constructor allocations rooted when relinked, since their descriptors
lack field metadata. Class references are rejected in exported results.

Field replacements use the same segment rollback as scalar writes.
Synchronization fields on retained receivers keep their SDK backing
cells when the invocation ends, including when two receivers share a
cell. Failed constructors release the cells they introduced. The
qualification workflow checks object composition, receiver identity,
cycles, repeated constructor failures, shared synchronization, and
collection rollback in both native profiles.

The native workflow also runs the existing member-binding, empty source
interface, byte-iteration, and typed result propagation fixtures in debug
and release. These checks
cover constructor and member shadowing, host calls across suspension,
empty collections and invalid calls through imported interfaces, and
iteration over all 256 byte values. The byte fixture also checks overload
resolution, snapshot ownership, and evaluation exhaustion. Typed result
propagation keeps error records and cancellation intact through loops,
cleanup, and nested or void results at the host boundary. The matching
C++ fixtures run in the compatibility workflow.

`ContextOwner` uses the SDK's owner-thread lifetime and revocation rules.
Native hosted bindings expose source context getters through
`HostedInstance::startContextOwner`. The handle inherits its binding's
lifetime and parent permit, including when the host retains it after the
binding is destroyed. Failed source mutations roll back receiver state
while revocation remains in effect. Source construction requires every
non-nullable context owner field to be initialized before publication.
An invocation captures its publication authority on entry. Revocation
prevents its next segment from publishing, preserves earlier commits, and
returns `context.stale` as cancellation. Explicit body and cleanup tokens
remain independent of that publication check.
The conformance test exercises these contracts in debug and release against
the C++ reference, including repeated cleanup of child receivers.

Compile with `--host-snapshot` or the CMake `HOST_SNAPSHOT` option to
expose public model state through `HostedInstance::snapshot()`. The
owner thread can read committed state during suspension. A revocation
observer that runs inside a source mutation still sees the last committed
state. The runtime shares immutable owned values with the returned
snapshot, which can outlive the binding. Earlier hosted objects can
still be loaded by the same SDK.

Use `--host-multiple-tasks` or CMake `HOST_MULTIPLE_TASKS` to run
overlapping hosted tasks on the binding's owner thread. Active tasks
share one `TaskScope` and publish checked changes at execution-segment
boundaries. A task waiting on a host service or semaphore yields to
other tasks. The runtime retains its borrowed graph receivers until
the task exits. Cancellation releases that task's roots while
preserving objects referenced by other tasks. A second task cannot
enter the model while a source segment is modifying unpublished state.

Use `HostedInstance::tryStartMethod` to admit a hosted source operation into a
group from its owning `TaskScope`. The admission outcome reports missing
methods, invalid argument counts, pre-cancellation, closed groups and capacity
limits. The admitted task retains the existing hosted invocation outcome.
Use `TaskScope::closeGroup` to request cancellation for its subtree and
`TaskScope::drainGroup` to wait for those tasks to settle. Sibling groups can
continue. The regression checks that each authored finally effect runs once
and that cancelled receiver writes follow the reference transaction policy.

If a model has several public constructors, use `--host-constructor`
or CMake `HOST_CONSTRUCTOR` with the checked parameter type list,
such as `()` or `(int)`. Every exported descriptor binds one constructor.
A host can link descriptors for several constructors by giving each
compiled object a distinct host symbol. The regression selects both
constructors from the existing composition fixture and compares their
state and generic method results with the C++ reference.

Use `--host-composition` or the CMake `HOST_COMPOSITION` option with
`HOST_OBJECT_REFERENCES` to include constructor recipes from a checked
composition in a hosted native object. `HostedInstance::constructService`
takes the fully qualified service name and its checked constructor arguments.
The constructors execute inside the binding's object graph. They accept scalar,
string, byte buffer, record, and collection snapshots, borrowed host ports,
and references to native objects of the checked concrete class from the same binding. If construction
fails or loses its publication authority, it rolls back changes to existing
native receivers before returning the outcome.

The binding retains strings, byte buffers, lists, arrays, hash sets, queues,
stacks, and dictionaries after their constructor returns. A task that reads
those fields before suspending keeps its snapshot when another task replaces
the fields. Implicit synchronization preserves effects when a base constructor
reaches a derived field through virtual dispatch.

A hosted constructor or method can borrow a C++ SDK semaphore, lock, atomic,
concurrent queue, concurrent stack, or concurrent dictionary. Wrap the existing
cell with the corresponding `HostedValue` factory and pass it as a top-level
binding or method argument. A constructor can retain these cells in its fields.
Their effects survive a later failure in the constructor.
The native invocation keeps the cell alive and uses its existing identity.
The binding checks the declared atomic and collection types before
executing source. Earlier runtimes refuse descriptors that require
unsupported shared argument types.

The typed collection overloads use `HostedValueCodec<T>` with the checked
PipeLang type name and C++ encoding and decoding functions. A dictionary
takes separate codecs for its key and value. Keep the functions safe for
concurrent calls and free of effects on the borrowed collection because a
projection can run while its cell is locked. A failed projection preserves
the previous contents. The integer queue overload can still borrow its
original C++ cell without a codec.

Pass `model::ParallelOptions` to the four-argument `HostedInstance::bind`
overload to set the maximum parallel worker count for that binding. Valid
counts range from 1 to 32. Existing overloads keep the default of four.
Parallel loops in hosted constructors use the binding's worker count and
share the initialization budget. A worker failure reaches the caller before
the constructor publishes ordinary state.

Lists can retain references to source objects in their owning receiver
graph. Reads preserve the selected object identity through list replacement,
and a failed call rolls back writes to that object. Record fields with
authored defaults are initialized before the constructor body runs.

A binding can admit a new scoped constructor while another admitted task is
suspended. Drain a service's task group before calling `HostedInstance::retireService`.
Other groups can keep their work pending. Retirement refuses while a borrowed
port has an outstanding host call and releases the binding's references to the
ports after settlement. Stale receiver handles fail when they call a retired
port, including after another receiver reuses its slot.

The installed native SDK's `pipelang/model/native_composition.hpp` exposes
checked constructor and factory plans through `HostedComposition`. Bind
the singleton inputs at the root and supply scoped inputs when creating a
child scope. Resolution reuses root singletons and services cached by each
scope. Every transient gets a fresh receiver. The SDK publishes a resolved
graph after all of its recipes succeed. Factory failure, cancellation and
receiver reuse leave previously published services intact. A factory returning
a derived resource through its base type uses the derived method and
cleanup implementations.

Constructed services own their implicit synchronization fields. An uninitialized
semaphore starts with zero permits. Locks, atomics, and concurrent collections
start with their usual default values independently in each receiver. Scope
closure cancels a suspended semaphore wait and drains its authored finalizer
before resource cleanup.

Call a model method with `tryStartMethod` or a checked export directly
with `tryStartServiceMethod`. Direct calls select the source signature,
for example `Read()` or `ReadStatus(bool)`. Class and source interface
exports select the checked concrete implementation. Synchronous authored
Result values retain their tags inside the invocation result. A child
scope can use its root's singleton handles. Compile with
`HOST_MULTIPLE_TASKS` for sibling calls in flight at the same time.

Closing a scope settles its task group and invokes authored resource
cleanup in reverse acquisition order. Root close uses the actual order
across its children and singleton resources. `closeAsync` returns a
completion handle that advances through `poll` while host callbacks
settle. Dropping the last scope handle also requests cleanup. Attempted
callbacks are recorded in `cleanupStatus`, including outcomes with
unknown external effects. An owner-free `observeCleanup` session retains
terminal diagnostics and task completion after the binding is released.
Failed construction releases adopted resources
while their provisional fields and dependencies remain available. Resolution
returns the original construction outcome. Cleanup uses an independent
token and a bounded evaluation allowance.

Hosted constructors can call source interfaces and virtual methods on the
receivers they build. These calls share the constructor's receiver arena and
evaluation budget. A constructor can inspect a concrete `Result`, including
the checked `Value` accessor, and raise an authored `Error`. The DI regression
uses the existing `RootDependency` and `RootService` constructors and compares
dispatch, host call order, authored failures, and cancellation with the C++
reference. GDB steps into the authored interface implementation and verifies
that its receiver matches the caller after the argument stores.

Hosted construction also supports source `try`, `catch`, and `finally`.
Cleanup runs on normal exits, faults, cancellation, and exhaustion of the
initialization budget. It uses the existing bounded cleanup allowance.
When cleanup fails during unwinding, the constructor reports the original
fault. Each nested `new` opens a savepoint after evaluating its arguments.
If construction fails, the caller can catch the error after the runtime
releases the failed receivers and restores changes to surviving receivers.
The regression compares cleanup, loop transfers, and repeated construction
rollback with the C++ reference.

Recursive helpers and nested constructors share the SDK's 32-frame source call
limit. A nested constructor checks that limit before opening its savepoint.
Hosted constructors can also build lists, append to them, and read their count
and indexed elements. A saved list retains its prefix when a local alias grows.
Sets, dictionaries, queues and stacks use the same owned collection operations
as invocations. Hosted iteration preserves its source snapshot and checked
optional access reports errors through the source catch.

A failed nested construction releases byte buffers, arrays, lists, queues,
stacks, sets and dictionary versions allocated after its savepoint. Appending
to a buffer, list, queue or stack backed by a block that predates the
savepoint creates a separate block so rollback can release the growth. The
regression repeatedly constructs and discards owned values, verifies that
earlier collection contents survived, and compares cleanup order with the
C++ reference.

Hosted initialization uses the SDK's atomic, semaphore, and lock operations.
Default atomic and lock fields create their cells on first access. A source
lock releases its guard before control reaches the enclosing catch, and an
effect on a shared atomic cell survives receiver rollback. The constructor
regression compares both behaviors with the C++ reference.

Hosted constructors also use the SDK's concurrent dictionaries, queues, and
stacks. Default receiver fields materialize on first access, and iteration
uses a snapshot taken when the loop begins. The differential regression clears
collections during iteration and verifies that computed text stored in shared
collections survives a failed nested construction.

Hosted construction uses the same floating-point math, numeric conversions,
and floating environment observations as native invocations. Checked
conversion failures reach source catches. The fixture checks both float
precisions, the reported rounding mode, and a caught conversion failure.
Checked and wrapping uint32 operations use the same source semantics, including
overflow, invalid shifts, and out-of-range conversions.

Synchronous helpers called during hosted initialization can propagate a
typed Result through source finally blocks. Constructors can inspect erased
IResult outcomes and typed errors. An empty interface reports the source
failure before any payload access. Nested record assignments work
for locals and receiver fields. Owned fixed-array creation and replacement
check the aggregate ownership limit before publishing the new value.
The regression checks cleanup after success, failure and cancellation,
owned record bytes after binding, and both caught and uncaught array faults.

Hosted initialization can construct key-value pairs, read ordinary and
concurrent dictionary keys, values and entries as owned snapshots, and
observe CancellationToken.None.

Select overloaded methods with a parameter type list in `--entry`
or `--host-method`, such as `Add(int)` and `Add(float)`. A hosted
descriptor can expose both overloads, and callers use their qualified
method selectors with `HostedInstance::startMethod`. Inherited methods
retain the receiver's concrete type for virtual dispatch. The debugger
uses the declaring PipeLang class and file.

Hosted constructors can allocate, read, update, and iterate owned
arrays. Construction and normal invocations share checked array
operations. The constructor owns its allocation until binding
completes. The regression checks size refusals, copy-on-write
iteration, and owned snapshots against the C++ backend. It also runs
the original Collections fixture against each backend.

Hosted constructors use the same checked text operations and owned-value
equality as ordinary native calls. A constructor can retain a slice of
computed text in an owned array. The reference comparison checks that
the slice remains valid after binding and that splitting a UTF-8 scalar
fails construction with the original error.

Hosted constructors can also build, append, slice, inspect, and iterate
byte buffers. Byte buffers assigned to inline receiver fields or nested
records retain their final bytes before binding publishes the instance.
Repeated field writes reuse the constructor's byte arena. Array elements
keep their constructor-owned values when a local buffer or another field
changes. The fixture checks 12,000 incremental writes and reads of binary
slices and snapshots after binding.

Conditional expressions in hosted constructors can select owned values
such as records. Only the selected branch runs. The array fixture checks
that an empty array can be constructed when the other branch would divide
by zero. Source-built constructors can also accept checked source interface
references. The hosted interface fixture passes references during initial
binding and later invocation, then dispatches through the retained object
in a subsequent call. Both debug and release passed with the C++ reference.
DockPipe's Windows SDK qualification harness, including its source decorator,
emits Windows x64 and ARM64 objects in both native profiles. A local
qualification adapter linked those objects with the Windows SDK transport and
the native PipeLang runtime. The four PDBs include the original
`FaultHarness.pipe` source. The linked harness still requires execution on
physical Windows.

Use `--host-object-references` or CMake `HOST_OBJECT_REFERENCES` to
pass object references through methods on their owning binding. A
projected reference keeps its graph object reachable until the host
releases it. The binding enforces its owner thread, checks the
declared argument type, and refuses references from other bindings
or closed owners. Use `HostedValue::sameObject` for alias comparisons.
During graph reclamation, revocation callbacks can read a committed
snapshot. Attempts to start a nested source invocation are rejected.

On Linux x86-64, 800 snapshots of a retained 4,096-element list took a
median of 440.973 ms with list copying and 0.205 ms with immutable
sharing. The C++ reference took 0.467 ms on this fixture. Seven samples
per implementation rotated run order on one allowed CPU. Both native
runs linked the same release object, and each run checked the returned
list. The measured interval excludes construction and the warm snapshot.
[Reproduce the measurement](../../workflows/native.debug/assets/scripts/measure-hosted-snapshot.sh).

Reachability scans reuse the last successful sweep for read-only calls.
Receiver allocations and reference or synchronization ownership writes
invalidate it. A successful final segment can pass its validated marks
to the sweep after the frame exits. A failed projection or rolled-back
segment leaves the graph eligible for fresh validation.

On Linux x86-64, 800 reads of a retained 4,096-receiver graph took a
median of 803.274 ms before this change and 1.291 ms afterward. The C++
reference took 0.176 ms on the same reads. Seven samples per build
alternated backend order on one allowed CPU. The two native runs linked
the same release object with their respective SDK archives. Each run
checked the returned values and receiver capacity. The measured interval
starts after graph construction and a warm read.
[Reproduce the measurement](../../workflows/native.debug/assets/scripts/measure-hosted-graph.sh).

Native `FixedArray<T,N>` stores its elements inline. The lowerer checks
dynamic indices before accessing memory and uses direct projections for
literal indices whose bounds it can prove. Hosted receivers store numeric
arrays inline and retain arrays with owned elements across invocations.
The runtime checks arrays against the SDK ownership limits before publishing
a replacement. Failed updates preserve the previous value. Hosted transfer
checks the nominal array name and exact extent. The fixture compares preserved
text, bytes, and numeric fields in committed snapshots after a failed update.
It also covers host round trips, iteration, rejected indices, and rollback.
Source debug information records the element type, extent, and stride.

Dynamic `Array<T>` values share immutable storage versions within an
invocation. Creating a uniformly filled array stores one copy of its
element, and setting a position copies the path to that element.
Aliases and iteration snapshots keep their original versions across
calls and waits. The SDK checks size, index, and logical ownership
limits before publishing a replacement and checks the nominal array
type during host transfer. Invocation-scoped constructors can create
arrays and call checked synchronous methods under the invocation's
evaluation budget. Their records can retain invocation-owned collection
values. Hosted construction retains its persistent field checks.

Native source interface expressions can construct closed implementations and
store them in invocation-local lists. Each allocation has a stable address and
a concrete type tag. The lowerer builds dispatch for synchronous and
asynchronous methods from the checked implementation set. A list of
interface values can pass to a child async method with an argument such as
a record. Debug and release
pass aggregates by borrowing durable caller-frame storage until the child has
copied its parameters. Source interface values are confined to the invocation
at the host result boundary. The native arena uses the SDK object domain's
4,096-cell limit and charges up to 1 MiB of receiver words, including headers.
The rollback journal has a separate 1 MiB cap. The interface proof covers
a mixed list, repeated allocations at one source site,
and cancellation during either of two genuine suspensions. The C++ reference
agrees on the successful values and returned failure. The hosted inheritance
fixture also exercises a diamond of source interfaces, cross-module dispatch,
alias mutation and rollback, and a suspended call that observes a later
mutation through the same receiver. Both native profiles and the C++ reference
pass these source contracts. GDB stopped inside the
selected implementation after suspension and read the typed request record.
The Windows executable passed under Wine. Repeated debug and release object
emissions matched on Linux x86-64, Windows x64, and macOS ARM64.

### Initial reference measurements

#### DockPipe target-report processes

On an Intel Core i9-10900K running Linux x86-64, each backend ran the same
four-case target report in 21 fresh processes after three warmup runs.
The benchmark alternated the first backend between measurement pairs and
compared every report byte with the checked output. The filesystem cache
was warm. The native release used LLVM 15. The official C++ probe used
GCC 11 at `-O1` with compiled Source units. The elapsed time covers process
startup, file and JSON operations, and report completion. The artifacts
include their respective host adapters and the same crypto provider.

| Backend | Median elapsed | 90th percentile elapsed | Median peak RSS | Executable size |
| --- | ---: | ---: | ---: | ---: |
| Direct native release | 7 ms | 10 ms | 5,972 KiB | 6,607,968 bytes |
| C++ reference | 11 ms | 18 ms | 7,420 KiB | 7,675,464 bytes |

The benchmark accepts the native executable, C++ executable, input
directory, expected report, and a fresh output directory in that order.
It records executable and input identities, host details, and raw
samples. [Benchmark script](../../workflows/native.debug/assets/scripts/measure-target-report.sh).

#### DockPipe atomic run-report processes

Nine interleaved Linux x86-64 runs per backend measured all six atomic
report cases in fresh processes after one warmup run per backend. The
filesystem cache was warm. Native used the release profile with LLVM 15
and C++ used GCC with `-O1`. Each pair checks the report contents after
normalizing the observed executable path and digest.

| Backend | Median elapsed | Median peak RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native release | 340 ms | 37,548 KiB | 6,491,792 bytes |
| C++ reference | 340 ms | 38,072 KiB | 6,313,328 bytes |

#### Persistent hosted list reads

A Linux x86-64 release probe binds 4,000 strings of 200 bytes and reads
80 entries from the same receiver field during one invocation. The initial
implementation failed at the invocation text storage limit because each read
imported the entire list. With invocation-local reuse of the packed snapshot,
five runs completed with the expected total of 16,000. Measured time from
host input construction through result projection ranged from 5.10 to 5.55 ms
with a median of 5.34 ms. The failing baseline provides no comparable
successful runtime. The [persistent report fixture](../../workflows/native.debug/assets/PersistentReportValues.pipe)
checks the same workload in both native profiles and the C++ reference.

#### Literal concatenation folding

The native typed IR folds concatenations of validated literals within a basic
block when the result fits the SDK string limit. The operation keeps the source
location and evaluation budget instructions. Concatenations involving dynamic
values or exceeding the limit use the existing checked runtime operation.

On Linux x86-64 with an Intel Core i9-10900K and LLVM 15, seven interleaved
samples per build followed one warmup per build. Each sample ran 80 hosted
calls with 4,096 concatenations each. Both builds used the same runtime
archive and checked the exact returned byte count on every invocation.

| Native release build | Median elapsed | Owner-thread allocation calls |
| --- | ---: | ---: |
| Before folding | 68.235 ms | 329,681 |
| With typed IR folding | 29.083 ms | 2,001 |

The object relocation count for the shared text operation decreased from two
to one. The benchmark counts calls to scalar C++ `operator new` on the
invocation thread during this workload. Pass the baseline and candidate
`pipelang-objectc` executables, LLVM `llc`, SDK include directory, native runtime
archive, C++ host-fixture compiler, and a fresh output directory to the
[benchmark script](../../workflows/native.debug/assets/scripts/measure-native-text-fold.sh).

#### Checked integer constant folding

The typed IR evaluates successful same-block integer operations with known
operands using the SDK checked arithmetic. Runtime faults retain their source
location, handler, and evaluation order. The regression checks overflow,
division by zero, and the minimum signed integer remainder edge case.

On Linux x86-64 with LLVM 15, seven interleaved samples followed a warmup.
Each sample ran 80 hosted calls with 4,096 iterations of the same checked
arithmetic loop.

| Build | Median elapsed |
| --- | ---: |
| Native release before folding | 29.271 ms |
| Native release with folding | 18.549 ms |
| C++ reference | 7.931 ms |

The release object went from twelve checked-integer runtime call sites to
three. The [verification and benchmark script](../../workflows/native.debug/assets/scripts/verify-integer-fold.sh)
accepts the candidate object compiler, reference model compiler, C++ host
compiler record, LLVM `llc`, SDK include directory, runtime archive, a fresh
output directory, and the baseline object compiler to enable measurements.

#### DockPipe driver-claim matrix

Seven interleaved Linux x86-64 process runs per backend measured the full
nine-case source suite after one warmup per backend. Native used LLVM 15
release objects and C++ used GCC with `-O1`. Both used the existing
33,554,432-step evaluation profile and compared the same case identities,
outcomes, body and cleanup flags, and final JSON document count. The two
drivers format their output differently. GNU time recorded wall time with
centisecond output precision. Other qualification jobs shared the host.

| Backend | Median elapsed | Median peak RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native release | 0.02 s | 18,560 KiB | 921,552 bytes |
| C++ reference | 3.96 s | 5,408 KiB | 684,776 bytes |

The native baseline failed the full matrix at its receiver storage limit,
so it has no successful runtime baseline. These results cover the complete
standalone fixture, including the host bridge and startup.

An isolated follow-up compared the native release pipeline with LLVM 15's
`default<O2>` IR pipeline inserted before machine-code emission. Five
interleaved measured builds per variant produced deterministic objects and
matched the reference outcomes on every run. Median object emission rose
from 0.39 s to 1.43 s. Both executables measured 0.02 s at GNU time's
centisecond precision. The extra pipeline reduced the object from 135,304
to 119,440 bytes and the linked executable from 925,840 to 914,672 bytes.
The production candidate keeps the existing release pipeline for this
tradeoff.

The GNU release build compiles the host lowering translation unit with
`-O2`. Seven interleaved builds of the multi-file DockPipe catalog produced
identical 152,376-byte objects with host lowering compiled at either
`-O3` or `-O2`. Median catalog compilation time was 0.46 s and 0.47 s,
respectively, with median peak RSS of 73,052 KiB and 73,032 KiB. No
application speedup was measured. This setting controls compilation of the
host tool. LLVM still applies the selected PipeLang code generation profile.

#### Standalone unsigned arithmetic

Five interleaved builds per backend compiled the same seeded arithmetic
program into static Linux x86-64 executables on an Intel Core i9-10900K.
Direct native used the release profile, LLVM 15, GNU `ld`, and the staged
runtime archive. The reference emitted headers from the same source and
compiled the test driver with `-std=c++20 -O2 -g1 -static -pthread`. Each
executable's first result was checked against the generated independent
result. [Program generator](../../workflows/native.debug/assets/scripts/generate-uint32-cases.sh)
and [reference test driver](../../workflows/native.debug/assets/UInt32ValuesReference.cpp).

| Backend | Median build wall time | Median peak child RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native | 0.62 s | 79,684 KiB | 2,912,312 bytes |
| C++ reference | 5.18 s | 363,160 KiB | 3,405,400 bytes |

The build timer includes source compilation and linking from fresh output
directories with normal filesystem caching. The measurements share the host
with qualification jobs. They establish a baseline for this fixture.

#### Standalone floating-point arithmetic

The same five-build Linux x86-64 procedure measured the floating-point fixture
with static linking and source line information in both backends. Every
executable returned the independently expected result of `42`.
[Floating-point fixture](../../workflows/native.debug/assets/FloatingValues.pipe).

| Backend | Median build wall time | Median peak child RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native | 0.53 s | 69,368 KiB | 3,226,744 bytes |
| C++ reference | 4.20 s | 332,704 KiB | 3,427,064 bytes |

#### Standalone byte buffers

The five-build procedure also measured the static ByteBuffer fixture on
the same host, before the nested record mutation case was added. Both
backends returned the independently expected result before each build
sample was admitted.
[ByteBuffer fixture](../../workflows/native.debug/assets/ByteBuffers.pipe).

| Backend | Median build wall time | Median peak child RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native | 0.51 s | 69,172 KiB | 2,919,312 bytes |
| C++ reference | 5.14 s | 355,392 KiB | 3,685,928 bytes |

Repeating the same five-build procedure with the record mutation fixture and
updated runtime gave the following medians. The fixture changed between runs,
so these results compare the two backends on the updated source.

| Backend | Median build wall time | Median peak child RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native | 0.53 s | 69,556 KiB | 2,923,760 bytes |
| C++ reference | 5.06 s | 359,160 KiB | 3,691,464 bytes |

#### Queue and stack

The same five alternating builds measured the standalone
[sequence fixture](../../workflows/native.debug/assets/Sequences.pipe).
The two executables both returned 42 before each build sample was admitted.

| Backend | Median build wall time | Median peak child RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native | 0.52 s | 69,628 KiB | 2,941,440 bytes |
| C++ reference | 4.80 s | 354,104 KiB | 3,592,672 bytes |

In 20 alternating warm process runs per backend, each with one warm-up and
an exit status and output check, median elapsed time was 53 ms native and
52 ms for the C++ reference at millisecond timer resolution. These timings
include launcher and scheduler overhead and the fixture's timed waits.

#### Dictionary

Five alternating builds of the standalone
[dictionary fixture](../../workflows/native.debug/assets/Dictionaries.pipe)
used the same compiler, measurement procedure, and expected result of 42.

| Backend | Median build wall time | Median peak child RSS | Executable size |
| --- | ---: | ---: | ---: |
| Direct native | 0.54 s | 70,516 KiB | 3,068,488 bytes |
| C++ reference | 6.15 s | 421,600 KiB | 4,048,456 bytes |

The median in 20 alternating warm process runs per backend was 17 ms for
both executables at millisecond timer resolution, including the timed wait.

#### Earlier frontend and platform measurements

On Linux x86-64, release compilation of the unchanged independent-cancellation
fixture with LLVM 15 emitted these continuation storage requirements.
They count the words in the generated descriptors for locals and owned scalar
objects. The v10 entry and worker also reserve eight words each for the
outgoing and incoming result buffers. The v9 runtime kept those results
separately from its descriptor's slot count.

| PipeLang function | v9 words | v10 words |
| --- | ---: | ---: |
| `App.Run` | 84 | 36 |
| `Worker.Execute` | 40 | 16 |
| `Helper.Close` | 24 | 6 |

These are generated storage sizes. End-to-end compilation latency, compiler
RSS and application throughput still need separate measurements.

On Linux x86-64 with an Intel Core i9-10900K, the pre-refactor reference compiler
produced the following results across 20 fresh processes per input:

| Input | Median | P95 | Maximum compiler RSS |
| --- | ---: | ---: | ---: |
| Single class returning an integer | 2.72 ms | 2.97 ms | 5,672 KiB |
| [Var inference fixture](../../tests/native-models/VarInference.pipe) with generics, collections and async effects | 4.17 ms | 4.68 ms | 6,164 KiB |

The measurements include process launch and JSON serialization through C++ source
generation. The file cache was uncontrolled and a compatibility build was active
on the host. They exclude downstream C++ compilation, linking and application
execution, so they are diagnostic samples and cannot serve as the full
end-to-end performance baseline.

An external LLVM 15 capability probe produced byte-identical repeated relocatable
objects for all six planned triples from a small handwritten IR input. A separate
handwritten IR sample linked and ran on Linux x86-64 without a C++ compiler.
GDB stopped at a mapped `.pipe` source line and read a local variable before
and after assignment. These are toolchain probes. PipeLang lowering, executable
linking, runtime ABI and platform execution require separate proof with emitted
PipeLang programs.
