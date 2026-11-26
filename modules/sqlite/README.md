# Optional native SQLite capability

`PipeLangSqlite 0.1.0` exports `PipeLang::Sqlite` and owned C++ types under
`pipelang/sqlite/{Connection,Value,Row,Options}.hpp`. It wraps maintained SQLite
3.54.1 with FTS5 and threadsafe mode, built from the pinned upstream amalgamation.
It has no compiler, domain SQL, JSON, orchestration, cloud or model-policy logic.
No system SQLite fallback or dynamic extension loading is available. Ordinary
PipeLang consumers use typed injected host capabilities; raw SQLite pointers and
native connections are not ordinary source values.

`Connection::Open(absolutePath, readOnly, Options{})` returns an owner-confined
connection. Calls return `pipelang::model::Result<T,string>` with Success,
Cancelled or a prefixed `sqlite.*` Failure. `Connection::OpenExisting` has the
same signature and checks but omits SQLite's create flag, including for writable
opens. A missing file fails without creating a database. Ordinary requests using
preconfigured storage should use this method; provisioning may explicitly use
`Open`. Neither method creates parent directories. Explicit `Close` invalidates the
connection, rolls back an active transaction, and retains the user database.
Destruction finalizes native ownership and rolls back uncommitted work without
throwing. Callers admit private parent directories; final symlinks, URI filenames,
relative and dot-component paths are refused. This is not a filesystem sandbox.

`Execute(sql, parameters, token)` prepares exactly one statement, binds SQLite's
parameter slots and drains its rows without materializing them. It
returns SQLite's directly changed row count, or zero if no changes occurred.
`Query(sql, parameters, maximumRows, token)` accepts read-only statements and
returns complete bounded owned rows; it fails instead of silently truncating.
The supplied vector must contain exactly `sqlite3_bind_parameter_count` values
in slot-index order; that count is SQLite's highest slot, including holes from
explicit numbering. Anonymous `?`, numbered `?N` and named `:name`, `@name` and
`$name` use SQLite's normal slot semantics. Repeated numbers/names reuse the same
bound value; each slot is bound once and excess/missing values fail. SQL is never
rewritten. Parameter values remain data, including embedded NUL text and binary
BLOBs. Text is validated UTF-8; arbitrary octets belong in Blob. NaN Real values
are refused because SQLite converts them to NULL.

Bounds are 1 MiB per SQL/value, highest parameter slot 256, 32 result columns,
1..256 requested rows, and 8 MiB aggregate parameter or result ownership. SQLite's
serialized row/length budget is 2 MiB. Default work limits are 100 ms busy time,
1000 ms operation time and 1,000,000 VM steps. Options permit busy times 0..5000 ms,
operation times 1..10000 ms and 1000..100,000,000 VM steps. Progress and busy
callbacks cooperate with cancellation; already completed statements retain their
authoritative outcome. These are cooperative SQLite work limits, not preemption
of arbitrary filesystem stalls. Connections cannot cross owner threads.

`Begin`, `Commit` and `Rollback` expose SQLite transactions. `BackupTo(newPath)`
uses SQLite's online backup API, requires no active caller transaction and refuses
an existing destination. It creates a consistent database snapshot only. It does
not coordinate Markdown/source manifests or implement a product backup. Failed
new backups are cleaned up only when their created file identity still matches.

Run `workflows/sqlite.build/config.yml` with an exact installed model SDK prefix,
its SHA256, a pinned amalgamation archive and fresh proof directory. The workflow
verifies/extracts upstream inputs, builds sequentially under existing containment,
runs native and ASan/UBSan fixtures, installs the optional package and checks an
installed CMake consumer. Normal builds explicitly use O3; sanitizer C++ boundary
code uses O1 and the maintained C amalgamation remains uninstrumented.
`PIPELANG_SQLITE_PROFILE=release` selects one O3 normal build and installed-consumer
check, without claiming a sanitizer run in that receipt. Its separately retained
qualification proof supplies sanitizer evidence. Linux/POSIX qualification does not establish Windows,
macOS, hosted release or broader database reliability acceptance.
