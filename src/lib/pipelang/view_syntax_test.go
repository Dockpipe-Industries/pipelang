package pipelang

import (
	"errors"
	"math"
	"reflect"
	"strings"
	"testing"
)

func parseViewFixture(t *testing.T, source string, lexer SourceLexer) (*ViewDeclaration, error) {
	t.Helper()
	file := &SourceFile{ID: "counter.pipe", Path: "counter.pipe", Text: source}
	return ParseViewSource(file, lexer)
}

func TestViewSyntaxIndependentTree(t *testing.T) {
	source := `view Counter {
 state enabled: bool = true;
 state count: int = -9223372036854775808;
 state name: string = "Rider\n🚲";
 event enabledEdited(bool);
 event increment();
 Column(id: "root") {
  Label(text: "Count: ", value: bind count);
  Button(clicked: increment);
 }
}`
	view, err := parseViewFixture(t, source, nil)
	if err != nil {
		t.Fatal(err)
	}
	if view.Name != "Counter" || view.Span != (Span{File: "counter.pipe", Start: 0, End: len(source)}) {
		t.Fatalf("declaration = %#v", view)
	}
	if len(view.State) != 3 || len(view.Events) != 2 || len(view.Root.Children) != 2 {
		t.Fatalf("unexpected tree: %#v", view)
	}
	if view.State[0].Name != "enabled" || view.State[0].Type != "bool" || view.State[0].Value.Kind != "boolean" || !view.State[0].Value.Boolean {
		t.Fatalf("boolean state = %#v", view.State[0])
	}
	if view.State[1].Value.Integer != math.MinInt64 || view.State[1].Value.Kind != "integer" {
		t.Fatalf("integer state = %#v", view.State[1])
	}
	if view.State[2].Value.Text != "Rider\n🚲" || view.State[2].Value.Kind != "text" {
		t.Fatalf("text state = %#v", view.State[2])
	}
	if view.Events[0].Name != "enabledEdited" || view.Events[0].Payload != "bool" || view.Events[1].Payload != "" {
		t.Fatalf("events = %#v", view.Events)
	}
	if view.Root.Component != "Column" || view.Root.Properties[0].Name != "id" || view.Root.Properties[0].Value.Text != "root" {
		t.Fatalf("root = %#v", view.Root)
	}
	binding := view.Root.Children[0].Properties[1].Value
	if binding.Kind != "binding" || binding.Text != "count" || source[binding.Span.Start:binding.Span.End] != "bind count" {
		t.Fatalf("binding = %#v", binding)
	}
	reference := view.Root.Children[1].Properties[0].Value
	if reference.Kind != "reference" || reference.Text != "increment" {
		t.Fatalf("reference = %#v", reference)
	}
	checkSpan := func(span Span, expected string) {
		t.Helper()
		if span.File != "counter.pipe" || !span.IsValid() || source[span.Start:span.End] != expected {
			t.Fatalf("span %#v does not select %q", span, expected)
		}
	}
	checkSpan(view.State[0].Span, "state enabled: bool = true;")
	checkSpan(view.State[0].Value.Span, "true")
	checkSpan(view.Events[0].Span, "event enabledEdited(bool);")
	checkSpan(view.Root.Properties[0].Span, `id: "root"`)
	checkSpan(view.Root.Children[1].Span, "Button(clicked: increment);")
	checkSpan(view.Root.Span, source[strings.Index(source, "Column"):strings.LastIndex(source, "}")-1])

	selected, err := parseViewFixture(t, source, TokenizeSource)
	if err != nil || !reflect.DeepEqual(view, selected) {
		t.Fatalf("selected reference lexer differs: %#v, %v", selected, err)
	}
}

func TestViewSyntaxRejectsUnsupportedGrammar(t *testing.T) {
	cases := map[string]string{
		"missing root":        `view Demo {}`,
		"multiple roots":      `view Demo { A(); B(); }`,
		"trailing tokens":     `view Demo { A(); } view Other { B(); }`,
		"declaration after":   `view Demo { A(); state count: int = 0; }`,
		"no root terminator":  `view Demo { A() }`,
		"no state terminator": `view Demo { state count: int = 0 A(); }`,
		"duplicate property":  `view Demo { A(id:"one",id:"two"); }`,
		"expression":          `view Demo { A(value: 1 + 2); }`,
		"call":                `view Demo { A(value: increment()); }`,
		"conditional":         `view Demo { if true { A(); } }`,
		"import":              `import UI; view Demo { A(); }`,
		"float":               `view Demo { A(value: 1.5); }`,
		"state binding":       `view Demo { state count: int = bind other; A(); }`,
		"event names":         `view Demo { event changed(value: int); A(); }`,
		"event arguments":     `view Demo { event changed(int,string); A(); }`,
		"trailing comma":      `view Demo { A(id:"one",); }`,
		"binding missing":     `view Demo { A(value:bind); }`,
		"overflow":            `view Demo { A(value:9223372036854775808); }`,
		"underflow":           `view Demo { A(value:-9223372036854775809); }`,
	}
	for name, source := range cases {
		t.Run(name, func(t *testing.T) {
			view, err := parseViewFixture(t, source, nil)
			diagnostics, ok := AsDiagnostics(err)
			if view != nil || !ok || len(diagnostics) != 1 || diagnostics[0].Category != CategorySyntax || !diagnostics[0].Primary.IsValid() {
				t.Fatalf("expected located syntax refusal, got %#v, %v", view, err)
			}
		})
	}
}

func TestViewSyntaxLeavesSemanticNamesToSDK(t *testing.T) {
	view, err := parseViewFixture(t, `view Arbitrary { state x: Custom = 0; event fired(Custom); CustomWidget(custom:bind unknown, handler:missing); }`, nil)
	if err != nil || view == nil || view.State[0].Type != "Custom" || view.Root.Component != "CustomWidget" {
		t.Fatalf("semantic identities were interpreted by parser: %#v, %v", view, err)
	}
}

func TestViewSyntaxCapacity(t *testing.T) {
	for _, count := range []int{128, 129} {
		source := "view Demo { Column() {" + strings.Repeat("A();", count-1) + "} }"
		view, err := parseViewFixture(t, source, nil)
		if (err == nil) != (count == 128) || (view != nil) != (count == 128) {
			t.Fatalf("node count %d = %#v, %v", count, view, err)
		}
	}
	for _, depth := range []int{8, 9} {
		source := "view Demo { " + strings.Repeat("A() {", depth-1) + "A();" + strings.Repeat("}", depth-1) + "}"
		view, err := parseViewFixture(t, source, nil)
		if (err == nil) != (depth == 8) || (view != nil) != (depth == 8) {
			t.Fatalf("depth %d = %#v, %v", depth, view, err)
		}
	}
	for _, declaration := range []string{"state x: int = 0;", "event fired();"} {
		for _, count := range []int{128, 129} {
			view, err := parseViewFixture(t, "view Demo {"+strings.Repeat(declaration, count)+" A(); }", nil)
			if (err == nil) != (count == 128) || (view != nil) != (count == 128) {
				t.Fatalf("declaration count %d = %#v, %v", count, view, err)
			}
		}
	}
	base := "view Demo { A(); }"
	for _, size := range []int{65536, 65537} {
		source := base + strings.Repeat(" ", size-len(base))
		view, err := parseViewFixture(t, source, nil)
		if (err == nil) != (size == 65536) || (view != nil) != (size == 65536) {
			t.Fatalf("source size %d = %#v, %v", size, view, err)
		}
	}
	view, err := parseViewFixture(t, `view Demo { A(value:9223372036854775807); }`, nil)
	if err != nil || view.Root.Properties[0].Value.Integer != math.MaxInt64 {
		t.Fatalf("maximum int64 = %#v, %v", view, err)
	}
}

func TestViewSyntaxAdmissionBeforeLexer(t *testing.T) {
	for _, file := range []*SourceFile{nil, {}, {ID: "bad.pipe"}, {ID: "bad.pipe", Text: "\xff"}, {ID: "large.pipe", Text: strings.Repeat(" ", 65537)}} {
		called := false
		view, err := ParseViewSource(file, func(*SourceFile) (LexicalResult, error) {
			called = true
			return LexicalResult{}, nil
		})
		if view != nil || err == nil || called {
			t.Fatalf("invalid source reached lexer: %#v, %v, called=%v", view, err, called)
		}
	}
}

func TestViewSyntaxLexicalErrors(t *testing.T) {
	for _, source := range []string{`view Demo { A(text:"\q"); }`, `view Demo { @ }`, `view Demo { A(text:"unterminated); }`} {
		for _, lexer := range []SourceLexer{nil, TokenizeSource} {
			view, err := parseViewFixture(t, source, lexer)
			diagnostics, ok := AsDiagnostics(err)
			if view != nil || !ok || len(diagnostics) != 1 || diagnostics[0].Category != CategoryLexical || diagnostics[0].Primary.File != "counter.pipe" {
				t.Fatalf("lexical diagnostic lost: %#v, %v", view, err)
			}
		}
	}
}

func TestViewSyntaxForeignLexerFailsClosed(t *testing.T) {
	componentError := errors.New("component unavailable")
	view, err := parseViewFixture(t, `view Demo { A(); }`, func(*SourceFile) (LexicalResult, error) {
		return LexicalResult{}, componentError
	})
	if view != nil || !errors.Is(err, componentError) {
		t.Fatalf("component error was hidden: %#v, %v", view, err)
	}
	for _, malformed := range []LexicalResult{
		{},
		{Tokens: []LexicalToken{{Kind: "unknown", Start: 0, End: 1}}},
		{Tokens: []LexicalToken{{Kind: "eof", Start: 0, End: 0}}},
		{Failure: &LexicalFailure{Code: CodeInvalidEscape, Message: "bad escape", Start: -1}},
		{Failure: &LexicalFailure{Code: CodeUnexpectedToken, Message: "syntax is not lexical"}},
		{Tokens: []LexicalToken{{Kind: "eof"}}, Failure: &LexicalFailure{Code: CodeUnexpectedChar, Message: "mixed result"}},
	} {
		view, err := parseViewFixture(t, `view Demo { A(); }`, func(*SourceFile) (LexicalResult, error) {
			return malformed, nil
		})
		if view != nil || err == nil {
			t.Fatalf("malformed foreign result accepted: %#v, %v", view, err)
		}
	}
	failure := &LexicalFailure{Code: CodeUnexpectedChar, Message: "foreign refusal", Start: 2, End: 3}
	_, err = parseViewFixture(t, `view Demo { A(); }`, func(*SourceFile) (LexicalResult, error) {
		return LexicalResult{Failure: failure}, nil
	})
	diagnostics, ok := AsDiagnostics(err)
	if !ok || len(diagnostics) != 1 || diagnostics[0].Code != failure.Code || diagnostics[0].Message != failure.Message || diagnostics[0].Primary != (Span{File: "counter.pipe", Start: 2, End: 3}) {
		t.Fatalf("foreign lexical diagnostic changed: %v", err)
	}
}
