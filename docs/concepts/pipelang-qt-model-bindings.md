# Checked Qt model bindings

Candidate host tool `pipelang-qt-bindc` consumes a generated ordinary-model manifest
and an explicitly selected canonical source model name. It supports project module
identities and manifest v1/v2, selecting only an exported native model root. It emits
a QObject wrapper around an explicitly supplied `unique_ptr<Model>`; it does not
select constructors, resolve services by name or reinterpret source method bodies.
The existing ForgePipe `.pview`/`.pxml` emitter is unchanged.

The first binding profile projects bool, owned UTF-8 string and `List<string>` public
snapshot fields plus zero-argument pure scalar getters. Synchronous methods accept
bool/string/string-list inputs and return bool/string or string-error Results with
void/bool/string success. Unsupported types, async methods, overloads and conflicting
Qt/public names refuse before emission. Numeric transport, reference-bearing Qt
values and general record/enum envelopes are not silently coerced through QVariant.

Generated invocations return `pipelang::qt::ActionReport`: the operation's typed
Success/Cancelled/Failure outcome and success-only payload are separate from the
projection outcome. Source success remains authoritative if projection fails.
Projection stages all owned values before notification; failure keeps the last
committed cache with explicit `projectionStatus` Failure. A consumer must treat that
projection as unavailable, not fresh success. Unexpected native faults use fixed safe
codes; raw exception messages and malformed metadata are not display values.

Qt text conversion preserves embedded NUL and valid Unicode by explicit lengths,
and refuses unpaired UTF-16/invalid UTF-8. The technical bridge bounds text/list
inputs before entering the existing runtime's owned-value checks. A failed/cancelled
outcome has no success payload; a void Success intentionally has no payload either.

The QObject and model stay on their creating Qt thread. Effectful reentry during
notification is refused. Checked pure queries may inspect already committed source
state during notification, without recursively projecting or emitting. Source/native
host callback reentry guards still apply. The wrapper owns its supplied model and
must be destroyed on its owner thread; QObject context ownership removes queued view
callbacks. It is not a cross-thread service object or an async network executor.

`pipelang_add_qt_bindings` is an optional CMake helper alongside the existing model
helper. It accepts an already declared model target, manifest path, canonical model
name, adapter identity, output directory and exact host compiler path/hash. The
consumer compiles moc output against its target Qt kit; the generator stays a host
executable. Every build verifies the selected installed model/Qt SDK inventory.
A versioned receipt binds generated header, model manifest and generator hashes.
`QT_NO_KEYWORDS` prevents Qt macros from corrupting otherwise valid source names.

For the qualified single-thread Emscripten strategy, consumers explicitly select
`EMSCRIPTEN_EXCEPTION_MODE JS`; the helper enables ordinary Emscripten C++ exception
lowering for fallible source calls. It does not enable pthreads or claim a source
async fetch strategy. Cloud's first candidate uses owned completion events from its
existing transport; permission, navigation, cancellation and request-epoch decisions
remain source-authored. Thin QML remains the layout/binding surface for this slice.

Validation is separate: bounded host metadata/compiler proofs and real native moc
consumer proof are recorded under `/tmp/pipelang-qt-bindings-20261109`. Browser/WASM
proof must run the actual Qt application and interact with rendered controls; a
headless model binary or QCoreApplication test alone is not that acceptance.

The exported ModelRuntime target is header-only. Its CMake package version is
architecture-independent so an explicitly pinned native host generator can serve
wasm32 compilation. This is not a native archive ABI promise: host executable
platform, exact SDK inventory and the target Qt kit remain explicitly selected.

The older [Qt boundary reference](pipelang-qt-boundary.md) is historical scalar and
resolver pilot evidence. It does not enlarge this current checked model profile.
A general Qt `.pview` backend remains separate work.

The current v1 Qt tool profile admits at most4MiB of manifest JSON and4MiB of
emitted header, JSON nesting64, metadata arrays256 entries and native identifier
components96 characters. Qt input conversion bounds a string to1,048,576 UTF-16
code units and a string list to4096 elements before the ordinary owned-value
runtime checks. These are explicit adapter-profile limits, not PipeLang source or
project-size ceilings. Cloud's source policy applies its own smaller profile DTO
bounds. Unsupported metadata is refused rather than truncated or coerced.
