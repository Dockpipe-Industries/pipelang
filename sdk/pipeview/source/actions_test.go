package source_test

import (
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

const actionSettings = `view Settings {
 state playerName: string = "Rider";
 state vsync: bool = true;
 action nameEdited(string) => set playerName;
 action vsyncEdited(bool) => set vsync;
 action save() => save;
 action revert() => revert;
 Column(id:"settings") {
  TextField(id:"name",value:bind playerName,changed:nameEdited);
  Toggle(id:"vsync",value:bind vsync,changed:vsyncEdited);
  Label(id:"status",value:bind save.status);
  Button(id:"save",clicked:save);
  Button(id:"revert",clicked:revert);
 }
}`

func compileProgramFixture(text string, lexer pipelang.SourceLexer) (source.Program, error) {
	return source.CompileProgram(&pipelang.SourceFile{ID: "actions.pipe", Path: "actions.pipe", Text: text}, lexer)
}

func TestActionSourceBuildsTypedRuntimePlanAndReadOnlyStatus(t *testing.T) {
	for _, lexer := range []pipelang.SourceLexer{nil, pipelang.TokenizeSource} {
		program, err := compileProgramFixture(actionSettings, lexer)
		if err != nil {
			t.Fatal(err)
		}
		expected := state.Definition{
			Initial: state.Values{
				"playerName": {Kind: pipeview.Text, Text: "Rider"},
				"vsync":      {Kind: pipeview.Boolean, Boolean: true},
			},
			Actions: []state.Action{
				{Name: "nameEdited", Payload: pipeview.Text, Operation: state.Set, Target: "playerName"},
				{Name: "vsyncEdited", Payload: pipeview.Boolean, Operation: state.Set, Target: "vsync"},
				{Name: "save", Payload: pipeview.Invocation, Operation: state.Save},
				{Name: "revert", Payload: pipeview.Invocation, Operation: state.Revert},
			},
		}
		if program.Definition == nil || !reflect.DeepEqual(*program.Definition, expected) {
			t.Fatalf("incorrect definition: %#v", program.Definition)
		}
		if !reflect.DeepEqual(program.StatusBindings, map[string]string{"save.status": "save"}) {
			t.Fatalf("incorrect computed bindings: %#v", program.StatusBindings)
		}
		snapshot, err := program.Document.Inspect()
		if err != nil {
			t.Fatal(err)
		}
		if snapshot.State["save.status"] != (pipeview.Value{Kind: pipeview.Text, Text: "Saved"}) || snapshot.Root.Children[2].Binding != "save.status" {
			t.Fatalf("status projection missing: %#v", snapshot)
		}
		if _, exists := program.Definition.Initial["save.status"]; exists {
			t.Fatal("computed status entered persistent state")
		}
		if snapshot.Events["nameEdited"] != (pipeview.EventInfo{Payload: pipeview.Text, Target: "playerName"}) || snapshot.Events["save"].Payload != pipeview.Invocation {
			t.Fatalf("incorrect UI requirements: %#v", snapshot.Events)
		}
	}
	if source.Contract != "pipelang.pipeview.source.v1" || source.ActionContract != "pipelang.pipeview.source.v2" {
		t.Fatal("source contracts changed")
	}
}

func TestActionSourceRefusesInvalidRuntimeAndControlContracts(t *testing.T) {
	cases := []struct {
		name    string
		text    string
		message string
	}{
		{"mixed profiles", strings.Replace(actionSettings, "action save() => save;", "action save() => save; event old();", 1), "cannot mix"},
		{"wrong set payload", strings.Replace(actionSettings, "nameEdited(string)", "nameEdited(bool)", 1), "set action payload"},
		{"missing set payload", strings.Replace(actionSettings, "nameEdited(string)", "nameEdited()", 1), "set action payload"},
		{"unknown payload", strings.Replace(actionSettings, "nameEdited(string)", "nameEdited(float)", 1), "unsupported action payload"},
		{"unknown target", strings.Replace(actionSettings, "set playerName", "set absent", 1), "unknown action state target"},
		{"save payload", strings.Replace(actionSettings, "save() => save", "save(string) => save", 1), "require no payload"},
		{"revert payload", strings.Replace(actionSettings, "revert() => revert", "revert(bool) => revert", 1), "require no payload"},
		{"wrong control payload", strings.Replace(actionSettings, "changed:nameEdited", "changed:vsyncEdited", 1), "action payload does not match"},
		{"unknown action", strings.Replace(actionSettings, "changed:nameEdited", "changed:unknown", 1), "unknown action"},
		{"duplicate action", strings.Replace(actionSettings, "action save() => save;", "action save() => save; action save() => save;", 1), "duplicate declaration"},
		{"state action collision", strings.Replace(actionSettings, "action save() => save;", "action playerName() => save;", 1), "duplicate declaration"},
		{"unused action", strings.Replace(actionSettings, "action save() => save;", "action unused() => revert; action save() => save;", 1), "action is not used"},
		{"status on field", strings.Replace(actionSettings, "value:bind playerName", "value:bind save.status", 1), "read-only"},
		{"status on toggle", strings.Replace(actionSettings, "value:bind vsync", "value:bind save.status", 1), "read-only"},
		{"unknown status owner", strings.Replace(actionSettings, "bind save.status", "bind absent.status", 1), "declared save action"},
		{"status on set", strings.Replace(actionSettings, "bind save.status", "bind nameEdited.status", 1), "declared save action"},
		{"unknown save member", strings.Replace(actionSettings, "bind save.status", "bind save.result", 1), "declared save action"},
		{"multiple saves", strings.Replace(actionSettings, "revert() => revert", "revert() => save", 1), "at most one save"},
		{"multiple reverts", strings.Replace(strings.Replace(actionSettings, "save() => save", "save() => revert", 1), `Label(id:"status",value:bind save.status);`, "", 1), "at most one save"},
		{"wrong set control target", `view X { state a:string=""; state b:string=""; action edit(string) => set b; Column(id:"root") { TextField(id:"a",value:bind a,changed:edit); Label(id:"b",value:bind b); } }`, "target must match"},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			program, err := compileProgramFixture(test.text, nil)
			diagnostics, ok := pipelang.AsDiagnostics(err)
			if !ok || len(diagnostics) != 1 || diagnostics[0].Category != pipelang.CategorySemantic || !strings.Contains(diagnostics[0].Message, test.message) {
				t.Fatalf("expected %q diagnostic, got %v", test.message, err)
			}
			span := diagnostics[0].Primary
			if span.File != "actions.pipe" || !span.IsValid() || span.Start == span.End || span.End > len(test.text) {
				t.Fatalf("invalid diagnostic span %#v", span)
			}
			if program.Definition != nil || program.StatusBindings != nil {
				t.Fatal("failed admission returned a runtime plan")
			}
			if _, err := program.Document.Inspect(); err == nil {
				t.Fatal("failed admission returned an inspectable document")
			}
		})
	}
}

func TestActionSourceAllowsOptionalSaveAndRevertAndNamedStatusOwner(t *testing.T) {
	for _, operation := range []string{"save", "revert"} {
		text := `view X { state name:string=""; action invoke() => ` + operation + `; Column(id:"root") { Label(id:"name",value:bind name); Button(id:"invoke",clicked:invoke); } }`
		if _, err := compileProgramFixture(text, nil); err != nil {
			t.Fatalf("optional %s refused: %v", operation, err)
		}
	}
	text := strings.ReplaceAll(actionSettings, "save", "persist")
	text = strings.Replace(text, "=> persist", "=> save", 1)
	program, err := compileProgramFixture(text, nil)
	if err != nil || program.StatusBindings["persist.status"] != "persist" {
		t.Fatalf("named save action status refused: %#v, %v", program, err)
	}
	text = `view X { state name:string=""; action edit(string) => set name; TextField(id:"name",value:bind name,changed:edit); }`
	if program, err := compileProgramFixture(text, nil); err != nil || len(program.StatusBindings) != 0 {
		t.Fatalf("set-only program refused: %#v, %v", program, err)
	}
}

func TestLegacyCompileRefusesActionsAndProgramPreservesLegacyDocument(t *testing.T) {
	if document, err := compileFixture(actionSettings, nil); err == nil || !strings.Contains(err.Error(), "CompileProgram") {
		t.Fatalf("Compile dropped runtime action plan: %#v, %v", document, err)
	}
	legacy := `view X { state enabled:bool=true; event edit(bool); Toggle(id:"toggle",value:bind enabled,changed:edit); }`
	document, err := compileFixture(legacy, nil)
	if err != nil {
		t.Fatal(err)
	}
	program, err := compileProgramFixture(legacy, nil)
	if err != nil {
		t.Fatal(err)
	}
	oldSnapshot, _ := document.Inspect()
	programSnapshot, _ := program.Document.Inspect()
	if !reflect.DeepEqual(oldSnapshot, programSnapshot) || program.Definition != nil || program.StatusBindings != nil {
		t.Fatalf("legacy program changed: %#v", program)
	}
	if _, err := compileProgramFixture(`view X { state name:string=""; event save(); Column(id:"root") { Label(id:"name",value:bind name); Label(id:"status",value:bind save.status); Button(id:"save",clicked:save); } }`, nil); err == nil {
		t.Fatal("legacy event profile admitted computed action status")
	}
}
