# Experimental C++ emitter port

Date: 2026-10-28. Scope: port the existing bounded emitter, retain Go as the
reference, and demonstrate the same compiled ForgePipe UI. No default-backend,
full-language, public ABI, cross-platform or performance promotion follows.

## Implementation boundary

`src/native/cpp-emitter` owns C++17 code generation, deterministic symbol and record
names, and a private admitted-Core JSON codec. The existing runtime support header
remains owned by `src/lib/pipelang/cppbackend/runtime.hpp` and is embedded from that
file at build time. The nlohmann/json 3.12.0 header is independently vendored with
its MIT license and byte provenance; its build checksum is enforced. No engine
implementation or sibling source dependency is introduced.

The Go parser, analysis, HIR/Core lowering and authoritative Core/capability
validation remain unchanged. `cppbackend.NativeEmitter` is an explicitly selected
process adapter with executable checksums before/after execution, bounded request,
response and diagnostic retention, cancellation and timeout handling. The frontend
adds `--cpp-emitter` and `--cpp-emitter-sha256`; omitting them retains Go. Selection,
identity, unsupported-input or process failure never falls back silently.

The wire contracts are `pipelang.cpp-emitter.request.v1` and
`pipelang.cpp-emitter.response.v1`. They are internal to this experimental port,
not a replacement public SDK. The native process owns framing, duplicate-key,
structure, capability and resource refusals. Complete semantic admission remains
with Go before invocation. Generated metadata retains the frontend identity and
adds the selected emitter identity.

## Verification

The existing eight-program pilot corpus compares every generated source byte and
semantic binding against Go before using native output. It retains all 160
independent frontend/HIR/Core/evaluator/oracle vectors and 16 invalid Core cases.
The existing contained workers completed 49 stages: Go and C++ application and
trace builds, compiler-generated assembly builds, fresh executions and malformed
host fixtures. One repetition checked 800 value outputs, including 320 trace
outputs, and 80 malformed-host refusals. Twelve additional raw-process protocol
tests passed, including duplicate keys, malformed/trailing input, closed types
and operations, missing references and byte/depth/node limits.

The Go-reference and native-emitter versions of the installed ForgePipe 0.9 SDK
UI example both passed their model CTest and actual native Vulkan synthetic-input
proof. Generated C++ and binding-adapter headers are byte-identical; metadata
only differs by the additional selected emitter identity. Both native captures
were inspected: title, declaration, count, editable name, enabled state and
Increment/Reset controls are readable and match. This demonstrates the existing
compiled scalar/document UI profile, not richer compile-time widget semantics.

Validation used GCC 11.4/C++17 for the emitter and generated native programs, the
existing cached Go 1.25.0 reference toolchain, installed DockPipe 0.6.33, and the
native Linux X11/Vulkan desktop. The SDK uses its exact pinned shader compiler
and Vulkan headers. No Qt adapter, physical controller, other-platform or speed
claim is made. Go remains the default/reference implementation.

## Reproduction and retained evidence

Enable `PIPELANG_BUILD_CPP_EMITTER=1` for the declared root build workflow. The
existing native pilot exporter accepts `PIPELANG_CPP_EMITTER` and
`PIPELANG_CPP_EMITTER_SHA256`; its default Go export layout remains unchanged.
The existing driver accepts `--emitter`, `--emitter-sha256` and `--core-only` for
this lane. The last switch skips optional Qt dependency discovery, not Core,
value, trace, assembly or malformed-host checks. Use the existing `job.py` and
`run.py` containment; memory/task/swap limits are unchanged.

ForgePipe's installed example accepts `PIPELANG_CPP_EMITTER` and
`PIPELANG_CPP_EMITTER_SHA256` CMake inputs alongside its separately pinned
frontend. Its binding adapter checks that the metadata identifies the selected
emitter. The development producer no longer imposes a redundant CLI-version
equality check. Consumer minimum-version policy remains separate DockPipe work.

Local raw artifacts are retained under `/tmp/pipelang-cpp-port/proof`: source
snapshot receipts, build/test outputs, the compared corpus, protocol receipts,
`native-execution-2/result.json`, UI results and native captures. The source
checkout's old exact CLI config and release lock are unchanged. Local workflow
runs use recorded temporary copies with only that obsolete execution gate
omitted and the installed 0.6.33 executable identity recorded.

Failed preparations are retained, not promoted into passing evidence: an older
Go launcher under offline containment, a cold alternate-toolchain build that hit
its time bound, and a temporary coordinator cleanup API mistake. The accepted
comparison directly selects the same cached Go toolchain used by baseline tests.
All accepted contained process trees were removed with zero swap and no OOM.
No files were committed, pushed or published. The DockPipe source repository was
not used or changed for this port.


## Final local artifact identities

The final qualified SDK source digest is `78e858eb881879caae1fd5fea52ea2ca5df554cadc0fea2a5ec8cf1b3883ab43`; its archive SHA-256 is
`0c91b5fd9330b1298b01cd67cd0842612dd697c4ba00d702af225fdc46497585`. The frontend executable SHA-256 is `b6618ecb7e79b8e7b926f2d40008301bcf0c3134ffc9f5f8ebf5eef38229a0f9` and the native
emitter executable SHA-256 is `9e34b5267a12204d449e1c12585bf77750d1a9f9b24cfa279c864c68d193148c`. The paired UI generated-header digest
is `d64fc2607957cfe199938e8665342b11f7ec3e2108d05b1f2898ab3f8ac6aefd`.

`ui-final-result.json` records both installed-example runs and three refusal
controls: wrong emitter checksum, missing emitter checksum, and mismatched
binding metadata. None publishes output or falls back. `final-export-job.json`
records that the final source re-export matches the executed corpus and that
the default Go export layout is preserved. Final native captures are under
`ui-final-go/native/` and `ui-final-cpp/native/`. The accepted binaries are copied
unchanged to the source checkout's ignored `bin/` directory for local use.
