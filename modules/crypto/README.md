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
