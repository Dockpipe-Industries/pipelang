# Experimental C++ PipeView syntax parser companion

## Admitted boundary

The bounded PipeView grammar is one complete parser responsibility. The default
`ParseViewSource(file, lexer)` remains the Go reference. An additive
`ParseViewSourceWithParser(file, lexer, parser)` hook admits the same source and
selected lexer result first, then supplies an already admitted token list to the
selected `ViewSourceParser`. A nil parser selects Go. Parser component errors
return without fallback; source syntax failures retain code, message and span.

`ViewSyntaxResult` selects exactly one declaration or `ViewSyntaxFailure`.
Admission belongs to Go: known failure codes, bounded diagnostic strings, file
identity, canonical token-boundary spans, AST depth/node/declaration limits,
ordered contained child extents, known value/action unions and coherent values.
SDK component/type/action checks remain authoritative after syntax admission.
Native parsing does not own source admission, lexing or SDK semantics.

The private JSON contracts are `pipelang.view-parser.request.v1` and
`pipelang.view-parser.response.v1`. The request contains exact `contract`,
`file`, `source_bytes`, `tokens` fields. Tokens use the existing admitted lexical
value representation. The response contains exact `contract`, `declaration`,
`failure` fields, with precisely one non-null outcome. Declaration members use
the Go syntax type member names; spans include the admitted file identity.
Every member is required, arrays are arrays (including empty arrays), unknown,
duplicate, case-alias and null scalar members are errors. Integer fields require
integer JSON numbers within their owning Go range. Strings preserve Unicode
scalar values. Request/response bytes, nesting and object counts are bounded.

`nativeviewparser.Parser{Path, SHA256, Context}.Parse` uses the existing
`internal/nativeprocess` checksum and cancellation boundary: explicit absolute
executable identity, checksum verification before/after execution, bounded
output and 30-second component deadline. CLI selection uses paired
`--cpp-view-parser` and `--cpp-view-parser-sha256`; completion metadata records
`view_parser_executable_sha256`. No PATH, environment or automatic selection.

For this slice XML input and XML output explicitly refuse selected native
parsing. The markup frontend currently reparses canonical source for span
mapping. Extending that boundary safely is separate; refusal avoids running an
unselected Go syntax parser inside a selected-native compilation. PipeLang
source with header or canonical PipeLang output uses the selected parser for
every parse. Existing XML behavior is unchanged when no parser is selected.

## Implementation and selection

The complete bounded grammar is implemented by `src/native/view-parser`:
state, event and closed typed action declarations; ordered elements/properties;
text, boolean and signed 64-bit values; references and one-member bindings;
original declaration, depth, element and source limits. The existing pinned
nlohmann JSON dependency is reused without changing its bytes or notices.

`src/lib/pipelang/view_parser.go` owns foreign syntax admission. It correlates
names, values, punctuation and ordered extents against the admitted tokens, checks
exclusive unions and resource limits, and never constructs a replacement tree or
calls the reference parser. Empty wire arrays remain mandatory and decode to the
same empty Go slice representation as the reference tree. The failure message
bound is source size plus 128 bytes because a quoted invalid identifier may
occupy nearly the entire admitted source.

The optional SDK adds `CompileWithParser` and `CompileProgramWithParser`;
existing entrypoints retain Go defaults. Both v1 events and v2 typed model/action
plans pass through the same SDK semantic checks. The CLI uses the selected parser
for header generation and every canonical PipeLang formatting parse. XML input
or output with selection is refused before output writes. Completion receipts
add `view_parser_executable_sha256` only when that component is selected.

Set `PIPELANG_BUILD_CPP_VIEW_PARSER=1` on the declared build workflow to build
`pipelang-cpp-view-parser`. `PIPELANG_CPP_VIEW_PARSER` is a test-only opt-in for
contained subprocess tests; production selection is exclusively through the
explicit API or paired CLI flags. Source and request strings are bounded to
64 KiB; messages are bounded to 16 MiB. The request decoder limits nesting and
node count before admitting tokens. Response decoding enforces exact required
members, scalar types and signed integer ranges, Unicode scalar preservation,
exclusive outcomes, and nesting/node/byte limits before AST admission.

## Focused proof, 2026-10-30

Local evidence is retained at
`/tmp/pipelang-forgepipe-overnight-20261030/native-view-parser/` (temporary,
not a durable release receipt). `native-build-2.json` and
`native-build-job-2.json` record the clean C++ build. `admission-3.json`,
`focused-3.json` and `final-checks-job.json` record final focused checks, with
exit zero, verified process-tree removal, zero swap and no OOM events.
The native executable SHA-256 is
`25beca51c3f5e7ef94401d8757a46ac3c18180187d5767c47f0e2054dbed206b`.
The proof directory's `acceptance.json` binds authored source hashes and receipts.

Focused coverage includes:

- Exact Go/native declaration and every span, plus syntax diagnostic
  code/message/category/span parity across v1/v2 grammar, token truncations and
  deletions, UTF-8 text, signed integer extrema/overflow, source sizes around
  64 KiB, declaration/node limits around 128 and depth around eight.
- Strict request/response refusals for duplicate, unknown, missing and case-alias
  members; null/type/range violations; malformed Unicode; trailing/deep/oversized
  messages; bad token extents/EOF; and invalid AST values, ownership or unions.
- Selected executable hash mismatch, cancellation, process failure, malformed or
  foreign responses, and no partial tree/fallback. The shared native-process
  test verifies bounded output draining. The adapter reuses the existing
  before/after checksum and 30-second deadline implementation; this focused run
  does not claim an independent 30-second timeout measurement.
- Combined native lexer/parser parity, unchanged SDK checked documents and typed
  model plans, byte-identical generated headers and canonical source, selected
  parser receipt identity, paired CLI selectors and explicit XML refusal.

The initial combined build exposed an older Go on the containment service PATH;
subsequent checks use the explicit installed Go 1.25.13 toolchain. Two large
compiler-package test builds reached the existing proactive 800 MiB stop and
cleaned up. The final compiler test build uses the already admitted `GOGC=20`
build setting in a fresh unit; cgroup limits, process deadlines and swap policy
remain unchanged. These are focused correctness receipts, not isolated compiler
performance measurements or full conformance coverage.

Go remains reference/default. This is the complete bounded view syntax
responsibility, not the general class parser, SDK semantic port, engine port,
UI interaction, platform/release qualification or a full native compilation
claim. Installed ForgePipe consumer integration has its own owning evidence.
