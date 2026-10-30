package markup

import (
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func TestOriginBoundariesAndDiagnosticOwnership(t *testing.T) {
	origins := originMap{
		authoredFile: "Authored.xml", generatedFile: "Generated.pipe",
		authoredBytes: 200, generatedBytes: 50,
		entries: []origin{
			{generated: pipelang.Span{File: "Generated.pipe", Start: 5, End: 40}, authored: pipelang.Span{File: "Authored.xml", Start: 0, End: 20}},
			{generated: pipelang.Span{File: "Generated.pipe", Start: 10, End: 20}, authored: pipelang.Span{File: "Authored.xml", Start: 30, End: 60}},
			{generated: pipelang.Span{File: "Generated.pipe", Start: 20, End: 30}, authored: pipelang.Span{File: "Authored.xml", Start: 60, End: 90}},
		},
	}
	cases := []struct {
		name      string
		start     int
		end       int
		wantStart int
		mapped    bool
	}{
		{name: "smallest containing origin", start: 11, end: 15, wantStart: 30, mapped: true},
		{name: "nonempty ending at boundary", start: 19, end: 20, wantStart: 30, mapped: true},
		{name: "point at adjacent boundary", start: 20, end: 20, wantStart: 60, mapped: true},
		{name: "genuine parent fallback", start: 19, end: 21, wantStart: 0, mapped: true},
		{name: "before containing source", start: 4, end: 6},
		{name: "half open end", start: 40, end: 40},
		{name: "outside root", start: 42, end: 43},
		{name: "EOF", start: 50, end: 50},
		{name: "beyond source", start: 50, end: 51},
		{name: "negative", start: -1, end: 2},
		{name: "reversed", start: 15, end: 14},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			actual, ok := origins.authoredSpan(pipelang.Span{File: "Generated.pipe", Start: test.start, End: test.end})
			if ok != test.mapped || ok && (actual.File != "Authored.xml" || actual.Start != test.wantStart) {
				t.Fatalf("wrong boundary mapping: span=%+v mapped=%v", actual, ok)
			}
		})
	}
	if _, ok := origins.authoredSpan(pipelang.Span{File: "Other.pipe", Start: 11, End: 15}); ok {
		t.Fatal("foreign source identity mapped to XML")
	}
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{
		{Path: "Authored.xml", Data: []byte(strings.Repeat("a", 200))},
		{Path: "Generated.pipe", Data: []byte(strings.Repeat("g", 50))},
		{Path: "Other.pipe", Data: []byte("other")},
	})
	if diagnostics.HasErrors() {
		t.Fatal(diagnostics)
	}
	authored, _ := sources.File("Authored.xml")
	generated, _ := sources.File("Generated.pipe")
	primary := pipelang.Span{File: "Generated.pipe", Start: 11, End: 15}
	related := pipelang.Span{File: "Generated.pipe", Start: 21, End: 25}
	foreign := pipelang.Span{File: "Other.pipe", Start: 0, End: 1}
	original := &pipelang.DiagnosticError{Sources: sources, Diagnostics: pipelang.Diagnostics{
		{
			Code: pipelang.CodeExpressionType, Category: pipelang.CategorySemantic,
			Severity: pipelang.SeverityWarning, Message: "Keep semantic metadata", Primary: primary,
			Related: []pipelang.RelatedSpan{{Span: related, Message: "Related origin"}, {Span: foreign, Message: "Foreign source"}},
		},
		{
			Code: pipelang.CodeUnexpectedToken, Category: pipelang.CategorySyntax,
			Severity: pipelang.SeverityError, Message: "Keep syntax identity", Primary: primary,
		},
		{
			Code: pipelang.CodeExpressionType, Category: pipelang.CategorySemantic,
			Severity: pipelang.SeverityError, Message: "Keep unmappable identity",
			Primary: pipelang.Span{File: "Generated.pipe", Start: 42, End: 43},
		},
	}}
	cause := fmt.Errorf("original checker context: %w", original)
	remapped, ok := origins.remapError(authored, generated, cause)
	var actual *pipelang.DiagnosticError
	if !ok || !errors.As(remapped, &actual) || actual == original || !errors.Is(remapped, original) || !errors.Is(remapped, cause) {
		t.Fatalf("mapped diagnostic or cause chain missing: %v", remapped)
	}
	first := actual.Diagnostics[0]
	if first.Primary.File != "Authored.xml" || first.Code != original.Diagnostics[0].Code ||
		first.Category != original.Diagnostics[0].Category || first.Severity != original.Diagnostics[0].Severity ||
		first.Message != original.Diagnostics[0].Message || first.Related[0].Span.File != "Authored.xml" ||
		first.Related[1].Span != foreign || first.Related[2].Span != primary {
		t.Fatalf("remap changed metadata or lost related provenance: %+v", first)
	}
	if actual.Diagnostics[1].Primary != primary || actual.Diagnostics[2].Primary.File != "Generated.pipe" {
		t.Fatal("syntax or unmappable diagnostics acquired fabricated XML origins")
	}
	actual.Diagnostics[0].Message = "Changed returned diagnostic"
	actual.Diagnostics[0].Related[0].Message = "Changed returned related note"
	if original.Diagnostics[0].Primary != primary || len(original.Diagnostics[0].Related) != 2 ||
		original.Diagnostics[0].Related[0].Span != related || original.Diagnostics[0].Message != "Keep semantic metadata" ||
		original.Diagnostics[0].Related[0].Message != "Related origin" {
		t.Fatal("mapping or editing its result mutated original diagnostics")
	}
}
