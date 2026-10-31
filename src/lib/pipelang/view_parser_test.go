package pipelang

import (
	"errors"
	"reflect"
	"strings"
	"testing"
)

func TestParseViewSelectedParserAdmission(t *testing.T) {
	text := `view V {
 state enabled:bool=true;
 event edited(bool);
 state title:string="Rider";
 action change(bool)=>set enabled;
 action saveSettings()=>save;
 action reset()=>revert;
 Column(id:"root") { Toggle(id:"toggle", value:bind enabled,changed:edited); Label(text:bind saveSettings.error); }
}`
	file := &SourceFile{ID: "view.pipe", Text: text}
	reference, err := ParseViewSource(file, nil)
	if err != nil {
		t.Fatal(err)
	}
	lexical, err := TokenizeSource(file)
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateViewSyntaxResult(file, lexical.Tokens, ViewSyntaxResult{Declaration: reference}); err != nil {
		t.Fatalf("reference tree refused: %v", err)
	}
	calls := 0
	selected := func(actual *SourceFile, tokens []LexicalToken) (ViewSyntaxResult, error) {
		calls++
		if actual != file || !reflect.DeepEqual(tokens, lexical.Tokens) {
			t.Fatal("parser did not receive admitted tokens")
		}
		return ViewSyntaxResult{Declaration: reference}, nil
	}
	actual, err := ParseViewSourceWithParser(file, nil, selected)
	if err != nil || !reflect.DeepEqual(actual, reference) || calls != 1 {
		t.Fatalf("selection: tree=%#v err=%v calls=%d", actual, err, calls)
	}
	componentFailure := errors.New("selected component failed")
	failed := func(*SourceFile, []LexicalToken) (ViewSyntaxResult, error) {
		return ViewSyntaxResult{}, componentFailure
	}
	if tree, err := ParseViewSourceWithParser(file, nil, failed); tree != nil || !errors.Is(err, componentFailure) {
		t.Fatalf("component failure fell back: tree=%#v err=%v", tree, err)
	}
	for _, text := range []string{"", strings.Repeat("x", 65537), "\xff", "@"} {
		file := &SourceFile{ID: "bad.pipe", Text: text}
		if tree, err := ParseViewSourceWithParser(file, nil, selected); tree != nil || err == nil {
			t.Fatalf("invalid source admitted: %v", err)
		}
	}
	if calls != 1 {
		t.Fatal("selected parser ran before source/lexical admission")
	}
}

func TestParseViewForeignTreeRefusals(t *testing.T) {
	file := &SourceFile{ID: "view.pipe", Text: `view V { state b:bool=true; state n:int=-1; action edit(bool)=>set b; action saveSettings()=>save; Column(id:"root") { Toggle(value:bind b,changed:edit); Label(text:"hello"); } }`}
	lexical, _ := TokenizeSource(file)
	mutations := map[string]func(*ViewDeclaration){
		"foreign file":       func(view *ViewDeclaration) { view.Root.Span.File = "other.pipe" },
		"unaligned span":     func(view *ViewDeclaration) { view.Root.Span.Start++ },
		"wrong name":         func(view *ViewDeclaration) { view.Name = "Other" },
		"wrong component":    func(view *ViewDeclaration) { view.Root.Component = "Wrong" },
		"wrong state type":   func(view *ViewDeclaration) { view.State[0].Type = "int" },
		"wrong state value":  func(view *ViewDeclaration) { view.State[0].Value.Boolean = false },
		"nonexclusive value": func(view *ViewDeclaration) { view.State[0].Value.Text = "hidden" },
		"unknown value kind": func(view *ViewDeclaration) { view.State[0].Value.Kind = "object" },
		"wrong integer":      func(view *ViewDeclaration) { view.State[1].Value.Integer = 1 },
		"wrong binding":      func(view *ViewDeclaration) { view.Root.Children[0].Properties[0].Value.Text = "n" },
		"unknown handler":    func(view *ViewDeclaration) { view.Actions[0].Operation = "execute" },
		"wrong target":       func(view *ViewDeclaration) { view.Actions[0].Target = "n" },
		"hidden target":      func(view *ViewDeclaration) { view.Actions[1].Target = "b" },
		"missing child":      func(view *ViewDeclaration) { view.Root.Children = view.Root.Children[:1] },
		"reordered children": func(view *ViewDeclaration) {
			view.Root.Children[0], view.Root.Children[1] = view.Root.Children[1], view.Root.Children[0]
		},
		"reordered states":     func(view *ViewDeclaration) { view.State[0], view.State[1] = view.State[1], view.State[0] },
		"overlapping children": func(view *ViewDeclaration) { view.Root.Children[1] = view.Root.Children[0] },
		"extra declaration":    func(view *ViewDeclaration) { view.State = append(view.State, view.State[0]) },
		"duplicate property": func(view *ViewDeclaration) {
			view.Root.Properties = append(view.Root.Properties, view.Root.Properties[0])
		},
	}
	for name, mutate := range mutations {
		t.Run(name, func(t *testing.T) {
			view, err := ParseViewSource(file, nil)
			if err != nil {
				t.Fatal(err)
			}
			mutate(view)
			if err := ValidateViewSyntaxResult(file, lexical.Tokens, ViewSyntaxResult{Declaration: view}); err == nil {
				t.Fatal("malformed foreign tree admitted")
			}
		})
	}
	view, _ := ParseViewSource(file, nil)
	failure := &ViewSyntaxFailure{Code: CodeExpectedToken, Message: "expected view", Span: Span{File: file.ID, Start: 0, End: 4}}
	for _, result := range []ViewSyntaxResult{{}, {Declaration: view, Failure: failure}, {Failure: &ViewSyntaxFailure{Code: CodeExpressionType, Message: "semantic", Span: failure.Span}}, {Failure: &ViewSyntaxFailure{Code: CodeExpectedToken, Message: "", Span: failure.Span}}} {
		if err := ValidateViewSyntaxResult(file, lexical.Tokens, result); err == nil {
			t.Fatal("invalid outcome admitted")
		}
	}
	selected := func(*SourceFile, []LexicalToken) (ViewSyntaxResult, error) {
		return ViewSyntaxResult{Failure: failure}, nil
	}
	tree, err := ParseViewSourceWithParser(file, nil, selected)
	diagnostics, ok := AsDiagnostics(err)
	if tree != nil || !ok || len(diagnostics) != 1 || diagnostics[0].Category != CategorySyntax || diagnostics[0].Code != failure.Code || diagnostics[0].Message != failure.Message || diagnostics[0].Primary != failure.Span {
		t.Fatalf("syntax failure projection differs: %v", err)
	}
}
