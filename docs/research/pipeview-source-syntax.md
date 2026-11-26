# PipeView source-syntax slice

Status: locally implemented and verified, 2026-10-28. This advances PV-02 with an
explicit experimental compiler entrypoint and a small settings consumer. Full
PV-01/PV-02, ordinary language/module integration and the richer settings model
remain pending. Go remains the compiler/parser/reference implementation.

## Changes and ownership

The [source profile](../concepts/pipeview-source.md) specifies the supported
syntax and limits. It accepts single-file `view` declarations, typed initial
scalar state, typed UI events, nested component blocks, named properties and
checked `bind`/event references. Ordinary class/module parsing is unchanged.

- `src/lib/pipelang/view_syntax.go` adds generic compiler AST/spans and an explicit
  `ParseViewSource` entrypoint over the existing lexer value boundary. It imports
  no PipeView SDK or renderer. Both default Go and pinned native tokenization
  feed the same parser. No second lexer is introduced.
- `sdk/pipeview/source` performs component/property/type/reference checks and
  lowers to the existing neutral declaration contract. It imports the compiler
  and neutral SDK, without renderer, engine or CLI code. Invalid programs never
  reach the transport adapter.
- `pipelang-viewc` selects the initial ForgePipe output profile and exports a
  header plus hash-bound completion metadata. All transport bytes are escaped
  individually, preserving text independently of C++ source encoding. The optional
  authoring-tool build flag now builds this CLI as well as the original Go SDK
  example; headless build defaults stay unchanged.
- ForgePipe's counter view now lives in `examples/pipelang-ui/View.pipe`, replacing
  the Go build-tool declaration for that consumer. Existing actions still compile
  from `Counter.pipe`; the bridge preserves full identity/signature admission.
- New `examples/pipelang-settings` consumes a `SettingsView.pipe` declaration for
  player name, VSync and Revert. Its `Settings.pipe` default functions compile
  through the real semantic/HIR/Core/C++ pipeline. The bridge calls them when the
  Revert event is dispatched. It reuses the counter's compiler admission helper.

Existing root compiler semantics, Core/Application IR schemas, accepted language
contracts, frozen goldens, lexer and C++ emitter implementations remain unchanged.
There is no registry/resolver addition or Qt dependency. New settings example
sources are not part of a newly assembled or released SDK payload.

## Verification and artifacts

Evidence root: `/tmp/pipeview-syntax`. `source-receipt.json` captures owning source
hashes and the compiler snapshot; `forge-original-hashes.json` and
`forge-source-receipt.json` identify original and candidate consumer files.

The installed `/usr/bin/dockpipe` is 0.6.33. It validated and ran both declared
PipeLang baseline workflows in contained workers. The temporary snapshot omits
the obsolete exact staging-CLI gate only; the owning `dockpipe.config.json` and
release lock retain their original bytes. DockPipe's repository was neither
inspected nor changed. The selected Go 1.25.0 toolchain and explicit `GOGC=20`
keep the same accepted resource limits used for the previous slice.

- `baseline-build.json` and `baseline-test.json`: accepted workflows, SDK/source
  checker/CLI tests, existing CLI/materializer/compiler transport tests, focused
  compiler tests and frozen compatibility. This is not the complete generated
  compiler campaign or a performance qualification.
- Parser tests assert independent syntax trees, source spans, signed int64
  extrema, source/node/depth/declaration limits, syntax/lexical/UTF-8 diagnostics,
  admission before foreign execution and fail-closed foreign token/component
  behavior. Generic component names stay syntactically inert until SDK checking.
- SDK source tests assert the expected settings declaration, all scalar types,
  shared references, exact located semantic refusals and document constraints.
  CLI tests assert deterministic artifacts/receipts, byte escapes, invalid input
  refusing without output, source/output collisions and destination preservation.
- `native-source.json`: 49 sources independently compare Go/native-token ASTs,
  source diagnostics, SDK admission and successful projected bytes. Go remains
  the reference on both paths. Invalid cases include type/reference failures,
  overflow, malformed text, unsupported expressions and capacity errors.
- `source-dependencies.json`: neutral core stays standard-library-only; transport
  adds only the neutral SDK; the source checker adds only the existing compiler
  closure and neutral SDK. Neither compiler nor headless materializer depends on
  UI packages. Existing public Go type-refusal cases still pass.
- `installed-ui.json` and `ui-result.json`: counter and settings each build twice,
  with default Go components and explicitly selected native lexer/emitter. All
  four builds pass model interaction tests and binding admission. Counter has
  12 Python admission tests; settings has five. These include view metadata,
  expected source/tool/hash/profile identities and changed generator refusal.
- Each paired build emits identical `generated.hpp`, `DemoBindings.hpp` and
  `PipeViewDocument.hpp` bytes. View metadata differs only in explicit lexer
  identity; action metadata identifies the selected native components.
- All four native Vulkan runs render eight visible frames and exercise synthetic
  input. Settings checks name edit, VSync toggle and compiled-default Revert;
  the model additionally refuses an unknown event. Counter retains existing
  pointer/controller/text/reset and checked arithmetic overflow coverage. Native
  dynamic linkage has no Qt library. Both settings captures were inspected and
  show the same usable title, player name, toggle and Revert control.

The qualified installed ForgePipe SDK is 0.9.0, source identity
`337c86fc85a696cb637ea94cb5137e0c1869b6850a2348e090bf67ea530402ab`, archive SHA-256
`76b28415007dae63dd68f755825155451dda84b24ebd32be009e9004ff859f08`. The archive and
all 159 expanded regular files were rechecked. Consumers compile only example
sources against installed CMake targets; no sibling engine implementation is
compiled. Tool hashes and generated artifacts are retained in `ui-result.json`.
Containment receipts require no swap/OOM and complete process-tree removal.

Settings captures: `settings-go/native/native-settings.png` and
`settings-cpp/native/native-settings.png`. Counter captures remain under
`ui-go/native/native-ui.png` and `ui-cpp/native/native-ui.png` in the evidence root.

## Limits and continuation

`state`/`event` are declaration-profile constructs, not general mutable state or
language effects. ForgePipe's existing Session still applies text/toggle edits
before dispatch. Revert is host wiring to admitted compiled functions. VSync is
UI state only; there is no renderer-setting integration, persistence or async
Save. No new physical input, Qt/QML, accessibility, IME, platform, memory-management
or performance qualification is claimed. The native basic font remains ASCII.

General package imports, richer component/model syntax, enums/checked numeric
controls, computed/dirty/validation state, typed fallible actions/cancellation,
styles, XML and transactional reload remain in the backlog. The source parser
and SDK checker preserve the separation needed for those later frontends and
adapters. Final shared-document capacity failures may currently point at the root;
more precise related-location diagnostics remain useful follow-up work.

No commit, push, publication, system install or DockPipe source change occurred.
