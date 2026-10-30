package pipelang

import (
	"bytes"
	"math"
	"reflect"
	"strings"
	"testing"
)

func parseFormatView(t *testing.T, text string) *ViewDeclaration {
	t.Helper()
	sources, diagnostics := NewSourceSet([]SourceInput{{Path: "format.pipe", Data: []byte(text)}})
	if diagnostics.HasErrors() {
		t.Fatalf("source admission failed: %v", diagnostics)
	}
	declaration, err := ParseViewSource(sources.Files()[0], nil)
	if err != nil {
		t.Fatal(err)
	}
	return declaration
}

func viewWithoutSpans(declaration *ViewDeclaration) ViewDeclaration {
	result := *declaration
	result.Span = Span{}
	result.State = append([]ViewState(nil), declaration.State...)
	for index := range result.State {
		result.State[index].Span = Span{}
		result.State[index].Value.Span = Span{}
	}
	result.Events = append([]ViewEvent(nil), declaration.Events...)
	for index := range result.Events {
		result.Events[index].Span = Span{}
	}
	result.Actions = append([]ViewAction(nil), declaration.Actions...)
	for index := range result.Actions {
		result.Actions[index].Span = Span{}
	}
	var elementWithoutSpans func(ViewElement) ViewElement
	elementWithoutSpans = func(element ViewElement) ViewElement {
		result := element
		result.Span = Span{}
		result.Properties = append([]ViewProperty(nil), element.Properties...)
		for index := range result.Properties {
			result.Properties[index].Span = Span{}
			result.Properties[index].Value.Span = Span{}
		}
		result.Children = make([]ViewElement, 0, len(element.Children))
		for _, child := range element.Children {
			result.Children = append(result.Children, elementWithoutSpans(child))
		}
		return result
	}
	result.Root = elementWithoutSpans(declaration.Root)
	return result
}

func TestFormatViewSourceCanonicalGrammarAndRoundTrip(t *testing.T) {
	text := `view Panel { state name:string="Rider"; event edited(string); action save()=>save; Column(id:"root") { TextField(id:"name",value:bind name,changed:edited); Label(id:"status",value:bind save.status); } }`
	declaration := parseFormatView(t, text)
	original := viewWithoutSpans(declaration)
	formatted, err := FormatViewSource(declaration)
	if err != nil {
		t.Fatal(err)
	}
	expected := "view Panel {\n" +
		"    state name: string = \"Rider\";\n" +
		"    event edited(string);\n" +
		"    action save() => save;\n" +
		"    Column(id: \"root\") {\n" +
		"        TextField(id: \"name\", value: bind name, changed: edited);\n" +
		"        Label(id: \"status\", value: bind save.status);\n" +
		"    }\n" +
		"}\n"
	if string(formatted) != expected {
		t.Fatalf("unexpected canonical source:\n%s", formatted)
	}
	reparsed := parseFormatView(t, string(formatted))
	if !reflect.DeepEqual(original, viewWithoutSpans(reparsed)) || !reflect.DeepEqual(original, viewWithoutSpans(declaration)) {
		t.Fatal("formatting changed tree semantics or mutated its input")
	}
	repeated, err := FormatViewSource(reparsed)
	if err != nil || !bytes.Equal(formatted, repeated) {
		t.Fatalf("canonical formatting is not idempotent: %v", err)
	}
}

func TestFormatViewSourcePreservesGenericValuesAndDeclarations(t *testing.T) {
	text := `view Generic {
 state low:Custom=-9223372036854775808;
 state high:int=9223372036854775807;
 state enabled:bool=false;
 state text:string="rider";
 event external(CustomPayload);
 action edit(CustomPayload)=>set low;
 action persist(CustomPayload)=>save;
 action restore()=>revert;
 Widget(text:"raw",enabled:true,count:-7,handler:external,value:bind persist.status) {
  state(other:bind low);
 }
}`
	declaration := parseFormatView(t, text)
	formatted, err := FormatViewSource(declaration)
	if err != nil {
		t.Fatal(err)
	}
	reparsed := parseFormatView(t, string(formatted))
	if !reflect.DeepEqual(viewWithoutSpans(declaration), viewWithoutSpans(reparsed)) {
		t.Fatalf("generic grammar semantics changed:\n%s", formatted)
	}
	if reparsed.State[0].Value.Integer != math.MinInt64 || reparsed.State[1].Value.Integer != math.MaxInt64 {
		t.Fatal("formatter lost signed integer bounds")
	}
	if reparsed.Actions[1].Payload != "CustomPayload" || len(reparsed.Events) != 1 {
		t.Fatal("formatter interpreted SDK interaction semantics")
	}
}

func TestFormatViewSourcePreservesExactUTF8AndControlBytes(t *testing.T) {
	value := "\a\b\f\n\r\t\v\"\\\x00\x01\x1f\x7f\ufeffe\u0301🏍\u2028"
	declaration := &ViewDeclaration{
		Name: "Text",
		Root: ViewElement{Component: "Label", Properties: []ViewProperty{{
			Name: "text", Value: ViewValue{Kind: "text", Text: value},
		}}},
	}
	formatted, err := FormatViewSource(declaration)
	if err != nil {
		t.Fatal(err)
	}
	reparsed := parseFormatView(t, string(formatted))
	if actual := reparsed.Root.Properties[0].Value.Text; actual != value {
		t.Fatalf("text bytes changed: got %q, want %q", actual, value)
	}
	for _, escaped := range []string{`\a`, `\b`, `\f`, `\n`, `\r`, `\t`, `\v`, `\"`, `\\`} {
		if !bytes.Contains(formatted, []byte(escaped)) {
			t.Fatalf("formatter omitted supported escape %q", escaped)
		}
	}
	if !bytes.Contains(formatted, []byte("🏍")) || !bytes.Contains(formatted, []byte{0}) || bytes.Contains(formatted, []byte(`\u`)) || bytes.Contains(formatted, []byte(`\x`)) {
		t.Fatalf("formatter introduced unsupported escapes or changed raw bytes: %q", formatted)
	}
}

func TestFormatViewSourceRejectsInvalidOrInjectedAST(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*ViewDeclaration)
	}{
		{"empty name", func(view *ViewDeclaration) {
			view.Name = ""
		}},
		{"injected name", func(view *ViewDeclaration) {
			view.Name = "X {} view Other"
		}},
		{"unicode name", func(view *ViewDeclaration) {
			view.Name = "Ridé"
		}},
		{"reserved name", func(view *ViewDeclaration) {
			view.Name = "Class"
		}},
		{"bad state name", func(view *ViewDeclaration) {
			view.State[0].Name = "first.name"
		}},
		{"bad state type", func(view *ViewDeclaration) {
			view.State[0].Type = "string;"
		}},
		{"state reference", func(view *ViewDeclaration) {
			view.State[0].Value = ViewValue{Kind: "reference", Text: "other"}
		}},
		{"state binding", func(view *ViewDeclaration) {
			view.State[0].Value = ViewValue{Kind: "binding", Text: "other"}
		}},
		{"bad event name", func(view *ViewDeclaration) {
			view.Events[0].Name = "bad()"
		}},
		{"bad event payload", func(view *ViewDeclaration) {
			view.Events[0].Payload = "string,bool"
		}},
		{"bad action name", func(view *ViewDeclaration) {
			view.Actions[0].Name = "bad()"
		}},
		{"bad action payload", func(view *ViewDeclaration) {
			view.Actions[0].Payload = "string value"
		}},
		{"unknown handler", func(view *ViewDeclaration) {
			view.Actions[0].Operation = "launch"
		}},
		{"save with target", func(view *ViewDeclaration) {
			view.Actions[0].Target = "name"
		}},
		{"set missing target", func(view *ViewDeclaration) {
			view.Actions[0].Operation = "set"
		}},
		{"set bad target", func(view *ViewDeclaration) {
			view.Actions[0].Operation = "set"
			view.Actions[0].Target = "name.status"
		}},
		{"bad component", func(view *ViewDeclaration) {
			view.Root.Component = "Label(); Other"
		}},
		{"declaration root", func(view *ViewDeclaration) {
			view.Root.Component = "state"
		}},
		{"bad property", func(view *ViewDeclaration) {
			view.Root.Properties[0].Name = "text:"
		}},
		{"duplicate property", func(view *ViewDeclaration) {
			view.Root.Properties = append(view.Root.Properties, view.Root.Properties[0])
		}},
		{"invalid UTF8", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "text", Text: "\xff"}
		}},
		{"text inactive bool", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "text", Text: "x", Boolean: true}
		}},
		{"text inactive integer", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "text", Integer: 1}
		}},
		{"boolean inactive text", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "boolean", Text: "false"}
		}},
		{"boolean inactive integer", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "boolean", Integer: 1}
		}},
		{"integer inactive text", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "integer", Text: "7"}
		}},
		{"integer inactive bool", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "integer", Boolean: true}
		}},
		{"reference inactive bool", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "reference", Text: "edited", Boolean: true}
		}},
		{"reference bind", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "reference", Text: "bind"}
		}},
		{"reference dotted", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "reference", Text: "save.status"}
		}},
		{"binding inactive integer", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "binding", Text: "name", Integer: 1}
		}},
		{"binding repeated member", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "binding", Text: "save.status.other"}
		}},
		{"binding empty member", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value = ViewValue{Kind: "binding", Text: "save."}
		}},
		{"unknown value", func(view *ViewDeclaration) {
			view.Root.Properties[0].Value.Kind = "expression"
		}},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			declaration := parseFormatView(t, `view X { state name:string="Rider"; event edited(string); action save()=>save; Label(text:"hi"); }`)
			test.mutate(declaration)
			formatted, err := FormatViewSource(declaration)
			if err == nil || formatted != nil || len(err.Error()) > 256 {
				t.Fatalf("invalid AST returned bytes or unbounded error: %q, %v", formatted, err)
			}
		})
	}
	if formatted, err := FormatViewSource(nil); err == nil || formatted != nil {
		t.Fatalf("nil declaration formatted: %q, %v", formatted, err)
	}
}

func TestFormatViewSourceEnforcesIndependentBounds(t *testing.T) {
	for _, kind := range []string{"state", "event", "action"} {
		for _, count := range []int{128, 129} {
			declaration := &ViewDeclaration{Name: "Limits", Root: ViewElement{Component: "Leaf"}}
			for index := 0; index < count; index++ {
				switch kind {
				case "state":
					declaration.State = append(declaration.State, ViewState{Name: "value", Type: "int", Value: ViewValue{Kind: "integer"}})
				case "event":
					declaration.Events = append(declaration.Events, ViewEvent{Name: "changed"})
				case "action":
					declaration.Actions = append(declaration.Actions, ViewAction{Name: "save", Operation: "save"})
				}
			}
			_, err := FormatViewSource(declaration)
			if (err == nil) != (count == 128) {
				t.Fatalf("%s count %d: %v", kind, count, err)
			}
		}
	}
	for _, count := range []int{128, 129} {
		declaration := &ViewDeclaration{Name: "Limits", Root: ViewElement{Component: "Container"}}
		for index := 1; index < count; index++ {
			declaration.Root.Children = append(declaration.Root.Children, ViewElement{Component: "Leaf"})
		}
		_, err := FormatViewSource(declaration)
		if (err == nil) != (count == 128) {
			t.Fatalf("element count %d: %v", count, err)
		}
	}
	for _, depth := range []int{8, 9} {
		root := ViewElement{Component: "Leaf"}
		for index := 1; index < depth; index++ {
			root = ViewElement{Component: "Container", Children: []ViewElement{root}}
		}
		_, err := FormatViewSource(&ViewDeclaration{Name: "Limits", Root: root})
		if (err == nil) != (depth == 8) {
			t.Fatalf("element depth %d: %v", depth, err)
		}
	}
	declaration := &ViewDeclaration{Name: "Limits", Root: ViewElement{Component: "Label", Properties: []ViewProperty{{Name: "text", Value: ViewValue{Kind: "text"}}}}}
	baseline, err := FormatViewSource(declaration)
	if err != nil {
		t.Fatal(err)
	}
	declaration.Root.Properties[0].Value.Text = strings.Repeat("x", viewSourceLimit-len(baseline))
	formatted, err := FormatViewSource(declaration)
	if err != nil || len(formatted) != viewSourceLimit {
		t.Fatalf("exact source limit refused: length=%d, %v", len(formatted), err)
	}
	declaration.Root.Properties[0].Value.Text += "x"
	if formatted, err := FormatViewSource(declaration); err == nil || formatted != nil {
		t.Fatal("excessive formatted source was accepted")
	}
}
