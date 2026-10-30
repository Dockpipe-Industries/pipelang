package markup

import (
	"errors"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func TestXMLFailureBoundsUTF8OffsetsAndNormalizesSourceIdentity(t *testing.T) {
	text := "α\r\n🏍Z"
	cases := []struct {
		offset int64
		start  int
		end    int
	}{
		{offset: -1, start: 0, end: 2},
		{offset: 0, start: 0, end: 2},
		{offset: 1, start: 0, end: 2},
		{offset: 2, start: 2, end: 3},
		{offset: 3, start: 3, end: 4},
		{offset: 4, start: 4, end: 8},
		{offset: 6, start: 4, end: 8},
		{offset: 8, start: 8, end: 9},
		{offset: 9, start: 9, end: 9},
		{offset: 999, start: 9, end: 9},
	}
	for _, path := range []string{"", "folder/../Normalized.xml"} {
		input := &pipelang.SourceFile{ID: "folder/../Fallback.xml", Path: path, Text: text}
		for _, test := range cases {
			err := xmlFailure(input, test.offset, "Bounded XML profile refusal")
			var failure *pipelang.DiagnosticError
			if !errors.As(err, &failure) || len(failure.Diagnostics) != 1 {
				t.Fatalf("missing structured failure: %v", err)
			}
			diagnostic := failure.Diagnostics[0]
			expectedID := pipelang.FileID("Fallback.xml")
			if path != "" {
				expectedID = "Normalized.xml"
			}
			admitted, exists := failure.Sources.File(diagnostic.Primary.File)
			if !exists || diagnostic.Primary.File != expectedID || diagnostic.Primary.Start != test.start || diagnostic.Primary.End != test.end {
				t.Fatalf("offset %d did not map to a bounded rune: %+v", test.offset, diagnostic.Primary)
			}
			resolved := pipelang.ResolveDiagnostics(failure.Sources, failure.Diagnostics)[0]
			if resolved.Primary.Start != admitted.Position(test.start) || resolved.Primary.End != admitted.Position(test.end) {
				t.Fatal("normalized diagnostic identity did not resolve its actual source bytes")
			}
		}
	}
}
