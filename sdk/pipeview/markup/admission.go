package markup

import (
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func xmlFailure(file *pipelang.SourceFile, offset int64, message string) error {
	text := file.Text
	if len(text) > MaximumBytes {
		// Oversized input has already failed admission. Only a bounded actual
		// prefix is needed to resolve its first-rune size diagnostic.
		end := MaximumBytes
		for end > 0 && !utf8.RuneStart(text[end]) {
			end--
		}
		text = text[:end]
	}
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{
		Path: sourcePath(file), Data: []byte(text),
	}})
	if diagnostics.HasErrors() {
		// Preserve source admission's original UTF-8 diagnostics. An invalid
		// byte sequence must not become a fabricated XML profile error.
		return &pipelang.DiagnosticError{Sources: sources, Diagnostics: diagnostics}
	}
	admitted := sources.Files()[0]
	start := int(max(int64(0), min(offset, int64(len(text)))))
	if start < len(text) {
		for start > 0 && !utf8.RuneStart(text[start]) {
			start--
		}
	}
	end := start
	if start < len(text) {
		_, width := utf8.DecodeRuneInString(text[start:])
		end += width
	}
	return &pipelang.DiagnosticError{
		Sources: sources,
		Diagnostics: pipelang.Diagnostics{{
			Code:     CodeInvalidXML,
			Category: pipelang.CategorySource,
			Severity: pipelang.SeverityError,
			Message:  message,
			Primary:  pipelang.Span{File: admitted.ID, Start: start, End: end},
		}},
	}
}
