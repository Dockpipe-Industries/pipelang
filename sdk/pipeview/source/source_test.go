package source_test

import (
	"errors"
	"math"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func compileFixture(text string, lexer pipelang.SourceLexer) (pipeview.Document, error) {
	return source.Compile(&pipelang.SourceFile{ID: "settings.pipe", Path: "settings.pipe", Text: text}, lexer)
}

func TestSettingsSourceProducesPortableSnapshot(t *testing.T) {
	text := `view Settings {
 state name: string = "Rider";
 state vsync: bool = true;
 event nameEdited(string);
 event vsyncEdited(bool);
 event revert();
 Column(id: "settings") {
  Label(id: "title", text: "Settings");
  TextField(id: "name", value: bind name, changed: nameEdited);
  Toggle(id: "vsync", label: "VSync", value: bind vsync, changed: vsyncEdited);
  Button(id: "revert", label: "Revert", clicked: revert);
 }
}`
	expected := pipeview.Snapshot{
		Contract: "pipelang.pipeview.declaration.v1",
		Root: pipeview.Element{ID: "settings", Kind: "column", Children: []pipeview.Element{
			{ID: "title", Kind: "label", Text: "Settings"},
			{ID: "name", Kind: "text-field", Binding: "name", Event: "nameEdited"},
			{ID: "vsync", Kind: "toggle", Text: "VSync", Binding: "vsync", Event: "vsyncEdited"},
			{ID: "revert", Kind: "button", Text: "Revert", Event: "revert"},
		}},
		State: map[string]pipeview.Value{
			"name":  {Kind: "string", Text: "Rider"},
			"vsync": {Kind: "bool", Boolean: true},
		},
		Events: map[string]pipeview.EventInfo{
			"nameEdited":  {Payload: "string", Target: "name"},
			"vsyncEdited": {Payload: "bool", Target: "vsync"},
			"revert":      {Payload: "invocation"},
		},
	}
	for _, lexer := range []pipelang.SourceLexer{nil, pipelang.TokenizeSource} {
		document, err := compileFixture(text, lexer)
		if err != nil {
			t.Fatal(err)
		}
		actual, err := document.Inspect()
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(actual, expected) {
			t.Fatalf("snapshot mismatch:\n got %#v\nwant %#v", actual, expected)
		}
	}
}

func TestSourcePreservesScalarDefaultsAndSharedBindings(t *testing.T) {
	text := `view Values {
 state low: int = -9223372036854775808;
 state high: int = 9223372036854775807;
 state enabled: bool = false;
 state name: string = "Rider\n🏍";
 event edited(bool);
 Row(id:"root") {
  Label(id:"low",value:bind low);
  Label(id:"high",value:bind high);
  Label(id:"name",text:"Name: ",value:bind name);
  Toggle(id:"first",value:bind enabled,changed:edited);
  Toggle(id:"second",value:bind enabled,changed:edited);
 }
}`
	document, err := compileFixture(text, nil)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := document.Inspect()
	if err != nil {
		t.Fatal(err)
	}
	if snapshot.State["low"].Integer != math.MinInt64 || snapshot.State["high"].Integer != math.MaxInt64 || snapshot.State["enabled"].Boolean || snapshot.State["name"].Text != "Rider\n🏍" {
		t.Fatalf("scalar defaults changed: %#v", snapshot.State)
	}
	if len(snapshot.Events) != 1 || snapshot.Events["edited"] != (pipeview.EventInfo{Payload: "bool", Target: "enabled"}) || snapshot.Root.Children[2].Text != "Name: " {
		t.Fatalf("shared declaration changed: %#v", snapshot)
	}
}

func TestSourceSemanticFailuresHaveExactOffenderSpans(t *testing.T) {
	cases := []struct {
		name     string
		text     string
		message  string
		selected string
	}{
		{"unknown state type", `view X { state a: float = 1; Label(id:"x"); }`, "unsupported view state type", "state a: float = 1;"},
		{"wrong bool default", `view X { state a: bool = "bad"; Label(id:"x"); }`, "initial value does not match bool", `"bad"`},
		{"wrong int default", `view X { state a: int = true; Label(id:"x"); }`, "initial value does not match int", "true"},
		{"wrong string default", `view X { state a: string = 42; Label(id:"x"); }`, "initial value does not match string", "42"},
		{"duplicate state", `view X { state a:int=1; state a:int=2; Label(id:"x"); }`, "duplicate declaration a", "state a:int=2;"},
		{"duplicate event", `view X { event a(); event a(); Label(id:"x"); }`, "duplicate declaration a", "event a();"},
		{"state event collision", `view X { state a:int=1; event a(); Label(id:"x"); }`, "duplicate declaration a", "event a();"},
		{"unknown payload", `view X { event a(float); Label(id:"x"); }`, "unsupported event payload", "event a(float);"},
		{"unknown component", `view X { Widget(id:"x"); }`, "unsupported PipeView component", `Widget(id:"x");`},
		{"unknown property", `view X { Label(id:"x",colour:"red"); }`, "unsupported property", `colour:"red"`},
		{"container property", `view X { Column(id:"x",value:bind missing) {} }`, "unsupported property", "value:bind missing"},
		{"duplicate control", `view X { Row(id:"r") { Label(id:"x"); Label(id:"x"); } }`, "duplicate control identity", `"x"`},
		{"missing id", `view X { Label(text:"x"); }`, "missing property id", `Label(text:"x");`},
		{"id type", `view X { Label(id:123); }`, "id requires a text literal", "123"},
		{"label type", `view X { Label(id:"x",text:true); }`, "text requires a text literal", "true"},
		{"missing binding", `view X { event edited(bool); Toggle(id:"x",changed:edited); }`, "value requires bind", `Toggle(id:"x",changed:edited);`},
		{"literal binding", `view X { event edited(bool); Toggle(id:"x",value:true,changed:edited); }`, "value requires bind", "true"},
		{"unbound reference", `view X { state enabled:bool=true; event edited(bool); Toggle(id:"x",value:enabled,changed:edited); }`, "value requires bind", "enabled"},
		{"unknown binding", `view X { Label(id:"x",value:bind missing); }`, "unknown state missing", "bind missing"},
		{"toggle state mismatch", `view X { state a:int=1; event edited(bool); Toggle(id:"x",value:bind a,changed:edited); }`, "requires bool state", "bind a"},
		{"field state mismatch", `view X { state a:bool=true; event edited(string); TextField(id:"x",value:bind a,changed:edited); }`, "requires string state", "bind a"},
		{"missing event", `view X { Button(id:"x"); }`, "clicked requires a declared event", `Button(id:"x");`},
		{"event literal", `view X { Button(id:"x",clicked:"go"); }`, "clicked requires a declared event", `"go"`},
		{"unknown event", `view X { Button(id:"x",clicked:missing); }`, "unknown event missing", "missing"},
		{"click payload", `view X { event go(int); Button(id:"x",clicked:go); }`, "event payload does not match", "go"},
		{"toggle payload", `view X { state a:bool=true; event edited(string); Toggle(id:"x",value:bind a,changed:edited); }`, "event payload does not match", "edited"},
		{"field payload", `view X { state a:string=""; event edited(bool); TextField(id:"x",value:bind a,changed:edited); }`, "event payload does not match", "edited"},
		{"unused state", `view X { state a:int=1; Label(id:"x"); }`, "state is not bound", "state a:int=1;"},
		{"unused event", `view X { event go(); Label(id:"x"); }`, "event is not used", "event go();"},
		{"leaf children", `view X { Label(id:"x") { Label(id:"child"); } }`, "leaf component cannot contain children", `Label(id:"x") { Label(id:"child"); }`},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			document, err := compileFixture(test.text, nil)
			diagnostics, ok := pipelang.AsDiagnostics(err)
			if !ok || len(diagnostics) != 1 {
				t.Fatalf("expected one diagnostic, got %v", err)
			}
			diagnostic := diagnostics[0]
			if diagnostic.Category != pipelang.CategorySemantic || !strings.Contains(diagnostic.Message, test.message) {
				t.Fatalf("unexpected semantic diagnostic: %#v", diagnostic)
			}
			span := diagnostic.Primary
			if span.File != "settings.pipe" || !span.IsValid() || span.Start == span.End || span.End > len(test.text) || test.text[span.Start:span.End] != test.selected {
				t.Fatalf("diagnostic span %#v must select %q", span, test.selected)
			}
			if _, inspectError := document.Inspect(); inspectError == nil {
				t.Fatal("failed source compilation returned an inspectable document")
			}
		})
	}
}

func TestSourceRejectsGrammarLexicalAndAdmissionFailures(t *testing.T) {
	cases := []struct {
		name string
		text string
	}{
		{"duplicate property", `view X { Label(id:"x",id:"y"); }`},
		{"missing default", `view X { state a:bool; Label(id:"x"); }`},
		{"expression default", `view X { state a:int=1+2; Label(id:"x"); }`},
		{"invalid escape", `view X { Label(id:"x",text:"\q"); }`},
		{"unterminated string", `view X { Label(id:"x",text:"unfinished); }`},
		{"unexpected character", `view X { @ }`},
		{"invalid utf8", "view X { Label(id:\"x\",text:\"\xff\"); }"},
		{"empty", ""},
		{"oversized", strings.Repeat(" ", 65537)},
		{"missing closing brace", `view X { Column(id:"x") { Label(id:"child");`},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			_, err := compileFixture(test.text, nil)
			diagnostics, ok := pipelang.AsDiagnostics(err)
			if !ok || len(diagnostics) != 1 || diagnostics[0].Primary.File != "settings.pipe" || !diagnostics[0].Primary.IsValid() {
				t.Fatalf("expected located refusal, got %v", err)
			}
		})
	}
}

func TestSourcePropagatesSelectedLexerFailure(t *testing.T) {
	expected := errors.New("selected lexer unavailable")
	called := false
	_, err := compileFixture(`view X { Label(id:"x"); }`, func(file *pipelang.SourceFile) (pipelang.LexicalResult, error) {
		called = file.ID == "settings.pipe"
		return pipelang.LexicalResult{}, expected
	})
	if !called || !errors.Is(err, expected) {
		t.Fatalf("selected lexer failure was hidden: called=%v, error=%v", called, err)
	}
}

func TestSourceCoreValidationRetainsLocatedDiagnostic(t *testing.T) {
	cases := []string{
		`view X { Row(id:"root") { Label(id:"bad id"); } }`,
		`view X { Label(id:"x",text:"` + strings.Repeat("a", 4097) + `"); }`,
		`view X { state a:string="` + strings.Repeat("a", 4097) + `"; Label(id:"x",value:bind a); }`,
		`view X { state a:bool=true; state b:bool=false; event edited(bool); Row(id:"root") { Toggle(id:"a",value:bind a,changed:edited); Toggle(id:"b",value:bind b,changed:edited); } }`,
	}
	for _, text := range cases {
		_, err := compileFixture(text, nil)
		diagnostics, ok := pipelang.AsDiagnostics(err)
		if !ok || len(diagnostics) != 1 || diagnostics[0].Category != pipelang.CategorySemantic || diagnostics[0].Primary.File != "settings.pipe" || !diagnostics[0].Primary.IsValid() || diagnostics[0].Primary.Start == diagnostics[0].Primary.End {
			t.Fatalf("core rejection lost located diagnostic: %v", err)
		}
	}
}
