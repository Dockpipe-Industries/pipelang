# Experimental C++ lexer port

Date: 2026-10-28. Scope: the next compiler slice after the native emitter. The
unchanged Go lexer and emitter remain the reference and default. The Go parser,
type checker, semantic identities and lowering remain authoritative. This does
not promote a complete C++ compiler, public native ABI or broader language.

## Owned boundaries

`src/native/lexer` implements the existing byte-level lexical contract in C++17.
It preserves tokens, decoded literals, half-open byte spans, EOF and lexical
failures, including the existing two-byte escape and ASCII identifier rules.
Sources are UTF-8 admitted by the existing Go SourceSet before tokenization.
The optional native profile accepts at most 65,536 source bytes per file.

The private request/response contracts are `pipelang.lexer.request.v1` and
`pipelang.lexer.response.v1`. The protocol refuses malformed, duplicate, missing,
case-aliased and unknown fields, noncanonical nullable outcomes, invalid scalar
strings and invalid offsets. Lexical source failures are explicit values;
protocol, identity and process faults are errors. No selected fault falls back.

`SourceLexer`, `LexicalResult` and `AnalyzeSemanticModuleSetWithLexer` provide an
explicit experimental value boundary. The native adapter supplies tokens to the
existing Go parser; ordinary entrypoints still call the unchanged Go lexer.
Compiler-owned admission checks token kinds, source bytes, spans, token count
and terminal EOF, and binds the existing SourceFile identity to diagnostics.
There is no environment/global lexer selection inside the compiler.

`internal/nativeprocess` owns the shared executable boundary for both native
components: exact SHA-256 before/after execution, 256-MiB executable identity
capacity, bounded output/diagnostic retention, timeout and cancellation. This
extracts the previous emitter process transport rather than duplicating it.
Emitter code generation and the reference compiler language rules are unchanged.

## Verification

The declared build/test workflows passed with installed DockPipe 0.6.33. A
recorded temporary source snapshot omits only the obsolete exact local CLI
execution gate. The real project config and release checksum lock are unchanged;
no DockPipe source repository was accessed or modified.

The existing verified containment lane passed:

- 330 targeted Go/native comparisons of complete tokens and lexical failures,
  plus all 113 existing `.pipe` fixtures within the native source bound.
- Pure compiler-boundary and strict response-decoder regressions, including
  malformed spans, early/missing EOF, duplicate/missing/null fields, scalar
  preservation, component faults and source admission before native invocation.
- 13 raw lexer protocol tests, including source/request/depth/node boundaries,
  invalid UTF-8, malformed input and expected lexical-failure responses.
- Fresh Go reference export with all 160 independent vectors and 16 invalid
  Core cases. The eight-program native lexer pipeline matched parsed AST,
  semantic declarations, Core, ordinary Go output, generated C++ and bindings.
- All 49 existing native pilot stages: Go/C++ applications, trace variants,
  compiler-generated assembly, fresh execution and malformed host inputs.
  One repetition checked 800 value outputs, including 320 trace outputs, and
  80 malformed-host refusals.

The native lexer pipeline writes fresh Core/C++/Go artifacts only after exact
reference comparison. Existing adapters, vectors and trace instrumentation are
copied from the freshly verified Go corpus with explicit byte provenance; they
are not described as newly generated instrumentation. The existing workers then
compile and execute these artifacts under the same memory/task/swap limits.
All accepted contained process trees were removed; no OOM or swap occurred.

## ForgePipe consumer proof

The source-bound installed ForgePipe 0.9 SDK passed its full local native
qualification. Its installed UI example now independently selects and checks
lexer and emitter executable identities at configure time and in binding
metadata. The all-Go and combined native lexer/emitter examples both passed
model CTest and real native X11/Vulkan synthetic-input proof. Their generated
C++ and adapter headers are byte-identical; their metadata only adds the
explicitly selected component identities. Both captures were inspected and show
the same readable declaration, count, name, toggle and action controls.

Lexer-only and emitter-only CLI selection also match the all-Go output. Four
lexical CLI diagnostic families are byte-identical to Go. Six missing/mismatched
component and binding-identity controls refuse without output or fallback.
Physical controller, Qt, other-platform and full-language qualification remain
outside this slice. No speed improvement is claimed.

## Reproduction and local artifacts

Set `PIPELANG_BUILD_CPP_LEXER=1` and `PIPELANG_BUILD_CPP_EMITTER=1` for the declared
build workflow. The frontend flags are `--cpp-lexer`, `--cpp-lexer-sha256`,
`--cpp-emitter` and `--cpp-emitter-sha256`; each selected component requires both
its absolute executable path and exact lowercase digest. Omitting selection
keeps that component in Go.

Native lexer tests use `PIPELANG_CPP_LEXER`; fixture comparisons additionally
select `PIPELANG_LEXER_FIXTURES`. The paired pipeline uses
`PIPELANG_LEXER_PILOT_CORPUS`, `PIPELANG_LEXER_PILOT_OUTPUT` and
`PIPELANG_CPP_EMITTER`. Run opt-in process/generation tests through the existing
`tests/containedexec/job.py` and `run.py` lane, not as unconstrained campaigns.

The installed ForgePipe example accepts `PIPELANG_CPP_LEXER` and
`PIPELANG_CPP_LEXER_SHA256` alongside the emitter CMake inputs. Its normal source
and SDK ownership boundaries are unchanged; no sibling engine source is linked.

Raw source snapshots, unit receipts, fresh corpora, native execution results,
CLI controls and captures are retained under `/tmp/pipelang-cpp-lexer/proof`.
The accepted build outputs are copied to the checkout's ignored `bin/` with a
separate receipt; previous emitter-slice artifacts are retained unchanged.
Changes are local and uncommitted; no publication or release was performed.

Final identities:

- Frontend SHA-256: `9c4aea4bc5c66765fe7d71ac3e9c2f3646c9f0f13469f148d7b34290b374ec89`.
- Native lexer SHA-256: `64345a57ba3c25deb2e6e3b2d34c0588bf770f51127ca09da29f7702901de66e`.
- Native emitter SHA-256: `9e34b5267a12204d449e1c12585bf77750d1a9f9b24cfa279c864c68d193148c`.
- ForgePipe SDK source digest: `337c86fc85a696cb637ea94cb5137e0c1869b6850a2348e090bf67ea530402ab`.
- ForgePipe SDK archive SHA-256: `76b28415007dae63dd68f755825155451dda84b24ebd32be009e9004ff859f08`.
