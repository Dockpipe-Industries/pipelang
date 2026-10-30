package markup_test

import (
	"errors"
	"reflect"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/markup"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func TestXMLAdmissionErrorsExposeStructuredAuthoredRanges(t *testing.T) {
	const opening = `<View name="V" profile="pipelang.pipeview.source.v1">`
	cases := []struct {
		name   string
		xml    string
		marker string
	}{
		{name: "unknown attribute", xml: opening + `<Label id="x" unknown="yes"/></View>`, marker: "<Label"},
		{name: "duplicate attribute", xml: opening + `<Label id="x" id="y"/></View>`, marker: "<Label"},
		{name: "malformed XML", xml: opening + `<Label id="x"></View>`, marker: "</View>"},
		{name: "DTD", xml: `<!DOCTYPE View SYSTEM "file:///unread">` + opening + `<Label id="x"/></View>`, marker: "<!DOCTYPE"},
		{name: "namespace", xml: `<View xmlns="urn:ui" name="V" profile="pipelang.pipeview.source.v1"><Label id="x"/></View>`, marker: "<View"},
		{name: "invalid numeric reference", xml: opening + `<Label id="x" text="&#xD800;"/></View>`, marker: "&#xD800;"},
		{name: "unsupported profile", xml: `<View name="V" profile="unsupported"><Label id="x"/></View>`, marker: "<View"},
		{name: "Unicode offset", xml: "\u00a0" + opening + `<Label id="x"/></View>`, marker: "\u00a0"},
		{name: "empty XML", xml: "", marker: ""},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			input := file(t, "Authored.xml", test.xml)
			_, err := markup.ToSource(input)
			var failure *pipelang.DiagnosticError
			if err == nil || !errors.As(err, &failure) || len(failure.Diagnostics) != 1 {
				t.Fatalf("XML admission error was not structured: %v", err)
			}
			diagnostic := failure.Diagnostics[0]
			start := strings.Index(test.xml, test.marker)
			end := start
			if test.marker != "" {
				_, width := utf8.DecodeRuneInString(test.marker)
				end += width
			}
			if diagnostic.Code != markup.CodeInvalidXML || diagnostic.Category != pipelang.CategorySource ||
				diagnostic.Severity != pipelang.SeverityError || diagnostic.Primary.File != input.ID ||
				diagnostic.Primary.Start != start || diagnostic.Primary.End != end || len(diagnostic.Related) != 0 {
				t.Fatalf("unexpected XML admission diagnostic: %+v, expected offsets [%d,%d)", diagnostic, start, end)
			}
			resolved := pipelang.ResolveDiagnostics(failure.Sources, failure.Diagnostics)[0]
			if resolved.Primary.Start != input.Position(start) || resolved.Primary.End != input.Position(end) ||
				!strings.HasPrefix(err.Error(), "Authored.xml:") {
				t.Fatalf("structured range did not retain authored source positions: %+v/%v", resolved.Primary, err)
			}
		})
	}
}

func TestXMLSizeRefusalAllocatesOnlyBoundedDiagnosticSource(t *testing.T) {
	input := &pipelang.SourceFile{ID: "Large.xml", Text: strings.Repeat(" ", markup.MaximumBytes+1)}
	_, err := markup.ToSource(input)
	var failure *pipelang.DiagnosticError
	if !errors.As(err, &failure) || failure.Diagnostics[0].Code != markup.CodeInvalidXML {
		t.Fatalf("size admission did not return an XML source diagnostic: %v", err)
	}
	diagnostic := failure.Diagnostics[0]
	admitted, exists := failure.Sources.File(diagnostic.Primary.File)
	if !exists || len(admitted.Text) > markup.MaximumBytes || diagnostic.Primary.Start != 0 || diagnostic.Primary.End != 1 {
		t.Fatalf("size diagnostic source was unbounded or incorrectly located: %+v", diagnostic)
	}
}

func TestInvalidUTF8RetainsCompilerSourceAdmissionDiagnostic(t *testing.T) {
	text := "Renée\r\n" + string([]byte{0xff})
	input := &pipelang.SourceFile{ID: "Invalid.xml", Path: "folder/../Invalid.xml", Text: text}
	_, expected := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: input.Path, Data: []byte(text)}})
	_, err := markup.ToSource(input)
	var failure *pipelang.DiagnosticError
	if !errors.As(err, &failure) || !reflect.DeepEqual(failure.Diagnostics, expected) || failure.Diagnostics[0].Code != pipelang.CodeInvalidUTF8 {
		t.Fatalf("invalid UTF8 was relabeled as XML: %v", err)
	}
	for _, input := range []*pipelang.SourceFile{nil, {Text: "<View/>"}} {
		_, err := markup.ToSource(input)
		var structured *pipelang.DiagnosticError
		if err == nil || errors.As(err, &structured) {
			t.Fatal("missing source identity must remain an ordinary tool error")
		}
	}
}
