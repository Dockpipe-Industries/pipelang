# Structured PipeView XML and CLI diagnostics

The next AI/tooling slice gives XML admission failures the owner-defined source
diagnostic code `PV1001`. It also adds `pipelang-viewc --diagnostics text|json`.
The default text interface remains readable. JSON mode writes one versioned
failure object to stderr, separating structured source diagnostics from ordinary
tool failures. Success remains silent, with unchanged output/receipt contracts.
The [contract](../concepts/pipeview-markup.md#machine-readable-failures) defines
the fields, exit status and argument-selection behavior.

The optional XML package retains original bounded source bytes, normalizes
admitted file identities through the existing SourceSet, and resolves one-rune
or EOF admission spans. It preserves compiler UTF-8 admission diagnostics instead
of disguising them as XML profile failures. An oversized library input uses a
bounded actual prefix only for its offset-zero size diagnostic. Typed semantic
mapping from the [previous checkpoint](pipeview-markup-diagnostics.md) is retained.

The CLI consumes the existing resolved compiler diagnostic API. It adds no
parallel range calculation or flag parser. Flag messages are buffered until the
parsed output mode is known, so JSON failures contain one object. Missing files,
ordinary component failures and other unstructured tool errors receive no
invented source code or location. A typed native-lexer diagnostic retains its
actual source identity. No runtime endpoint or automatic source execution is
introduced.

Local verification at `/tmp/pipeview-diagnostics-json/acceptance.json` retains
focused XML/CLI tests, bounded worker/job receipts, installed DockPipe build/test
workflow results and an actual Go/native lexer comparison. Cases include malformed
XML, unsupported profiles, duplicate/unknown attributes, Unicode/rune/EOF bounds,
invalid UTF-8, type errors with related locations, flag failures, ordinary tool
errors, silent success and failed-publication preservation.

Both actual compiler paths report the same JSON admission/type failures and
regenerate the exact settings header accepted in
`/tmp/pipeview-markup/acceptance.json`. Its installed ForgePipe native Save/reopen
and inspection evidence is reused; this diagnostic-only slice repeats no UI or
SDK experiment. The validation snapshot omits only the older exact CLI gate,
records that exception and runs installed DockPipe 0.6.33. Committed configuration
and dependency locks remain unchanged.

Changes stay in PipeLang's optional frontend, optional authoring CLI, tests and
docs. The local development view compiler is refreshed. Native components,
ForgePipe and DockPipe source, application behavior and frozen compiler fixtures
remain unchanged. No commit, push, publication or system-wide installation is
included.
