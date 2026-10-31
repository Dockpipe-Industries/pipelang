package pipelang

import (
	"fmt"
	"strings"
	"testing"
)

func TestViewActionSyntaxTreeAndQualifiedBinding(t *testing.T) {
	text := `view Settings {
 state playerName: string = "Rider";
 action nameEdited(string) => set playerName;
 action save() => save;
 action revert() => revert;
 Column(id:"settings") {
  TextField(id:"name",value:bind playerName,changed:nameEdited);
  Label(id:"status",value:bind save.status);
 }
}`
	for _, lexer := range []SourceLexer{nil, TokenizeSource} {
		view, err := parseViewFixture(t, text, lexer)
		if err != nil {
			t.Fatal(err)
		}
		if len(view.Actions) != 3 || len(view.Events) != 0 {
			t.Fatalf("unexpected declarations: %#v", view)
		}
		action := view.Actions[0]
		if action.Name != "nameEdited" || action.Payload != "string" || action.Operation != "set" || action.Target != "playerName" {
			t.Fatalf("incorrect action: %#v", action)
		}
		if text[action.Span.Start:action.Span.End] != "action nameEdited(string) => set playerName;" {
			t.Fatalf("incorrect action span: %#v", action.Span)
		}
		if view.Actions[1].Payload != "" || view.Actions[1].Target != "" || view.Actions[1].Operation != "save" || view.Actions[2].Operation != "revert" {
			t.Fatalf("incorrect invocation actions: %#v", view.Actions)
		}
		binding := view.Root.Children[1].Properties[1].Value
		if binding.Kind != "binding" || binding.Text != "save.status" || text[binding.Span.Start:binding.Span.End] != "bind save.status" {
			t.Fatalf("incorrect computed binding: %#v", binding)
		}
	}
}

func TestViewActionSyntaxRejectsGeneralHandlers(t *testing.T) {
	cases := []string{
		`action edit(string) => set;`,
		`action edit(string) => set name.extra;`,
		`action edit(string value) => set name;`,
		`action edit(string,bool) => set name;`,
		`action edit(string) { name = value; }`,
		`action edit(string) => name = value;`,
		`action edit() => launch;`,
		`action save() => save name;`,
		`action revert() => revert();`,
		`action save() => save`,
	}
	for _, declaration := range cases {
		t.Run(declaration, func(t *testing.T) {
			_, err := parseViewFixture(t, `view X { `+declaration+` Label(id:"x"); }`, nil)
			if diagnostics, ok := AsDiagnostics(err); !ok || len(diagnostics) != 1 || diagnostics[0].Category != CategorySyntax {
				t.Fatalf("expected located syntax refusal, got %v", err)
			}
		})
	}
	for _, value := range []string{"save.status.extra", "save.", ".status"} {
		if _, err := parseViewFixture(t, `view X { Label(id:"x",value:bind `+value+`); }`, nil); err == nil {
			t.Fatalf("accepted invalid qualified binding %q", value)
		}
	}
	if _, err := parseViewFixture(t, `view X { Button(id:"x",clicked:save.status); }`, nil); err == nil {
		t.Fatal("accepted qualified non-binding reference")
	}
}

func TestViewActionSyntaxHasIndependentDeclarationLimit(t *testing.T) {
	var declarations strings.Builder
	for index := 0; index < 128; index++ {
		fmt.Fprintf(&declarations, "state s%d:bool=true; event e%d(); action a%d() => save;\n", index, index, index)
	}
	text := "view X { " + declarations.String() + ` Label(id:"x"); }`
	view, err := parseViewFixture(t, text, nil)
	if err != nil || len(view.State) != 128 || len(view.Events) != 128 || len(view.Actions) != 128 {
		t.Fatalf("independent declaration limits changed: %#v, %v", view, err)
	}
	text = "view X { " + declarations.String() + `action excess() => save; Label(id:"x"); }`
	if _, err := parseViewFixture(t, text, nil); err == nil || !strings.Contains(err.Error(), "action declaration limit") {
		t.Fatalf("expected action limit refusal, got %v", err)
	}
}
