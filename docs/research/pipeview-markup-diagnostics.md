# Authored XML semantic diagnostic locations

This follow-up to the [XML frontend checkpoint](pipeview-markup.md) maps shared
typed-checker failures to the authored XML element opening tag. The optional
markup package owns this mapping; compiler semantics, runtime behavior, headers
and source/receipt contracts are unchanged. Attribute-level precision remains
pending. The [format contract](../concepts/pipeview-markup.md) describes the
current user-facing diagnostic behavior.

XML parsing retains original UTF-8 spans before consuming child elements. After
canonical formatting, the generated and authored declaration trees are paired
structurally with identity/shape guards. A diagnostic maps through the smallest
containing generated origin. This avoids text searching, which would confuse
repeated IDs, labels or escaped values. The generated span remains related
context; structured diagnostic resolution has both source files available.
Mapped errors preserve their code, category, severity, message and cause.

Only failures from shared semantic checking are eligible. Lexer/process errors,
including a selected lexer returning a semantic-looking diagnostic, retain
generated identity. Unmapped failures also retain their original attribution.
There is no native lexer fallback and no extra selected-lexer invocation.

Focused tests cover declaration/action/binding errors, unused/duplicate
declarations, nested controls and repeated text, multiline/CRLF/Unicode authored
locations, structured and related spans, boundary refusal and native error
identity. CLI tests cover all three output formats and refuse to publish on a
type error. Build/test workflows use installed DockPipe 0.6.33 in the same
temporary gate-exception model as the original XML checkpoint; committed
configuration and lock files are unchanged.

The proof at `/tmp/pipeview-markup-diagnostics/acceptance.json` retains bounded
resource receipts and comparisons using both Go and the actual native lexer.
Both report the same authored XML error. Both regenerate the exact native
settings header accepted in `/tmp/pipeview-markup/acceptance.json`. The installed
ForgePipe native Save/reopen and model/inspection evidence from that checkpoint
is reused because compiled application bytes are unchanged. No native UI run or
SDK experiment was repeated for this diagnostic-only slice.

The source edits and refreshed optional local view compiler remain in PipeLang.
ForgePipe and DockPipe repositories were not changed. No commit, push,
publication or system installation is part of this work.
