# Native ordinary-model foundation runtime

This header-only C++20 runtime supplies language-owned values, transactional
model calls and structured host tasks. It has no ForgePipe, Qt, view compiler or
renderer dependency. `pipelang/model/runtime.hpp` is its public include; consumers
link standard threads. The location under the PipeView SDK is packaging, not a
private UI action language.

Generated C++ exposes source names and types. A class's `snapshot() const` returns
an owned `Model::State` containing only public owned fields. Private state remains
in an inaccessible transaction-storage type; private injected services are never
in a snapshot or public manifest. Pure methods return their raw typed value. Synchronous
effects return `Result<T,std::string>`; an authored Result return remains that
same Result, and void uses `InvocationResult`. Calls inspect `.is_success()`,
`.is_cancelled()`, `.is_failure()`, `.error()` and, for successful nonvoid results,
`.value()`. Wrong-case Value access is an explicit runtime failure.

An async source method returns a copyable `Task<Result<T,std::string>>` and starts
on the model's creator thread immediately. `completed()` and `result()` are
creator-thread reads; pending result access is refused. `cancel()` is thread-safe.
A generated constructor accepts its authored parameters plus optional trailing
`TaskOptions{capacity}`. Capacity defaults to four active coroutines, with an
admitted range of 1..32. Excess starts return completed Failure tasks without
executing source. Application single-flight rules remain authored ordinary state
and methods; there is no built-in Save policy.

Each model scope owns one worker and an owner executor. Host interface source
methods declare Task<Result<T,string>>; generated C++ host implementations provide
`virtual Result<T,std::string> Method(owned_arguments, CancellationToken)`. Only
runtime jobs call these host methods, on the worker. Model state is never shared
with the worker. Captured records, strings, results and cancellation state are
owned; the host interface lifetime is shared until its tasks finish. Host jobs
are serialized in FIFO submission order and completions are delivered in that
order by owner `poll()`. One poll processes only its entry queue; newly queued
completions wait for a later poll. Different multi-await tasks may finish in a
different order from initiation, deterministically following their FIFO host-job
and owner continuation sequence. No worker or foreign thread resumes model code.

Cancellation combines caller tokens and task-owner cancellation. Pending jobs
skip host invocation when cancelled. In-flight hosts must cooperate and eventually
return; arbitrary blocking native I/O cannot be preempted safely. Host exceptions
become Failure. Strings, recursively nested record/Result values and host error
text are checked for scalar UTF-8 and the string byte ceiling before admission;
malformed native exception text is replaced by an explicit ASCII failure detail. A successful host result remains authoritative even if late
cancellation arrives after an external commit. Source handles success, failure
and cancellation after await and owns application state cleanup.

Owner-thread model destruction requests cancellation, joins all host work, then
settles queued owner continuations while model state and injected services still
exist. This preserves already-committed Success even if the caller never polled.
Any new await during closing yields Cancelled synchronously without dispatching
more host work. Source control flow and helper expansion are statically bounded,
so closing cleanup cannot enqueue an unbounded operation chain. Remaining frames
are then destroyed. Retained Task handles own completed results only and cannot
resume retired instances or mutate a replacement model. Destroying a model on a
foreign thread terminates rather than racing an active frame; all ordinary model
access, task observation and polling reject a foreign owner explicitly.

Synchronous calls use one owned working State and a no-throw swap on successful
return. Nested mutating helpers share it. Async methods commit each completed
segment before suspension and refresh from current state on resumption, so
intervening edits are preserved. A failed segment rolls back that segment; it
does not rewind already committed source state or external I/O. General finally
or local Task composition is not claimed by this capability.
