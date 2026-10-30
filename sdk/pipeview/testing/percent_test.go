package testing_test

import (
	"encoding/json"
	"math"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/inspect"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/markup"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	viewtest "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/testing"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

const percentSource = `view V {
    state volume:int=100;
    action edited(int)=>set volume;
    action save()=>save;
    action revert()=>revert;
    Column(id:"root") {
        PercentStepper(id:"volume",label:"Volume",value:bind volume,changed:edited);
        Label(id:"status",value:bind save.status);
        Button(id:"save",clicked:save);
        Button(id:"revert",clicked:revert);
    }
}`

func percentProgram(t *testing.T) source.Program {
	t.Helper()
	program, err := source.CompileProgram(&pipelang.SourceFile{ID: "percent.pipe", Text: percentSource}, nil)
	if err != nil {
		t.Fatal(err)
	}
	return program
}

func TestPercentCapabilityAndXML(t *testing.T) {
	program := percentProgram(t)
	document, err := program.Document.Inspect()
	if err != nil || document.Contract != pipeview.PercentContract {
		t.Fatalf("percent declaration capability: %v", err)
	}
	options := forgepipe.Options{Scale: 1, Focus: "#FFAD45FF"}
	wire, err := forgepipe.Encode(program.Document, options)
	if err != nil {
		t.Fatal(err)
	}
	var decoded map[string]any
	if err := json.Unmarshal(wire, &decoded); err != nil || decoded["schema"] != forgepipe.PercentSchema {
		t.Fatal("percent transport omitted its explicit capability")
	}
	xml, err := markup.FromSource(&pipelang.SourceFile{ID: "percent.pipe", Text: percentSource}, nil)
	if err != nil {
		t.Fatal(err)
	}
	roundTrip, err := markup.CompileProgram(&pipelang.SourceFile{ID: "percent.xml", Text: string(xml)}, nil)
	if err != nil {
		t.Fatal(err)
	}
	actual, err := forgepipe.Encode(roundTrip.Document, options)
	if err != nil || string(actual) != string(wire) || !reflect.DeepEqual(program.Definition, roundTrip.Definition) {
		t.Fatal("XML changed percent declaration or model plan")
	}
	legacy, err := pipeview.Compile(pipeview.Toggle("toggle", "", pipeview.State("on", true), pipeview.Change[bool]("edit")))
	if err != nil {
		t.Fatal(err)
	}
	schema, err := forgepipe.SchemaFor(legacy)
	if err != nil || schema != forgepipe.Schema {
		t.Fatal("legacy capability changed")
	}
	for _, initial := range []int64{math.MinInt64, -1, 0, 100, 101, math.MaxInt64} {
		_, err := pipeview.Compile(pipeview.PercentStepper("percent", "", pipeview.State("volume", initial), pipeview.Change[int64]("edit")))
		if err != nil {
			t.Fatalf("raw canonical draft %d was clamped/refused: %v", initial, err)
		}
	}
}

func TestPercentTypingAndCanonicalHandles(t *testing.T) {
	cases := []string{
		strings.ReplaceAll(percentSource, "state volume:int=100", `state volume:string="100"`),
		strings.ReplaceAll(percentSource, "edited(int)", "edited(bool)"),
		strings.ReplaceAll(percentSource, "label:\"Volume\"", "min:0"),
		strings.ReplaceAll(percentSource, "value:bind volume", "value:100"),
		strings.ReplaceAll(percentSource, "changed:edited", "changed:unknown"),
		strings.ReplaceAll(percentSource, "state volume:int=100;", "state volume:int=100; state other:int=50;"),
	}
	for _, text := range cases {
		if _, err := source.CompileProgram(&pipelang.SourceFile{ID: "bad.pipe", Text: text}, nil); err == nil {
			t.Fatal("invalid percent declaration accepted")
		}
	}
	if _, err := pipeview.Compile(pipeview.PercentStepper("percent", "", pipeview.Binding[int64]{}, pipeview.Change[int64]("edited"))); err == nil {
		t.Fatal("zero binding accepted")
	}
	if err := (viewtest.PercentStepper{}).Change(0); err == nil {
		t.Fatal("unmounted percent handle accepted")
	}
}

func TestPercentDraftValidationSaveAndRevert(t *testing.T) {
	program := percentProgram(t)
	validate := func(values state.Values) bool {
		value := values["volume"].Integer
		return value >= 0 && value <= 100
	}
	host, err := viewtest.Mount(program.Document, *program.Definition, program.StatusBindings, viewtest.Options{Validate: validate})
	if err != nil {
		t.Fatal(err)
	}
	stepper, err := host.PercentStepper("volume")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := host.Toggle("volume"); err == nil {
		t.Fatal("wrong typed handle accepted")
	}
	save, _ := host.Button("save")
	revert, _ := host.Button("revert")
	for _, value := range []int64{math.MinInt64, -1, 101, math.MaxInt64} {
		if err := stepper.Change(value); err != nil {
			t.Fatal(err)
		}
		snapshot := host.Snapshot()
		if snapshot.Draft["volume"].Integer != value || snapshot.Valid || snapshot.CanSave || snapshot.Status != "Invalid settings" {
			t.Fatal("invalid canonical draft was not preserved")
		}
		if request, err := save.Invoke(); err == nil || request != nil {
			t.Fatal("invalid draft started Save")
		}
		frame, err := host.Inspect(inspect.Policy{ExposeBindings: map[string]bool{"volume": true}})
		if err != nil || frame.Schema != inspect.PercentContract || frame.Bindings["volume"].Type != pipeview.Integer {
			t.Fatalf("invalid draft inspection: %v", err)
		}
	}
	if _, err := revert.Invoke(); err != nil {
		t.Fatal(err)
	}
	if host.Snapshot().Draft["volume"].Integer != 100 {
		t.Fatal("Revert lost baseline")
	}
	for _, outcome := range []state.Outcome{state.Failure, state.Cancelled, state.Success} {
		if err := stepper.Change(37); err != nil {
			t.Fatal(err)
		}
		request, err := save.Invoke()
		if err != nil || request == nil {
			t.Fatal(err)
		}
		request.Snapshot["volume"] = pipeview.Value{Kind: pipeview.Integer, Integer: 1}
		if err := stepper.Change(22); err == nil {
			t.Fatal("busy edit accepted")
		}
		if err := host.Complete(request.ID+1, state.Success); err == nil {
			t.Fatal("stale completion accepted")
		}
		if err := host.Complete(request.ID, outcome); err != nil {
			t.Fatal(err)
		}
		expected := int64(100)
		if outcome == state.Success {
			expected = 37
		}
		if host.Snapshot().Saved["volume"].Integer != expected {
			t.Fatal("captured save snapshot aliased or wrong outcome applied")
		}
	}
	if err := stepper.Change(72); err != nil {
		t.Fatal(err)
	}
	if _, err := revert.Invoke(); err != nil {
		t.Fatal(err)
	}
	if host.Snapshot().Draft["volume"].Integer != 37 {
		t.Fatal("Revert used defaults instead of saved volume")
	}
}
