package markup_test

import (
	"bytes"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/markup"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

const settingsSource = `view SettingsView {
    state playerName: string = "Rider";
    state vsync: bool = true;
    action nameEdited(string) => set playerName;
    action vsyncEdited(bool) => set vsync;
    action save() => save;
    action revert() => revert;
    Column(id: "root") {
        Label(id: "title", text: "Settings");
        TextField(id: "name-field", value: bind playerName, changed: nameEdited);
        Toggle(id: "vsync-toggle", label: "VSync", value: bind vsync, changed: vsyncEdited);
        Label(id: "save-status", value: bind save.status);
        Row(id: "buttons") {
            Button(id: "save-button", label: "Save", clicked: save);
            Button(id: "revert-button", label: "Revert", clicked: revert);
        }
    }
}`

const settingsXML = `<View name="SettingsView" profile="pipelang.pipeview.source.v2">
    <State name="playerName" type="string" value="Rider"/>
    <State name="vsync" type="bool" value="true"/>
    <Action name="nameEdited" payload="string" operation="set" target="playerName"/>
    <Action name="vsyncEdited" payload="bool" operation="set" target="vsync"/>
    <Action name="save" operation="save"/>
    <Action name="revert" operation="revert"/>
    <Column id="root">
        <Label id="title" text="Settings"/>
        <TextField id="name-field" value="{bind playerName}" changed="{nameEdited}"/>
        <Toggle id="vsync-toggle" label="VSync" value="{bind vsync}" changed="{vsyncEdited}"/>
        <Label id="save-status" value="{bind save.status}"/>
        <Row id="buttons">
            <Button id="save-button" label="Save" clicked="{save}"/>
            <Button id="revert-button" label="Revert" clicked="{revert}"/>
        </Row>
    </Column>
</View>`

func file(t *testing.T, path, text string) *pipelang.SourceFile {
	t.Helper()
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: path, Data: []byte(text)}})
	if diagnostics.HasErrors() {
		t.Fatal(diagnostics)
	}
	return sources.Files()[0]
}

func assertProgramsEqual(t *testing.T, expected, actual source.Program) {
	t.Helper()
	expectedDocument, err := expected.Document.Inspect()
	if err != nil {
		t.Fatal(err)
	}
	actualDocument, err := actual.Document.Inspect()
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(expectedDocument, actualDocument) ||
		!reflect.DeepEqual(expected.Definition, actual.Definition) ||
		!reflect.DeepEqual(expected.StatusBindings, actual.StatusBindings) {
		t.Fatalf("conversion changed semantics:\nexpected=%+v/%+v/%+v\nactual=%+v/%+v/%+v",
			expectedDocument, expected.Definition, expected.StatusBindings,
			actualDocument, actual.Definition, actual.StatusBindings)
	}
}

func TestSettingsSemanticRoundtrip(t *testing.T) {
	original := file(t, "SettingsView.pipe", settingsSource)
	expected, err := source.CompileProgram(original, nil)
	if err != nil {
		t.Fatal(err)
	}
	program, err := markup.CompileProgram(file(t, "SettingsView.xml", settingsXML), nil)
	if err != nil {
		t.Fatal(err)
	}
	assertProgramsEqual(t, expected, program)
	xml, err := markup.FromSource(original, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(xml, []byte(`profile="pipelang.pipeview.source.v2"`)) ||
		!bytes.Contains(xml, []byte(`value="{bind save.status}"`)) {
		t.Fatalf("missing typed action profile or computed binding: %s", xml)
	}
	generated, err := markup.ToSource(file(t, "Roundtrip.xml", string(xml)))
	if err != nil {
		t.Fatal(err)
	}
	roundtrip, err := source.CompileProgram(file(t, "Roundtrip.pipe", string(generated)), nil)
	if err != nil {
		t.Fatal(err)
	}
	assertProgramsEqual(t, expected, roundtrip)
	second, err := markup.FromSource(file(t, "Roundtrip.pipe", string(generated)), nil)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(xml, second) {
		t.Fatalf("XML conversion is not canonical:\n%s\n%s", xml, second)
	}
}

func TestLegacyAllControlsAndScalarRoundtrip(t *testing.T) {
	text := `view Legacy {
        state name: string = "Renée {bind name} {save} & < > \" \\ \t\n\r終";
        state enabled: bool = false;
        state minimum: int = -9223372036854775808;
        state maximum: int = 9223372036854775807;
        state empty: string = "";
        event nameEdited(string);
        event enabledEdited(bool);
        event clicked();
        Column(id: "root") {
            Label(id: "literal", text: "{bind name} {clicked}");
            TextField(id: "name", value: bind name, changed: nameEdited);
            Toggle(id: "enabled", label: "False remains false", value: bind enabled, changed: enabledEdited);
            Label(id: "minimum", value: bind minimum);
            Label(id: "maximum", value: bind maximum);
            Label(id: "empty", value: bind empty);
            Row(id: "buttons") {
                Button(id: "button", label: "{clicked}", clicked: clicked);
            }
        }
    }`
	original := file(t, "Legacy.pipe", text)
	expected, err := source.CompileProgram(original, nil)
	if err != nil {
		t.Fatal(err)
	}
	xml, err := markup.FromSource(original, nil)
	if err != nil {
		t.Fatal(err)
	}
	for _, required := range []string{
		`profile="pipelang.pipeview.source.v1"`, `value="false"`, `value=""`,
		`value="-9223372036854775808"`, `value="9223372036854775807"`,
		`&#x9;`, `&#xA;`, `&#xD;`, `text="{bind name} {clicked}"`, `label="{clicked}"`,
	} {
		if !bytes.Contains(xml, []byte(required)) {
			t.Fatalf("XML lost scalar or literal %q: %s", required, xml)
		}
	}
	actual, err := markup.CompileProgram(file(t, "Legacy.xml", string(xml)), nil)
	if err != nil {
		t.Fatal(err)
	}
	assertProgramsEqual(t, expected, actual)
}

func TestCanonicalXMLNormalizesReversedProperties(t *testing.T) {
	text := strings.Replace(settingsSource, `Label(id: "title", text: "Settings")`,
		`Label(text: "Settings", id: "title")`, 1)
	text = strings.Replace(text, `TextField(id: "name-field", value: bind playerName, changed: nameEdited)`,
		`TextField(changed: nameEdited, value: bind playerName, id: "name-field")`, 1)
	first, err := markup.FromSource(file(t, "Reversed.pipe", text), nil)
	if err != nil {
		t.Fatal(err)
	}
	converted, err := markup.ToSource(file(t, "Reversed.xml", string(first)))
	if err != nil {
		t.Fatal(err)
	}
	second, err := markup.FromSource(file(t, "Canonical.pipe", string(converted)), nil)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(first, second) {
		t.Fatalf("property order did not reach a canonical fixed point:\n%s\n%s", first, second)
	}
}

func TestXMLStandardEntitiesAndLiteralBraces(t *testing.T) {
	text := `<View name="V" profile="pipelang.pipeview.source.v1">
        <!-- Literal properties never interpret binding or action braces. -->
        <Label id="label" text="{bind name} &amp; &lt; &gt; &quot; &apos; &#x1F3CD; &#9;&#10;&#13;"/>
    </View>`
	program, err := markup.CompileProgram(file(t, "Entities.xml", text), nil)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := program.Document.Inspect()
	if err != nil {
		t.Fatal(err)
	}
	if snapshot.Root.Text != "{bind name} & < > \" ' 🏍 \t\n\r" || snapshot.Root.Binding != "" || snapshot.Root.Event != "" {
		t.Fatalf("entity decoding or literal braces changed: %+v", snapshot.Root)
	}
}

func TestStrictXMLRejections(t *testing.T) {
	minimal := `<View name="V" profile="pipelang.pipeview.source.v1"><Label id="label" text="ok"/></View>`
	cases := []struct {
		name string
		text string
	}{
		{name: "empty", text: ""},
		{name: "BOM", text: "\ufeff" + minimal},
		{name: "XML declaration", text: `<?xml version="1.0" encoding="UTF-8"?>` + minimal},
		{name: "processing instruction", text: `<?target instruction?>` + minimal},
		{name: "processing instruction after root", text: minimal + `<?target instruction?>`},
		{name: "DTD", text: `<!DOCTYPE View SYSTEM "file:///should-never-be-read">` + minimal},
		{name: "entity declaration", text: `<!DOCTYPE View [<!ENTITY attack "value">]>` + minimal},
		{name: "unknown directive", text: `<!unsupported>` + minimal},
		{name: "unknown entity", text: strings.Replace(minimal, `text="ok"`, `text="&external;"`, 1)},
		{name: "duplicate attribute", text: strings.Replace(minimal, `id="label"`, `id="label" id="again"`, 1)},
		{name: "unknown attribute", text: strings.Replace(minimal, `id="label"`, `id="label" onclick="run()"`, 1)},
		{name: "unknown root attribute", text: strings.Replace(minimal, `name="V"`, `name="V" ignored="true"`, 1)},
		{name: "default namespace", text: strings.Replace(minimal, `<View `, `<View xmlns="urn:ui" `, 1)},
		{name: "empty namespace declaration", text: strings.Replace(minimal, `<View `, `<View xmlns="" `, 1)},
		{name: "namespace prefix", text: strings.Replace(minimal, `<View `, `<View xmlns:x="urn:ui" `, 1)},
		{name: "namespaced attribute", text: strings.Replace(minimal, `id="label"`, `id="label" xml:lang="en"`, 1)},
		{name: "unbound namespace prefix", text: strings.Replace(minimal, `<Label `, `<x:Label `, 1)},
		{name: "mixed text", text: strings.Replace(minimal, `<Label `, `unowned text<Label `, 1)},
		{name: "non XML whitespace", text: "\u00a0" + minimal},
		{name: "CDATA text", text: strings.Replace(minimal, `<Label `, `<![CDATA[unowned text]]><Label `, 1)},
		{name: "multiple documents", text: minimal + minimal},
		{name: "multiple component roots", text: strings.Replace(minimal, `</View>`, `<Label id="other"/></View>`, 1)},
		{name: "missing component root", text: `<View name="V" profile="pipelang.pipeview.source.v1"/>`},
		{name: "unsupported root", text: `<Window name="V"/>`},
		{name: "unknown component", text: strings.Replace(minimal, `<Label `, `<Unknown `, 1)},
		{name: "missing profile", text: strings.Replace(minimal, ` profile="pipelang.pipeview.source.v1"`, "", 1)},
		{name: "unknown profile", text: strings.Replace(minimal, source.Contract, "future", 1)},
		{name: "v2 without actions", text: strings.Replace(minimal, source.Contract, source.ActionContract, 1)},
		{name: "v1 with actions", text: strings.Replace(settingsXML, source.ActionContract, source.Contract, 1)},
		{name: "missing name", text: strings.Replace(minimal, `name="V" `, "", 1)},
		{name: "bad view identifier", text: strings.Replace(minimal, `name="V"`, `name="run();evil"`, 1)},
		{name: "truncated", text: `<View name="V" profile="pipelang.pipeview.source.v1">`},
		{name: "mismatched end", text: strings.Replace(minimal, `</View>`, `</Wrong>`, 1)},
		{name: "declaration after root", text: strings.Replace(minimal, `</View>`, `<State name="late" type="string" value="x"/></View>`, 1)},
		{name: "declaration contains child", text: `<View name="V" profile="pipelang.pipeview.source.v1"><State name="x" type="string" value="x"><Label id="nested"/></State><Label id="label"/></View>`},
		{name: "leaf contains child", text: `<View name="V" profile="pipelang.pipeview.source.v1"><Label id="label"><Label id="nested"/></Label></View>`},
		{name: "invalid numeric character", text: strings.Replace(minimal, `text="ok"`, `text="&#0;"`, 1)},
		{name: "surrogate character", text: strings.Replace(minimal, `text="ok"`, `text="&#xD800;"`, 1)},
		{name: "decimal surrogate", text: strings.Replace(minimal, `text="ok"`, `text="&#55296;"`, 1)},
		{name: "low surrogate", text: strings.Replace(minimal, `text="ok"`, `text="&#xDFFF;"`, 1)},
		{name: "hex beyond Unicode", text: strings.Replace(minimal, `text="ok"`, `text="&#x110000;"`, 1)},
		{name: "decimal beyond Unicode", text: strings.Replace(minimal, `text="ok"`, `text="&#1114112;"`, 1)},
		{name: "unsupported binding expression", text: strings.Replace(settingsXML, `{bind playerName}`, `{playerName + 1}`, 1)},
		{name: "unsupported action expression", text: strings.Replace(settingsXML, `{nameEdited}`, `{nameEdited()}`, 1)},
		{name: "unknown state type", text: strings.Replace(settingsXML, `type="string"`, `type="float"`, 1)},
		{name: "invalid bool literal", text: strings.Replace(settingsXML, `value="true"`, `value="1"`, 1)},
		{name: "missing state value", text: strings.Replace(settingsXML, ` value="Rider"`, "", 1)},
		{name: "integer overflow", text: `<View name="V" profile="pipelang.pipeview.source.v1"><State name="n" type="int" value="9223372036854775808"/><Label id="n" value="{bind n}"/></View>`},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			input := file(t, "Authored.xml", test.text)
			if result, err := markup.ToSource(input); err == nil || result != nil {
				t.Fatalf("unsupported XML did not refuse: result=%q error=%v", result, err)
			}
		})
	}
}

func TestValidReplacementCharacterAndCommentReferences(t *testing.T) {
	cases := []struct {
		name     string
		literal  string
		expected string
	}{
		{name: "authored replacement character", literal: "\ufffd", expected: "\ufffd"},
		{name: "referenced replacement character", literal: "&#xFFFD;", expected: "\ufffd"},
		{name: "escaped reference text", literal: "&amp;#xD800;", expected: "&#xD800;"},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			text := `<View name="V" profile="pipelang.pipeview.source.v1">` +
				`<!-- Ignored examples: &#xD800; &#55296; &#x110000; &external; -->` +
				`<Label id="label" text="` + test.literal + `"/></View>`
			program, err := markup.CompileProgram(file(t, "References.xml", text), nil)
			if err != nil {
				t.Fatal(err)
			}
			snapshot, err := program.Document.Inspect()
			if err != nil {
				t.Fatal(err)
			}
			if snapshot.Root.Text != test.expected {
				t.Fatalf("valid authored text changed: actual=%q expected=%q", snapshot.Root.Text, test.expected)
			}
		})
	}
}

func TestSharedCheckerDiagnosticsIdentifyAuthoredXMLAndGeneratedSource(t *testing.T) {
	cases := []string{
		strings.Replace(settingsXML, `{bind playerName}`, `{bind missing}`, 1),
		strings.Replace(settingsXML, `payload="string"`, `payload="bool"`, 1),
		strings.Replace(settingsXML, `target="playerName"`, `target="vsync"`, 1),
		strings.Replace(settingsXML, `<Column id="root">`, `<Event name="legacy"/><Column id="root">`, 1),
		strings.Replace(settingsXML, `id="title"`, `id="root"`, 1),
	}
	for index, text := range cases {
		t.Run(fmt.Sprint(index), func(t *testing.T) {
			_, err := markup.ToSource(file(t, "Authored.xml", text))
			var diagnostic *pipelang.DiagnosticError
			if err == nil || !errors.As(err, &diagnostic) || !strings.HasPrefix(err.Error(), "Authored.xml:") {
				t.Fatalf("shared diagnostic did not identify authored XML: %v", err)
			}
			primary := diagnostic.Diagnostics[0]
			if primary.Primary.File != "Authored.xml" || len(primary.Related) == 0 ||
				primary.Related[len(primary.Related)-1].Span.File != "Authored.xml.generated.pipe" {
				t.Fatalf("shared diagnostic lost generated provenance: %+v", primary)
			}
		})
	}
	_, err := markup.ToSource(file(t, "Authored.xml", `<View unknown="x"/>`))
	var local *pipelang.DiagnosticError
	if err == nil || !errors.As(err, &local) || local.Diagnostics[0].Code != markup.CodeInvalidXML ||
		local.Diagnostics[0].Category != pipelang.CategorySource || local.Diagnostics[0].Severity != pipelang.SeverityError ||
		local.Diagnostics[0].Primary.File != "Authored.xml" || strings.Contains(err.Error(), "generated PipeLang") {
		t.Fatalf("local XML diagnostic has wrong provenance: %v", err)
	}
}

func TestXML10UnrepresentableSourceIsRefused(t *testing.T) {
	for _, forbidden := range []rune{0, 1, 7, 8, 11, 12, 14, 31, 0xfffe, 0xffff} {
		t.Run(fmt.Sprintf("U+%04X", forbidden), func(t *testing.T) {
			text := "view V { Label(id: \"label\", text: \"" + string(forbidden) + "\"); }"
			input := file(t, "Unrepresentable.pipe", text)
			if _, err := source.CompileProgram(input, nil); err != nil {
				t.Fatalf("fixture must be valid PipeLang before testing XML: %v", err)
			}
			result, err := markup.FromSource(input, nil)
			if err == nil || result != nil || !strings.Contains(err.Error(), "XML 1.0 cannot represent") {
				t.Fatalf("unrepresentable character changed instead of refusal: %q/%v", result, err)
			}
		})
	}
	for _, invalid := range []string{
		`view V { Unknown(id:"x"); }`,
		`view V { state name: string="Rider"; Label(id:"x", value:bind missing); }`,
	} {
		if result, err := markup.FromSource(file(t, "Invalid.pipe", invalid), nil); err == nil || result != nil {
			t.Fatal("source was not checked before XML conversion")
		}
	}
}

func TestBoundsAndAdmittedInputs(t *testing.T) {
	minimal := `<View name="V" profile="pipelang.pipeview.source.v1"><Label id="label"/></View>`
	atLimit := strings.Repeat(" ", markup.MaximumBytes-len(minimal)) + minimal
	if _, err := markup.ToSource(file(t, "AtLimit.xml", atLimit)); err != nil {
		t.Fatalf("exact byte limit refused: %v", err)
	}
	if result, err := markup.ToSource(file(t, "TooLarge.xml", " "+atLimit)); err == nil || result != nil {
		t.Fatal("excess XML bytes were accepted")
	}
	if result, err := markup.ToSource(nil); err == nil || result != nil {
		t.Fatal("nil input was accepted")
	}
	if result, err := markup.ToSource(&pipelang.SourceFile{Text: minimal}); err == nil || result != nil {
		t.Fatal("unadmitted input was accepted")
	}
	if result, err := markup.ToSource(&pipelang.SourceFile{ID: "Invalid.xml", Text: string([]byte{0xff})}); err == nil || result != nil {
		t.Fatal("invalid UTF8 was accepted")
	}
	for _, count := range []int{128, 129} {
		var text strings.Builder
		text.WriteString(`<View name="V" profile="pipelang.pipeview.source.v1"><Column id="root">`)
		for index := 1; index < count; index++ {
			fmt.Fprintf(&text, `<Label id="label%d"/>`, index)
		}
		text.WriteString(`</Column></View>`)
		_, err := markup.ToSource(file(t, "Nodes.xml", text.String()))
		if (err == nil) != (count == 128) {
			t.Fatalf("node count %d admission differs: %v", count, err)
		}
	}
	for _, depth := range []int{8, 9} {
		var text strings.Builder
		text.WriteString(`<View name="V" profile="pipelang.pipeview.source.v1">`)
		for index := 1; index < depth; index++ {
			fmt.Fprintf(&text, `<Column id="column%d">`, index)
		}
		text.WriteString(`<Label id="label"/>`)
		text.WriteString(strings.Repeat(`</Column>`, depth-1))
		text.WriteString(`</View>`)
		_, err := markup.ToSource(file(t, "Depth.xml", text.String()))
		if (err == nil) != (depth == 8) {
			t.Fatalf("depth %d admission differs: %v", depth, err)
		}
	}
	for _, kind := range []string{"State", "Event", "Action"} {
		var text strings.Builder
		text.WriteString(`<View name="V" profile="pipelang.pipeview.source.v2">`)
		for index := 0; index < 129; index++ {
			switch kind {
			case "State":
				fmt.Fprintf(&text, `<State name="s%d" type="string" value="x"/>`, index)
			case "Event":
				fmt.Fprintf(&text, `<Event name="e%d"/>`, index)
			case "Action":
				fmt.Fprintf(&text, `<Action name="a%d" operation="save"/>`, index)
			}
		}
		text.WriteString(`<Label id="label"/></View>`)
		if _, err := markup.ToSource(file(t, "Declarations.xml", text.String())); err == nil || !strings.Contains(err.Error(), "declaration capacity exceeded") {
			t.Fatalf("%s declaration limit not enforced locally: %v", kind, err)
		}
	}
}

func TestSelectedLexerReceivesGeneratedPipeLang(t *testing.T) {
	calls := 0
	lexer := func(input *pipelang.SourceFile) (pipelang.LexicalResult, error) {
		calls++
		if !strings.HasSuffix(input.Path, ".xml.generated.pipe") || !strings.HasPrefix(input.Text, "view ") {
			t.Fatalf("selected lexer received authored XML or wrong path: %+v", input)
		}
		return pipelang.TokenizeSource(input)
	}
	actual, err := markup.CompileProgram(file(t, "Settings.xml", settingsXML), lexer)
	if err != nil {
		t.Fatal(err)
	}
	if calls != 1 {
		t.Fatalf("selected lexer calls=%d, expected one checked lowering", calls)
	}
	expected, err := source.CompileProgram(file(t, "Settings.pipe", settingsSource), nil)
	if err != nil {
		t.Fatal(err)
	}
	assertProgramsEqual(t, expected, actual)
	refusal := fmt.Errorf("explicit selected lexer refusal")
	if result, err := markup.CompileProgram(file(t, "Settings.xml", settingsXML),
		func(*pipelang.SourceFile) (pipelang.LexicalResult, error) { return pipelang.LexicalResult{}, refusal }); err == nil || result.Definition != nil || !strings.Contains(err.Error(), refusal.Error()) {
		t.Fatal("selected lexer failure did not propagate without fallback")
	}
}
