# Native cryptography capability — ABI 1 candidate

This module is a small interoperability boundary over maintained cryptographic
libraries. It does not implement cryptographic algorithms or a TLS stack. The
current concrete consumer is PipeServe's verified-certificate fingerprint.
Only SHA-256 is exposed, using RustCrypto `sha2 = 0.10.9` and the committed Cargo.lock.
Randomness, signatures, password hashing and authenticated encryption remain
unimplemented until separately selected consumers require them. rustls uses AWS-LC
for its own TLS cryptography independently of this module. OpenSSL remains a
separately selected PipeServe backend; no automatic fallback occurs.

`include/pipelang_crypto.h` describes the versioned C ABI:

- `pipelang_crypto_abi_version()` returns 1.
- `pipelang_crypto_v1_sha256(input,length,output,output_length)` hashes up to 1 MiB
  into exactly 32 caller-owned bytes. Empty input may be null; output may not.
- Return codes: 0 Success, 1 invalid pointer/length, 2 input-size limit, 3 caught Rust
  panic. No allocation ownership or borrowed pointer survives the call.
- Input/output may overlap. All nonempty input and output addresses must reference
  the documented valid extents. Pointer checks cannot prove arbitrary foreign
  memory is valid; this is a trusted native caller contract, not a memory sandbox.

The wrapper catches ordinary Rust unwinding panics at the C ABI. Fatal process
conditions such as allocator aborts are not recoverable errors. Ordinary PipeLang
programs receive owned values through injected host interfaces and never receive
raw native pointers. General `ByteBuffer` semantics live in the language runtime,
not in this cryptographic adapter. HTTP framing, application permissions and content
processing do not belong in this module.

Build with the exact toolchain recorded by the candidate receipt (Rust/Cargo 1.95.0;
manifest minimum 1.85), prefetching locked dependencies separately if absent. Then
run the declared `workflows/crypto.build/config.yml` with an explicit admitted
DockPipe binary, `PIPELANG_CRYPTO_SDK_PREFIX`, installed
`PIPELANG_NATIVE_RESOLVER_PREFIX`, explicit `PIPELANG_NATIVE_TOOLCHAIN` and its
SHA256, a prepared `PIPELANG_NATIVE_CACHE`, and fresh `PIPELANG_CRYPTO_PROOF`.
The module CMake target uses the installed PipeLang native-dependency resolver;
the workflow no longer invokes Cargo directly.
The workflow verifies the native containment runner and uses `--frozen --offline`,
one build worker and unchanged aggregate/workload resource caps. It does not install
Rust, fetch dependencies during the build, or alter compiler/consumer release pins.
The core C++ compiler has no mandatory Rust dependency from this optional module.

Qualification includes published SHA-256 empty/abc vectors, overlapping buffers,
invalid extents, null arguments and unchanged-output failures. A deterministic
4,096-input native-boundary fuzz fixture runs with C++ ASan/UBSan and output canaries.
Rust unit tests execute the library itself; C++ instrumentation does not establish
sanitizer coverage of Rust or all transitive library internals. This is bounded
Linux x86-64 evidence, not ARM64/NAS/cross-platform or production certification.

Preserve this module's Apache-2.0 notice and all linked dependency notices. Candidate
packaging includes exact Cargo.lock, library/header digests and dependency licenses.
Security updates require a reviewed dependency/lock update, rebuilding the library,
re-running ABI and hostile-input tests, then updating all consumer artifact digests.
No release, publication, broad plugin framework or standalone crypto service is added.

## Additive stream ABI2 — source implementation, qualification pending

`include/pipelang_crypto_stream.h` declares independent versioned stream symbols.
`pipelang_crypto_stream_abi_version()` returns2. ABI1's version remains1 and its
whole-buffer function/header, limits, error values and behavior are preserved.
The same maintained sha2 dependency and exact Cargo.lock are used; there is no
new cryptographic implementation, dependency, core SDK selection or default.

Create a native owner, begin one or more contexts with explicit maximum byte
counts, update with chunks <=1MiB, finish into32 caller-owned bytes plus the exact
byte count, close each context, then close the owner. The registry has16 fixed
owner slots and64 fixed context slots shared across this linked provider instance.
Contexts contain maintained Sha256 state directly, not allocated foreign pointers.
All methods serialize through one mutex; there are no callbacks/background tasks.
Generation IDs are positive <=INT64_MAX and never reused. The future source host
adapter keeps its owner ID private and exposes only validated context IDs.
These IDs enforce lifecycle ownership; they are not tenant authorization against
native callers who deliberately copy another owner's complete capability.

Maximum bytes may be0 for an empty-only stream and may not exceed UINT64_MAX/8.
Every update checks addition and the running maximum before hashing; this admits
512MiB file digests without changing the1MiB chunk or ABI1 buffer limits. A running
limit failure poisons its context: Finish cannot return a prefix digest as success.
Invalid argument/chunk-size refusal consumes nothing and leaves state unchanged.
Finish seals, retaining its bounded slot until Close; repeated Finish/Update fails.
Close releases active, failed or finalized state. Immediate repeated close succeeds
only while that slot's tombstone has not been reused. Owner close releases all its
contexts; a closed/reused owner or foreign/stale context is refused. Panic handling
invalidates all retained owners/contexts and permits a fresh owner without keeping
a poisoned mutex. Identifier exhaustion fails before wrap. No unbounded history,
source-visible raw pointer, allocator ownership transfer or retained borrowed
input/output is introduced. Caller pointers remain the explicit trusted native
contract; pointer extent/alignment cannot be validated by this ABI.

This native primitive has no CancellationToken ABI or scheduling. A future source
adapter checks cancellation before/after each bounded Update and closes affected
state before returning Cancelled, preventing retries from hashing a consumed chunk
twice. It preserves close/cleanup failures independently when required. No source
adapter, CNG backend, file capture, output writer or policy runner is implemented
by this slice. Existing Linux-only dependency/toolchain qualification still applies.

Pending Rust tests exercise published empty/abc/56-byte/million-a vectors, every
split of short vectors, exact1MiB update and invalid arguments, poisoned running
bounds, unchanged failure outputs, count arithmetic at overflow/capacity, owner/
context limits, stale/reused/cross-owner handles, sealed-slot lifecycle, cleanup,
identifier exhaustion and caught-panic recovery. A constant-memory512MiB stream
checks byte count and agreement with a separately driven maintained sha2 context;
that latter check is a boundary/segmentation witness, not an independent512MiB
cryptographic oracle. Existing ABI1 tests remain unchanged. Parent schedules all
Rust/native/link/consumer proof serially; source-only is not acceptance, and C++
ASan/UBSan alone does not instrument Rust internals.

The existing crypto install includes both headers; its SHA256SUMS inventory adds
the new header beside the archive, ABI1 header and lock. No existing consumer pins,
lockfiles, toolchain manifests or core SDK package files are changed. Rebuild and
repin a qualified additive artifact explicitly before any consumer uses new symbols.
