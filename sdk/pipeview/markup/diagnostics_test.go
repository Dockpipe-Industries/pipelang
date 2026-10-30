package markup_test

import (
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/markup"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func TestSemanticDiagnosticsResolveAuthoredStartTags(t *testing.T) {
	const typedTarget = `<View name="V" profile="pipelang.pipeview.source.v2">
    <State name="name" type="string" value="Rider"/>
    <State name="other" type="string" value="Other"/>
    <Action name="edited" payload="string" operation="set" target="name"/>
    <Column id="root">
        <Label id="first" value="{bind name}"/>
        <TextField id="edit" value="{bind other}" changed="{edited}"/>
    </Column>
</View>`
	const nested = `<View name="V" profile="pipelang.pipeview.source.v1">
    <State name="name" type="string" value="Rider"/>
    <Event name="edited" payload="string"/>
    <Column id="root"><Row id="nested">
        <Label id="intro" text="Renée 🏍"/>
        <TextField id="edit" value="{bind missing}" changed="{edited}"/>
    </Row></Column>
</View>`
	multiline := strings.ReplaceAll(nested, `<TextField id="edit" value="{bind missing}" changed="{edited}"/>`,
		"<TextField\n            id=\"edit\"\n            value=\"{bind missing}\"\n            changed=\"{edited}\"/>")
	multiline = strings.ReplaceAll(multiline, "\n", "\r\n")
	cases := []struct {
		name           string
		xml            string
		tag            string
		message        string
		lastOccurrence bool
	}{
		{
			name:    "set payload type",
			xml:     strings.Replace(settingsXML, `name="nameEdited" payload="string"`, `name="nameEdited" payload="bool"`, 1),
			tag:     `<Action name="nameEdited" payload="bool" operation="set" target="playerName"/>`,
			message: "payload must match target state type",
		},
		{
			name:    "set target type",
			xml:     strings.Replace(settingsXML, `target="playerName"`, `target="vsync"`, 1),
			tag:     `<Action name="nameEdited" payload="string" operation="set" target="vsync"/>`,
			message: "payload must match target state type",
		},
		{
			name:    "control target differs from action",
			xml:     typedTarget,
			tag:     `<Action name="edited" payload="string" operation="set" target="name"/>`,
			message: "target must match its control binding",
		},
		{
			name:    "unknown binding",
			xml:     strings.Replace(settingsXML, `{bind playerName}`, `{bind missing}`, 1),
			tag:     `<TextField id="name-field" value="{bind missing}" changed="{nameEdited}"/>`,
			message: "unknown state missing",
		},
		{
			name:    "unknown action reference",
			xml:     strings.Replace(settingsXML, `{nameEdited}`, `{missing}`, 1),
			tag:     `<TextField id="name-field" value="{bind playerName}" changed="{missing}"/>`,
			message: "unknown action missing",
		},
		{
			name:    "duplicate state",
			xml:     strings.Replace(settingsXML, `<Column id="root">`, `<State name="playerName" type="string" value="duplicate"/><Column id="root">`, 1),
			tag:     `<State name="playerName" type="string" value="duplicate"/>`,
			message: "duplicate declaration playerName",
		},
		{
			name:    "unused state",
			xml:     strings.Replace(settingsXML, `<Column id="root">`, `<State name="unused" type="string" value="x"/><Column id="root">`, 1),
			tag:     `<State name="unused" type="string" value="x"/>`,
			message: "state is not bound",
		},
		{
			name:           "duplicate action",
			lastOccurrence: true,
			xml:            strings.Replace(settingsXML, `<Column id="root">`, `<Action name="save" operation="save"/><Column id="root">`, 1),
			tag:            `<Action name="save" operation="save"/>`,
			message:        "duplicate declaration save",
		},
		{
			name:    "unused action",
			xml:     strings.Replace(settingsXML, `<Column id="root">`, `<Action name="unused" operation="revert"/><Column id="root">`, 1),
			tag:     `<Action name="unused" operation="revert"/>`,
			message: "action is not used",
		},
		{
			name:    "duplicate control identity",
			xml:     strings.Replace(settingsXML, `id="title"`, `id="root"`, 1),
			tag:     `<Label id="root" text="Settings"/>`,
			message: "duplicate control identity",
		},
		{
			name:    "repeated component kinds keep distinct origins",
			xml:     `<View name="V" profile="pipelang.pipeview.source.v1"><Column id="root"><Label id="same" text="first"/><Label id="same" text="second"/></Column></View>`,
			tag:     `<Label id="same" text="second"/>`,
			message: "duplicate control identity",
		},
		{
			name:    "nested component",
			xml:     nested,
			tag:     `<TextField id="edit" value="{bind missing}" changed="{edited}"/>`,
			message: "unknown state missing",
		},
		{
			name:    "Unicode and CRLF multiline tag",
			xml:     multiline,
			tag:     "<TextField\r\n            id=\"edit\"\r\n            value=\"{bind missing}\"\r\n            changed=\"{edited}\"/>",
			message: "unknown state missing",
		},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			input := file(t, "Authored.xml", test.xml)
			for _, compile := range []bool{false, true} {
				var err error
				if compile {
					_, err = markup.CompileProgram(input, nil)
				} else {
					_, err = markup.ToSource(input)
				}
				var failure *pipelang.DiagnosticError
				if err == nil || !errors.As(err, &failure) || len(failure.Diagnostics) == 0 {
					t.Fatalf("missing typed authored diagnostic: %v", err)
				}
				diagnostic := failure.Diagnostics[0]
				start := strings.Index(test.xml, test.tag)
				if test.lastOccurrence {
					start = strings.LastIndex(test.xml, test.tag)
				}
				if start < 0 {
					t.Fatal("expected authored tag missing from fixture")
				}
				expected := pipelang.Span{File: input.ID, Start: start, End: start + len(test.tag)}
				if diagnostic.Primary != expected || diagnostic.Category != pipelang.CategorySemantic ||
					diagnostic.Code != pipelang.CodeExpressionType || diagnostic.Severity != pipelang.SeverityError ||
					!strings.Contains(diagnostic.Message, test.message) {
					t.Fatalf("wrong authored diagnostic: got=%+v expected=%+v", diagnostic, expected)
				}
				resolved := pipelang.ResolveDiagnostics(failure.Sources, failure.Diagnostics)[0]
				if resolved.Primary.File != input.ID || resolved.Primary.Start != input.Position(start) ||
					resolved.Primary.End != input.Position(start+len(test.tag)) {
					t.Fatalf("authored range did not resolve exact UTF8/CRLF offsets: %+v", resolved.Primary)
				}
				if len(resolved.Related) == 0 || resolved.Related[len(resolved.Related)-1].Range.File != "Authored.xml.generated.pipe" {
					t.Fatalf("generated related range missing: %+v", resolved.Related)
				}
				if !strings.Contains(resolved.Related[len(resolved.Related)-1].Message, "containing authored start tag") {
					t.Fatal("diagnostic must disclose element-level XML precision")
				}
				if !strings.HasPrefix(err.Error(), fmt.Sprintf("Authored.xml:%d:%d:", input.Position(start).Line, input.Position(start).Column)) {
					t.Fatalf("Error did not render authored location: %v", err)
				}
			}
		})
	}
}

func TestSelectedLexerRefusalsKeepGeneratedIdentityAndCause(t *testing.T) {
	for _, category := range []pipelang.DiagnosticCategory{pipelang.CategoryLexical, pipelang.CategorySemantic} {
		t.Run(string(category), func(t *testing.T) {
			calls := 0
			var original *pipelang.DiagnosticError
			lexer := func(generated *pipelang.SourceFile) (pipelang.LexicalResult, error) {
				calls++
				sources, _ := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: generated.Path, Data: []byte(generated.Text)}})
				original = &pipelang.DiagnosticError{Sources: sources, Diagnostics: pipelang.Diagnostics{{
					Code: pipelang.CodeExpressionType, Category: category, Severity: pipelang.SeverityError,
					Message: "Explicit native boundary refusal",
					Primary: pipelang.Span{File: generated.ID, Start: 0, End: 4},
				}}}
				return pipelang.LexicalResult{}, original
			}
			_, err := markup.CompileProgram(file(t, "Native.xml", settingsXML), lexer)
			var actual *pipelang.DiagnosticError
			if err == nil || calls != 1 || !errors.Is(err, original) || !errors.As(err, &actual) || actual != original {
				t.Fatalf("selected refusal chain/call count changed: calls=%d error=%v", calls, err)
			}
			if actual.Diagnostics[0].Primary.File != "Native.xml.generated.pipe" ||
				!strings.Contains(err.Error(), "generated PipeLang diagnostic") || len(actual.Diagnostics[0].Related) != 0 {
				t.Fatal("native refusal was misattributed to authored XML")
			}
		})
	}
}
