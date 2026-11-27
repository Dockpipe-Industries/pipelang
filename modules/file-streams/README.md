# PipeLang file streams

This optional package selects Linux x86-64 and exact installed PipeLang Model
and CryptoStream SDK manifests. It ships the PipeLang.IO Source descriptor and
a C++ adapter for the generated interface. It does not bundle another hash
implementation or modify either dependency SDK.

Composition admits an absolute directory as a held directory-object
capability. Callers select canonical relative paths and a bounded native
path profile. Capture and digest accept regular files and refuse symlinks.
Hardlinks are permitted, and the returned link count is a metadata
observation. The provider retains the root and ancestor descriptors and
compares the named edges and final file revisions at its observation points.
The captured buffer is an immutable copy of the bytes the adapter accepted.
Use a controlled local workspace. Concurrent mutations that escape metadata
observations can still affect the accepted bytes. The API offers neither a continuous namespace observation nor an atomic
snapshot of a concurrently modified source. Reads can precede a late refusal.

Source owns the lifetimes of successful captures and uses CloseSnapshot.
Repeated close works while the local tombstone remains. Closed or foreign
IDs cannot read or refer to another capture. Digest streams through CryptoStream
in blocks of at most 64 KiB. Capture admits at most 4 MiB per file. Digest
admits at most 512 MiB. The adapter probes EOF at the cap before accepting the
result. Each operation charges openat, fstatat, fstat and read attempts,
including retries, to the selected finite syscall budget. Root admission uses
its own budget. Local kernel calls may stall between cancellation checks.

One executable link domain has eight owners, eight concurrent operations,
256 held or reserved descriptors and eight pending or retained snapshots with
32 MiB of reserved capture payload. Returned ByteBuffer copies and allocator
overhead have separate costs. Calls on a busy owner fail immediately. An owner
must outlive its calls. Cancellation releases in-flight native resources and
publishes no captured buffer. Read failures carry NoDurableOutput because the
read operations create no named output.

ExclusiveFileOutput provides direct exclusive creation and bounded appends to
its new descriptor. It never replaces or deletes an existing
name. It defaults to 4 MiB total and accepts the runtime's 1 MiB bounded
fragments. A composition can explicitly select up to 32 MiB for a controlled
staged writer. The writer and reader share the existing native owner, operation,
and descriptor ledgers.

Successful creation returns a live handle and makes the name visible.
Append, commit and close update or retire that handle. Append failure or
cancellation consumes an admitted writer and retains any named effect.
Close of an unfinished writer returns Unknown even when releasing its native
resources succeeds. A recent closed handle replays its typed terminal effect.
Commit checks the written length, syncs the file and parent directory, seals the
digest, verifies the observed name and root, and closes native resources.
Committed describes the checks at that point. The controlled workspace must
exclude concurrent external writers for an independently stable output.
AtomicFileReplacement holds both the target directory and its private
staging directory. A caller first commits a staged file, then supplies that
file's observed identity and extent. A single rename replaces the target and
the adapter checks the visible inode and syncs both directories. Failure or
cancellation after the rename attempt carries Unknown, so the caller can read
back the target before deciding how to reconcile it.

The staging directory is reserved for its composition owner. The euid and
mode checks enforce private OS access, while cooperation is required among
processes running as that same user. Metadata checks report observed changes.
The contract does not furnish a continuously immutable staging namespace.
ExistingFileAppend admits a caller-selected extent cap up to 8 GiB and
appends one bounded fragment to an existing private regular file. Inspection
returns the observed extent and revision, and each append requires that pin.
A single O_APPEND write places the new bytes at the current end. A short
write or cancellation after the attempt returns Unknown with retained effects.
The adapter does not retry a partial fragment. Stale pins refuse further
writes, so a caller can inspect and reconcile the current stream first.
Success describes the visible extent after the descriptor closes. It does
not promise a per-record disk sync.

Configure the package with exact core and CryptoStream manifest pins and an
independent absolute installation prefix. Select that prefix and its exact
manifest when building a consumer, and supply the installed Source descriptors
to the compiler. No downloads occur during dependency selection.
The qualification consumer in tests/file-streams compiles and runs the same
Source cases against normal, instrumented and relocated SDKs. Sanitizer
instrumentation covers the C++ consumer, generated Source and native adapter.
The installed Rust provider is used as built.
