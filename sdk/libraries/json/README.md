# PipeLang.Json 0.1.0 — immutable strict JSON reader

This general SDK effect exposes JSON syntax and typed observations. The calling
PipeLang application owns schema validation, traversal decisions, allowed fields,
cardinalities, IDs, evidence, coverage, resume and publication policy. No file I/O,
hashing or product schema is included.

The source library is installed under `share/pipelang-model/libraries/json` and
consumed through the existing checksum-pinned `SOURCE_LIBRARIES` contract. Its
native implementation is `PipeLang::JsonReader`, exported by PipeLangModel.
Instantiate `pipelang::json::ReaderAdapter<Contract,Limits,Node,Kind>` using the
compiler-generated `JsonReader`, `JsonReadLimits`, `JsonNode` and `JsonKind` types.
This adapter implements the actual generated host interface, without a custom
interpreter or compiler syntax. `tests/json` is a generated-source consumer.

## Input and lifetime

Call `BeginDocument`, then append ByteBuffer fragments, each at most 1MiB. Only
`CompleteDocument` parses. It validates the entire input before any node query
succeeds; an unvisited invalid member still refuses completion. Append failure,
parse/resource failure and cancellation poison the document and immediately free
the complete/partial tree arena and its reservation. Captured input remains
reserved until close for exact-byte observations. Closing it remains
necessary and permitted. Successful completion freezes the document; another
append or completion is refused. Exact captured bytes remain available through
`InputLength` / `InputSlice`, including after a failed parse, until close.

Each Reader defaults to eight live documents. Its native ReaderOptions constructor
selects a positive capacity up to the hard maximum of sixteen. Process-unique generation IDs and
nonoverlapping node-ID ranges reject stale, cross-reader and mixed-document
handles. These are ownership checks, not an authorization boundary against a
caller deliberately forging another live handle. Close releases the captured
input and complete/partial tree. An immediately repeated close succeeds; a closed
ID retained in a slot's one-entry tombstone becomes invalid when the slot is
reused. Unknown/foreign IDs fail. Destruction releases every remaining document.
The synchronous Reader is single-owner: callers serialize access.

`CompleteDocument` receives a CancellationToken. It checks before/after parsing
and at every scalar, key and container event. Parsing whitespace or one token is
bounded by the admitted input, but it cannot interrupt the pinned parser inside
a token. Cancellation returns `Cancelled("json.cancelled")`, never Success.
Append/query operations are bounded synchronous effects; they have no implicit
scheduler or deadline. Host containment remains independent.

## Strict syntax and numeric policy

Input is UTF8 only, without a BOM. UTF16/32 auto-detection is deliberately absent.
Decoded-key duplicates in any object fail, including escaped-equivalent keys;
equal keys in distinct objects are allowed. Malformed/truncated syntax, trailing
values, invalid UTF8, overlong encodings, escaped lone surrogates, raw controls,
NaN/Infinity and binary64 exponent overflow are refused. Raw NUL is refused
before the pinned lexer can mistake it for an end marker; trailing bytes cannot
hide behind NUL. Escaped NUL is valid.
This UTF8/finite contract deliberately tightens the historical Python
`json.loads(bytes)` boundary; it is not a claim of identical legacy acceptance.

Number nodes retain decimal integer spelling normalized by the pinned parser
(for example `-0` becomes `0`); float-form tokens retain their parsed lexeme.
`ScalarSlice` is therefore a scalar observation, not an exact-input number
lexeme contract or serializer. Use `InputSlice` for exact captured bytes/pins.
`Int64Value` accepts only integer-form values in signed 64-bit range; decimal or
exponent-form numbers do not become integer identifiers by rounding. Integer
conversion never passes through double. `FiniteFloatValue` uses checked
locale-independent binary64 conversion and refuses overflow/underflow reported
by `from_chars`. It may round large finite integers as ordinary binary64 does.
Do not use that conversion for integer identity. Complete refuses nonfinite
numbers; conversion failure remains recoverable and does not mutate the tree.

## Queries

`Kind` distinguishes Null, Boolean, Number, String, Object and Array.
`FindMember` returns `JsonNode?`: a missing member has no value, while JSON null is
Some(node with Null kind). `Count` and `At` operate only on arrays/objects; object
iteration preserves input member order. `NameLength`/`NameSlice` on the child
node expose its decoded member name, so source can reject unknown fields.
Array indexes are signed zero-based and checked. Arrays are not transported as
source Lists and can contain more than 4096 entries.

Bool/int/float queries reject wrong kinds. String and number scalar bytes and
member names support length and bounded slice queries. All lengths/offsets are
bytes. ByteBuffer slices may split UTF8; decode only a complete valid fragment.
A string larger than one source string remains accessible by bounded fragments.
Missing name, wrong kind, invalid index and invalid handle are distinct failures.

## Optional decoded-string equality index

The optional index uses a separate `JsonStringIndexer` interface. Existing
`JsonReader` implementers and its default limits/capacities remain unchanged.
Instantiate `pipelang::json::StringIndexAdapter<Contract,Limits,Index,Node>` with
compiler-generated `JsonStringIndexer`, `JsonStringIndexLimits`,
`JsonStringIndex` and `JsonNode` types, passing the associated
`ReaderAdapter::nativeReader()` by reference. That Reader must outlive the
optional adapter and own every document used with it. Access remains serialized
and single-owner. No arena is allocated until an index is explicitly begun.
Linux x64 native boundary and actual generated-interface checks pass normally
and with the reader fully ASan/UBSan/LeakSanitizer-instrumented. The source fixture
uses 4097 independent calls; one authored large traversal, maximal index/tree
coexistence and native Windows/macOS qualification remain separate, pending proof.

`BeginStringIndex(document, limits)` requires a complete, live document and
admits one live index per document. Both `entries` and `retainedBytes` must be
explicitly positive. Entries cannot exceed the document's admitted node capacity;
the host may select a capacity above the number of strings actually present,
including on a document without strings. There is no arbitrary entry or memory
default. Retained bytes cannot exceed the unchanged 640MiB aggregate ceiling and
are charged to its existing input/tree/index ledger before allocation. Coexisting
document reservations may refuse a smaller request. The selected arena has no
upstream allocation fallback and is independently reclaimed on close. Retained
bytes describe reserved arena storage, not a promise that a given entry count
fits on every ABI. The document's native `retainedCapacity` query continues to
report its tree and captured-input capacity; `aggregateRetainedBytes` includes
all optional index reservations as well.

`AddString(index, node, token)` accepts only a complete String node in the same
Reader and document. It returns true for the first decoded value and false for
an existing equal value, including the same node or an escaped-equivalent
spelling. Duplicates still return false when entry capacity or arena space is
full. Full length-qualified UTF8 bytes, including escaped NUL, determine equality;
case and Unicode normalization distinctions are preserved. Strings larger than
one source string are accepted by node identity without copying through a source
value. No automatic traversal, identifier grammar, schema or duplicate rejection
policy is included.

`ContainsString(index, candidate, token)` checks the same decoded equality.
The live String candidate may come from any complete document owned by the
same Reader. Lookup borrows the candidate only for the call, allocates no
entries, and leaves both documents and index capacity unchanged. Closing the
candidate document does not affect existing entries. A stale or foreign
candidate fails at the document or node ownership check.

The index is an AVL ordered set of borrowed immutable decoded string views. Its
search has logarithmic comparison count and each comparison remains dependent on
string lengths. Cancellation is checked before work, at every comparison and
every at-most-4096-byte comparison chunk, and immediately before insertion.
Cancellation returns `Cancelled("json.cancelled")` with no insertion. After
allocation succeeds, linking and balancing form a noncancellable commit region;
a successful insertion is never reclassified because a token changes afterward.
Limit/allocation/cancellation failure leaves prior entries and the document
usable. An arena allocation failure can prevent further distinct values while
all previously inserted values remain queryable.

Index effects are external host effects: source rollback does not undo prior
insertion. Keep the index lifetime in source `try/finally` and call
`CloseStringIndex` on every exit before closing its document. Close requires
no token and remains available during cancellation. Immediately repeated close
succeeds; a successful begin in that slot invalidates the one-entry tombstone.
Process-unique index generations are never reused. Unknown, foreign and stale
handles fail. Insertion also refuses candidates from another document. These identities are ownership checks rather than
an anti-forgery authorization boundary. Document close/destruction releases the
index before its borrowed decoded text. Repeated close/reopen cycles do not
accumulate allocation in the document arena.

Additional stable failures are `json.string_index` (invalid/stale index),
`json.string_index_limit` (the document already owns one),
`json.string_index_entry_limit` (a new distinct value exceeds admission), and
`json.string_index_state` (an impossible internal AVL path bound). Existing
`json.document`, `json.node`, `json.kind`, `json.not_complete`,
`json.limits`, `json.retained_limit`, `json.memory_limit` and
`json.generation_limit` retain their generic meanings. No increase in input,
node, collection, document, process or host containment limits is implied. In
particular, entry admission does not promise that every 4MiB document and its
index fit together; maximal workloads and other platforms need independent proof.

## Admission and retained storage

Every limit must be positive and no larger than its hard SDK ceiling:

| Limit | Hard ceiling |
| --- | ---: |
| Captured input bytes | 4MiB |
| Total value nodes, including root/containers | 2097152 |
| Open object/array nesting | 64 |
| Total decoded member-name + scalar string/number bytes | 4MiB |
| One decoded member name | 1MiB |
| Retained tree allocation arena | 512MiB |
| One append or returned slice | 1MiB |
| Live documents per Reader (default 8, host selected) | 16 |
| Process-wide retained input + arena reservations | 640MiB |

The tree, child indexes and duplicate-key map use one fixed PMR arena with no upstream
allocation fallback. Exhaustion returns `json.memory_limit`; input admission
alone does not guarantee the tree fits. Input storage reserves the selected input
limit separately. Per-document defaults remain conservative: 131072 nodes and a 32MiB arena; callers
can explicitly select the larger hard bounds. Input storage reserves its selected
limit and refuses an implementation capacity above that bound. A shared atomic
RAII ledger refuses input+arena reservations above 640MiB across all Reader
instances in the linked SDK runtime, before allocation. At default capacity eight,
one Reader reserves at most 288MiB input/arena plus fixed bookkeeping. Allocation
failure returns its entire reservation. Parse/append/cancellation failure releases
its tree arena immediately; captured input remains charged until close. Close and
destruction release all remaining reservations. These bounds preserve existing
unit/kernel limits; they are not a relaxation of host containment.
Standard-library input vector capacity is observed by `retainedCapacity` in the
native resource fixture. The parser's temporary token strings are separate:
their length is bounded by 4MiB admitted input, and its parser state by depth 64.
The retained-arena cap is not a claim of an exact whole-process/allocator RSS cap.
Native allocation failure is recoverable. The maximal <=4MiB flat array may
contain 2097152 total nodes; the new selectable ceiling admits that cardinality.
Whether its representation fits a selected arena is an executable memory-proof
obligation, not an unmeasured guarantee.

Keys are compared through a bounded tree map, avoiding quadratic duplicate-key
scans. Indexed member/array access is constant time; member lookup is logarithmic.
The pinned, checksum-checked nlohmann parser is reused from its existing owning
repository path; no second copy is vendored.

Stable error codes are `json.limits`, `json.retained_limit`, `json.document_limit`, `json.document`,
`json.generation_limit`, `json.node`, `json.not_complete`, `json.read_state`,
`json.input_limit`, `json.node_limit`, `json.depth_limit`, `json.decoded_limit`,
`json.name_limit`, `json.memory_limit`, `json.duplicate_key`, `json.encoding`,
`json.syntax`, `json.cancelled`, `json.kind`, `json.index`, `json.no_name`,
`json.slice`, `json.integer_range`, and `json.number_range`. Syntax includes
invalid encoding inside tokens and nonfinite exponent parse refusal.

Linux x64 qualification passes42 authored tests through the real generated host
ABI, native resource/lifetime checks and the maximal4MiB/2,097,152-node flat array,
with normal and full ASan/UBSan/leak instrumentation. The maximal workload used
about523MiB normal and590MiB instrumented under unchanged containment limits.
Native Windows/macOS/WASM acceptance remains unverified.

The SDK installs the native archive, public headers, source library and immutable
native reader sources. `PipeLangModel_JSON_READER_SOURCE_DIRECTORY` identifies
manifest-bound sources for instrumentation; no source checkout is required.
The existing pinned JSON header is included in that generated SDK artifact with
its license, rather than copied into another owning source tree. The general
reader does not implement file capture, stream hashing, JSON output or product
qualification policy.


## Optional borrowed value output

Bind the generated `JsonValueRenderer` interface with a
`pipelang::json::ValueRendererAdapter` when the host needs to copy a checked
JSON subtree into its own private output. Supply the same native Reader that
owns the borrowed node and a byte sink. The adapter emits compact, spaced, or
two-space indented JSON in decoded object-key order and can emit ASCII escapes.
Array order and the reader's stored numeric spelling are preserved. An initial
indentation depth permits embedding an indented subtree in a larger document.

Select a positive, per-value output limit up to 32 MiB. The native renderer
delivers at most 4 KiB per call without assembling the output in memory.
A sink must consume each borrowed byte fragment during the callback. Callers
retain custody of any delivered prefix after a failure or cancellation. Use a
private staging sink and commit only after the enclosing document succeeds.
An in-progress render retains the source document until it returns. JSON
formatting does not establish the schema or trust of the emitted data.

## Structured document output

A host can bind `JsonDocumentOutput` with
`pipelang::json::DocumentWriterAdapter` to emit one document to its own sink.
Source selects the object members and their order. The adapter checks container
state and combines typed scalars, nested arrays/objects and checked borrowed
JSON values. It enforces a single byte limit across the document and its final
newline. Use a new adapter for each document.

The sink must consume fragments during the callback. A failed or cancelled
operation leaves any emitted prefix in the sink and invalidates the writer.
Keep the sink private until the Source operation finishes and the owning output
effect commits. The adapter validates UTF-8 text and rejects non-finite numbers.

Native composition can capture one complete value with
`pipelang::json::DocumentWriter::capture`. It owns the serialized bytes and
allows the original reader documents to close before a later
`DocumentWriter::captured` call. The caller supplies a byte limit and the
enclosing container depth. The receiving writer must match that depth, its
layout, and its ASCII escaping setting. The capture has no document newline.
Use the value's size to account for the caller's aggregate output allowance.
A cancelled capture yields no value. A cancelled transfer invalidates the
receiving writer and may leave a prefix in its staged sink.

A `DocumentWriterAdapter` can borrow an existing native writer for the
duration of the capture callback. Source using that adapter supplies one
complete value. The capturing owner seals it after the callback returns.

## Optional wire encoding detection

Set `JsonReadLimits.detectUnicodeEncoding` for a document whose producer can
supply a UTF-8 BOM or UTF-16/UTF-32 input. Detection uses a byte order mark when
present and the leading JSON code units otherwise. The default remains strict
UTF-8 without a BOM. Decoding rejects invalid or incomplete code units, checks
cancellation and charges temporary storage to the reader's aggregate ledger.
`InputLength` and `InputSlice` still expose the exact original input bytes.
Decoded string and number access retain their existing JSON validation rules.
