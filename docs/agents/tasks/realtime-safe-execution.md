# PipeLang Feature: Compiler-Enforced Real-Time Safe Execution

## Backlog status and activation

Recorded 2026-11-08. Status: **deferred; not implemented or qualified by this
record**. Begin investigation during the NAM Tone Deck port. This task does not
supersede the current PipeLang engine-first priority or activate compiler/runtime
implementation, consumer porting, SDK publication or benchmark campaigns now.

The requirements below preserve the submitted feature request. Annotation syntax,
safety levels, diagnostic identifiers and phase boundaries are proposals pending
the pre-implementation investigation, not accepted language contracts.

When activated, follow [architecture](../core/architecture.md),
[validation](../core/validation.md) and the [language direction](../backlog.md#language-direction).
General analysis and code generation belong in `src/native/model-compiler`;
runtime and installed SDK support belong in `sdk/pipeview/model`. Keep these
capabilities independent of NAM, Qt and ForgePipe implementation. Inspect the
then-current compiler/runtime and NAM callback before selecting the smallest useful
profile; do not assume a collector exists or that existing ownership, budgets,
atomic APIs or allocation-free native exports establish callback safety.

Retain a deferred status until that investigation produces a phased plan with
explicit supported operations, fail-closed call admission, runtime/interop trust
boundaries and bounded validation. Record static restrictions separately from
observed timing and any future timing proof. The full feature acceptance criteria
below remain pending even if an initial allocation-free slice is qualified.

## Submitted requirements

**Project:** PipeLang  
**Category:** Compiler / Runtime / Language Features  
**Priority:** Deferred — implement during NAM Tone Deck port  
**Initial validation:** NAM Tone Deck real-time audio processing  
**Future applications:** ForgePipe, audio plugins, DSP, embedded systems, industrial controllers

## Objective

Introduce first-class real-time-safe execution capabilities into PipeLang, allowing developers to declare functions and execution paths that must satisfy deterministic runtime constraints.

The primary goal is to support low-latency applications such as NAM Tone Deck without requiring the entire application to abandon convenient higher-level memory management.

Real-time safety should be enforced through compiler analysis wherever possible, rather than relying entirely on developer discipline or globally disabling garbage collection.

## 1. Real-Time Function Annotation

Investigate syntax conceptually equivalent to:

```csharp
[Realtime]
public void ProcessAudio(
    AudioBuffer input,
    AudioBuffer output)
{
    // Real-time-safe DSP processing
}
```

The exact syntax should follow existing PipeLang language conventions.

Functions marked as real-time must be statically analyzed for operations that could introduce unpredictable execution latency.

The compiler should reject known violations and report clear diagnostics.

## 2. Real-Time Safety Restrictions

Within a real-time execution context, prohibit or explicitly control:

- Heap allocations and dynamic object creation.
- Garbage collection or runtime-managed operations that could trigger collection.
- Blocking mutexes, waits, and synchronization primitives.
- Filesystem and network I/O.
- Thread creation and blocking thread coordination.
- Sleeping, yielding, or scheduling operations with unbounded latency.
- Exceptions requiring dynamic allocation or stack unwinding.
- Unbounded recursion.
- Calls to functions without verified real-time safety.
- Runtime reflection and dynamic loading.
- Hidden allocations introduced by compiler-generated code.

Inspect generated native code for hidden runtime operations.

Distinguish operations that are provably forbidden from operations whose timing cannot be statically bounded.

Do not claim that an operation is hard-real-time safe merely because it performs no allocation.

## 3. Transitive Call-Graph Analysis

Real-time safety must apply to the entire reachable call graph.

If a real-time function calls another function, that function must satisfy the same restrictions.

Example:

```csharp
[Realtime]
void ProcessAudio()
{
    ApplyGain();       // Allowed if verified
    LoadPreset();      // Compiler error: filesystem I/O
}
```

Support:

- Direct calls.
- Instance methods.
- Virtual dispatch.
- Interfaces.
- Function pointers and callbacks.
- Generic functions.
- Native interoperability boundaries.

For indirect calls, require sufficient evidence that every possible target satisfies the real-time contract.

If the compiler cannot verify safety, reject the call or require an explicit unsafe declaration with clearly documented limitations.

## 4. Memory Management Policies

Investigate function-level or execution-context-level memory policies.

Support separating real-time processing from managed application code.

### Real-time execution

- No garbage collection activity on the audio thread.
- No implicit heap allocations.
- Preallocated buffers and state.
- Stack-based storage where appropriate.
- Explicitly controlled memory lifetimes.
- Deterministic resource handling where verifiable.

### Non-real-time execution

- Normal memory management.
- Dynamic allocation.
- Model loading.
- Preset management.
- Filesystem access.
- UI updates.
- Background processing.

The system should allow safe communication between these execution contexts.

Disabling garbage collection globally must not be the only supported approach.

Investigate whether the existing PipeLang runtime and generated native code require explicit thread registration, safepoints, or other mechanisms that could interfere with real-time execution.

## 5. Safe Communication Between Threads

Provide or support appropriate real-time communication primitives.

Examples:

- Preallocated single-producer/single-consumer queues.
- Lock-free parameter updates.
- Atomic state exchange.
- Double-buffered configuration.
- Preallocated event queues.
- Safe transfer of prepared DSP state.

Do not assume every lock-free algorithm is automatically real-time safe.

Account for memory reclamation, atomic operation guarantees, and object lifetimes.

Real-time threads must never wait for UI or background threads to complete work.

## 6. Native C/C++ Interoperability

PipeLang must support calling native DSP and audio libraries without unnecessary overhead.

Investigate:

- Direct C ABI interoperability.
- Native C++ integration through supported bindings or wrappers.
- Stable buffer layouts.
- Pointer and span-like access.
- Zero-copy audio processing.
- SIMD-compatible data structures.
- Alignment requirements.
- Native callback interoperability.

Native functions must not automatically be considered real-time safe.

Introduce explicit native-function contracts or annotations where appropriate.

The compiler should distinguish:

1. Verified PipeLang real-time-safe functions.
2. Native functions with developer-asserted real-time contracts.
3. Functions with unknown safety properties.
4. Known unsafe functions.

Document that external native contracts cannot be fully proven by the PipeLang compiler.

## 7. NAM Tone Deck Integration

Use NAM Tone Deck as the initial real-world validation project.

The desired architecture separates audio processing from application management.

### Real-time audio thread

Responsible for:

- Processing audio buffers.
- Executing DSP algorithms.
- Running loaded NAM models.
- Applying real-time-safe parameter changes.
- Processing preallocated signal chains.
- Maintaining predictable callback execution.

### Background/application threads

Responsible for:

- Loading NAM models.
- Allocating and preparing DSP state.
- Managing presets.
- Reading and writing files.
- Updating the Qt UI.
- Performing expensive initialization.
- Preparing replacement processing graphs.

Model and preset changes should be prepared outside the audio callback and transferred using an appropriate real-time-safe mechanism.

Avoid destroying large DSP objects or reclaiming memory synchronously inside the audio callback.

## 8. Compiler Diagnostics

Provide actionable errors when real-time restrictions are violated.

Example:

```text
PL-RT001: Heap allocation prohibited in real-time context.

Function: ProcessAudio
Call chain:
  ProcessAudio
    -> ApplyEffects
      -> CreateTemporaryBuffer

Suggestion:
Preallocate the buffer during initialization.
```

Diagnostics should identify the offending operation and relevant call chain.

Where possible, provide alternatives that preserve real-time safety.

## 9. Real-Time Safety Levels

Investigate whether multiple levels of guarantees would be useful.

For example:

**Allocation-free**
- No heap allocation.
- No garbage collection triggered by the execution path.

**Non-blocking**
- Allocation-free requirements.
- No known blocking operations.
- No unverified blocking calls.

**Real-time constrained**
- Non-blocking requirements.
- Verified call-graph restrictions.
- Controlled memory ownership.
- Restricted runtime interactions.

Avoid claiming mathematically bounded execution time without an appropriate timing model and proof.

For initial implementation, prioritize a practical, compiler-enforced audio-callback safety profile.

## 10. Performance and Validation

Create a dedicated validation suite using NAM Tone Deck.

Measure:

- Audio callback execution time.
- Worst observed callback latency.
- Allocation count inside callbacks.
- Garbage collection activity.
- Audio underruns and dropouts.
- Memory usage.
- CPU utilization.
- Native interoperability overhead.
- Behaviour during concurrent UI and model-loading activity.

Test multiple sample rates and buffer sizes.

For example:

- 44.1 kHz.
- 48 kHz.
- 96 kHz.
- 32, 64, 128, and 256-frame buffers.

Compare against the existing C++ implementation where practical.

Use realistic NAM models and effect chains.

Include stress tests involving model switching, preset changes, UI interaction, and background I/O.

Runtime measurements complement static verification but do not establish hard real-time guarantees.

## 11. Implementation Strategy

Before implementation:

1. Inspect the current PipeLang C++ compiler architecture.
2. Identify the current memory-management model and runtime dependencies.
3. Review function annotations, attributes, and type-system capabilities.
4. Inspect generated native code and compiler-inserted runtime calls.
5. Identify existing interop mechanisms.
6. Review NAM Tone Deck's current audio callback and DSP architecture.
7. Determine the smallest useful compiler-enforced safety profile.
8. Produce a phased implementation plan.

### Phase 1 — Allocation-Free Functions

Implement real-time annotations and transitive allocation analysis.

Reject known allocations and unsupported calls.

### Phase 2 — Non-Blocking Execution

Extend analysis to blocking operations, synchronization, I/O, and runtime interactions.

### Phase 3 — Native Interoperability Contracts

Introduce explicit safety declarations for native functions and callbacks.

### Phase 4 — NAM Tone Deck Port

Port the relevant audio processing paths and validate against the existing implementation.

### Phase 5 — Advanced Real-Time Capabilities

Investigate stronger ownership guarantees, memory reclamation, bounded execution analysis, and reusable real-time libraries.

## 12. Acceptance Criteria

- PipeLang supports declaring an allocation-free or real-time-constrained function.
- The compiler rejects direct heap allocations in restricted functions.
- Restrictions propagate through the reachable call graph.
- Hidden compiler-generated allocations are detected or prevented.
- Unknown native calls cannot silently bypass safety checks.
- Audio processing can run without GC pauses on its execution thread.
- DSP buffers and processing state can be prepared outside the callback.
- Real-time-safe communication between application and audio threads is supported.
- NAM Tone Deck can process audio through PipeLang without callback allocations or known blocking operations.
- Automated tests validate compiler diagnostics and generated-code behaviour.
- Benchmarking identifies any significant performance regressions compared with the existing C++ implementation.
- Documentation clearly distinguishes compiler-enforced restrictions from timing guarantees that cannot be proven.

## Design Principle

**Real-time safety should be a property the PipeLang compiler can help enforce, not merely a convention developers are expected to follow.**

Developers should be able to use high-level PipeLang features throughout an application while explicitly designating performance-critical execution paths that require stronger guarantees.

The first implementation should prioritize practical low-latency audio processing for NAM Tone Deck, with an architecture that can later support game engines, embedded systems, and industrial workloads.

Do not overengineer hard-real-time guarantees before establishing a reliable allocation-free, non-blocking execution model.
