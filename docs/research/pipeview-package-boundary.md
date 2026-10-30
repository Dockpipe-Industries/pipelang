# PipeView first package-boundary slice

Status: locally implemented and verified, 2026-10-28. This is the bounded first
PV-01 foundation, not full package-system or PipeView acceptance. Go remains the
reference/default compiler. No language contract, Core/Application IR schema,
parser grammar, frozen fixture, native emitter or native lexer implementation
changed in this slice.

## Implemented ownership

The [experimental SDK](../../sdk/pipeview/README.md) contains two real Go packages:

- `sdk/pipeview`: typed bool/int64/string state declarations, typed UI events,
  portable components, validation and immutable inspection snapshots. Its entire
  dependency closure is standard library plus itself.
- `sdk/pipeview/forgepipe`: explicit capability admission and deterministic
  generation of the existing `forgepipe.ui.v1` transport. Its non-standard
  dependency closure is just these two SDK packages. No renderer or engine
  implementation is copied into PipeLang.

They live in the existing PipeLang Go module. No new resolver, registry, module
replacement, mandatory runtime or empty future package is introduced. The
headless materializer does not depend on either UI package. Backend-neutral UI
contracts are a companion declaration model; the existing Application IR remains
the authoritative Core-backed snapshot/section projection and is unchanged.
These are experimental host SDK packages, not implemented authored PipeLang
imports or the final public UI namespace. Compatibility/distribution of published
packages and the foundation prerequisites remain on PV-01's backlog.

The optional `examples/pipeview-forgepipe` build tool uses typed constructors to
generate `PipeViewDocument.hpp`. The ForgePipe example's `Counter.pipe` now only
contains `CanIncrement`, `Increment` and `Reset`; escaped author-written view JSON
has been removed. The view is currently authored in the Go SDK example. Proposed
PipeLang component syntax, XML, styles, reactive state/actions, Qt/QML bridge and
hot reload are not implemented by this slice.

UI events are typed host wiring requirements, not canonical callable identities
or effect capabilities. The ForgePipe example's compiler-facing bridge checks
full canonical method identities and exact signatures against the existing
compiler export metadata. It also checks source/header/compiler/component hashes,
export/language/compiler profiles, unique functions/symbols and duplicate JSON
keys. CMake pins the separately selected generator, and the build-time adapter
checks its hash again before execution. No handwritten `.pipe` interpretation
exists in the adapter.

## Verification

Evidence root: `/tmp/pipeview-first`. Source snapshots and original-file hashes
preserve the inherited uncommitted emitter/lexer work and ForgePipe changes.

- Installed `/usr/bin/dockpipe` 0.6.33 validated both PipeLang workflow declarations
  and ran the existing build/test workflows with the new package checks.
- PipeLang's committed config still pins an older staging CLI. Only the temporary
  proof snapshot omits that obsolete exact-version gate; the owning config and
  release lock are unchanged. `source-receipt.json` records the exception and all
  source hashes. DockPipe's repository was neither inspected nor modified.
- The first worker preparation selected an older Go executable and failed before
  acceptance. Explicitly selecting the cached Go 1.25.0 fixed that issue. The
  ordinary compiler baseline then hit its bounded timeout under memory reclaim.
  The accepted test workflow used explicit `GOGC=20`, with the same code, compiler
  and containment limits; it completed in about 12.3 seconds, with a sampled
  compiler peak of 699184 KiB, no swap/OOM and verified process-tree removal.
  Failed preparations remain in `packages-job*` and `verified-baseline-test*`.
- `bounded-gc-baseline-test.json` records CLI/materializer, both new SDK packages,
  compiler transport, frozen compatibility and the workflow's focused compiler
  tests. This is baseline proof, not the complete generated conformance campaign
  or a performance qualification.
- `package-contracts.json` independently proves dependency isolation and compiles
  a headless SDK consumer. Three invalid public Go consumers are refused: a
  string-bound Toggle, a string change event on Toggle, and an unsupported float
  state. No UI dependency is added to the headless materializer.
- SDK tests independently check inspection/event contents, ownership isolation,
  zero/missing/duplicate/conflicting declarations, UTF-8, depth/node/text bounds,
  exact int64 extrema, deterministic transport and adapter refusals. Core permits
  128-byte identities; ForgePipe refuses its narrower 64-byte limit. The adapter
  also refuses transports beyond the installed decoder's 65536-byte capacity.
- Both Go-reference and explicitly selected native lexer/emitter builds consumed
  the same qualified installed ForgePipe SDK 0.9.0. The archive SHA-256 is
  `76b28415007dae63dd68f755825155451dda84b24ebd32be009e9004ff859f08`.
  All 159 expanded regular files and symlinks were checked against that archive;
  no sibling engine source was compiled. The candidate example is an external
  consumer snapshot, not a newly published SDK payload.
- Both builds passed the model interaction test and 11 Python binding-admission
  tests, including changed generator refusal. They produced identical
  `generated.hpp`, `DemoBindings.hpp` and `PipeViewDocument.hpp` bytes. The export
  contains only the three expected action functions. The compiler executable hash
  remains the accepted lexer slice's `9c4aea4bc5c66765fe7d71ac3e9c2f3646c9f0f13469f148d7b34290b374ec89`.
- Both native Vulkan runs completed eight visible GPU frames and synthetic input
  checks. Both screenshots were inspected: the count, rider text, toggle and
  Increment/Reset controls remain visible and consistent. Native dynamic linkage
  contains no Qt libraries. `ui-result.json` retains all tool/artifact identities;
  `installed-ui.json` records accepted containment and cleanup.

Native captures: `ui-go/native/native-ui.png` and
`ui-cpp/native/native-ui.png`, under the evidence root. The model checks pointer,
controller navigation, text input, disabled updates, reset and atomic checked
arithmetic overflow refusal. They do not qualify physical input/controller
hardware, Qt/QML, accessibility, IME/shaping or additional platforms.

## Remaining boundaries

ForgePipe Session still applies toggle/text edits before emitting events. Neither
this SDK nor the example claims the future checked-action mutation contract.
Inspection snapshots describe declarations and initial values; they are not the
live AI protocol or an authorization surface. No production inspection endpoint
or reload server is installed. Arbitrary UTF-8 survives the transport tests, but
the existing native basic-font renderer retains its printable-ASCII profile.

Future slices must preserve the shared declarations and keep markup, styling,
compiler admission, tooling and renderers with their own owners. The next useful
work is to resolve authored package/import and view-frontend prerequisites, then
build a typed settings screen through the accepted compiler pipeline. Async
settings, state migration and native code reload retain their separate acceptance
requirements in the [roadmap](../agents/backlog.md#pipeview-modular-typed-ui-roadmap).

No commit, push, publication, system installation or DockPipe repository change
is part of this slice. PipeLang stays open source; ForgePipe and MesaMoto licensing
and inherited notices are unchanged.
