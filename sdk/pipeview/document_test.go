package pipeview_test

import (
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
)

func TestInspectionPreservesTypedDeclaration(t *testing.T) {
	count := pipeview.State("count", int64(-23))
	enabled := pipeview.State("enabled", true)
	name := pipeview.State("name", "\ufeffRider\x00e\u0301🏍")
	root := pipeview.Column("root",
		pipeview.Label("count-label", "Count: ", count),
		pipeview.Toggle("enabled-toggle", "Enabled", enabled, pipeview.Change[bool]("enabled-edited")),
		pipeview.TextField("name-field", name, pipeview.Change[string]("name-edited")),
		pipeview.Row("actions", pipeview.Button("increment-button", "Increment", pipeview.Click("increment"))))
	document, err := pipeview.Compile(root)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := document.Inspect()
	if err != nil {
		t.Fatal(err)
	}
	expected := pipeview.Snapshot{
		Contract: "pipelang.pipeview.declaration.v1",
		Root: pipeview.Element{ID: "root", Kind: "column", Children: []pipeview.Element{
			{ID: "count-label", Kind: "label", Text: "Count: ", Binding: "count"},
			{ID: "enabled-toggle", Kind: "toggle", Text: "Enabled", Binding: "enabled", Event: "enabled-edited"},
			{ID: "name-field", Kind: "text-field", Binding: "name", Event: "name-edited"},
			{ID: "actions", Kind: "row", Children: []pipeview.Element{
				{ID: "increment-button", Kind: "button", Text: "Increment", Event: "increment"},
			}},
		}},
		State: map[string]pipeview.Value{
			"count":   {Kind: "int64", Integer: -23},
			"enabled": {Kind: "bool", Boolean: true},
			"name":    {Kind: "string", Text: "\ufeffRider\x00e\u0301🏍"},
		},
		Events: map[string]pipeview.EventInfo{
			"enabled-edited": {Payload: "bool", Target: "enabled"},
			"name-edited":    {Payload: "string", Target: "name"},
			"increment":      {Payload: "invocation"},
		},
	}
	if !reflect.DeepEqual(snapshot, expected) {
		t.Fatalf("inspection mismatch:\n got %#v\nwant %#v", snapshot, expected)
	}
}

func TestDeclarationAndInspectionOwnTheirCollections(t *testing.T) {
	children := []pipeview.Node{pipeview.Row("row", pipeview.Label("value", "", pipeview.State("count", int64(4))))}
	root := pipeview.Column("root", children...)
	children[0] = pipeview.StaticText("replacement", "changed")
	document, err := pipeview.Compile(root)
	if err != nil {
		t.Fatal(err)
	}
	first, err := document.Inspect()
	if err != nil {
		t.Fatal(err)
	}
	first.Root.Children[0].Children[0].ID = "mutated"
	first.State["count"] = pipeview.Value{Kind: pipeview.Integer, Integer: 99}
	first.Events["injected"] = pipeview.EventInfo{Payload: pipeview.Invocation}
	second, err := document.Inspect()
	if err != nil {
		t.Fatal(err)
	}
	if second.Root.Children[0].ID != "row" || second.Root.Children[0].Children[0].ID != "value" || second.State["count"].Integer != 4 || len(second.Events) != 0 {
		t.Fatalf("caller mutation changed the compiled declaration: %#v", second)
	}
}

func TestCompileRefusesInvalidDeclarations(t *testing.T) {
	invalidText := string([]byte{0xff})
	cases := map[string]pipeview.Node{
		"zero node":                  {},
		"empty identity":             pipeview.StaticText("", ""),
		"invalid identity":           pipeview.StaticText("with space", ""),
		"long identity":              pipeview.StaticText(strings.Repeat("a", 129), ""),
		"duplicate identity":         pipeview.Row("root", pipeview.StaticText("same", ""), pipeview.StaticText("same", "")),
		"zero label binding":         pipeview.Label("label", "", pipeview.Binding[int64]{}),
		"zero toggle binding":        pipeview.Toggle("toggle", "", pipeview.Binding[bool]{}, pipeview.Change[bool]("changed")),
		"zero field binding":         pipeview.TextField("field", pipeview.Binding[string]{}, pipeview.Change[string]("changed")),
		"invalid binding identity":   pipeview.Label("label", "", pipeview.State("invalid state", int64(1))),
		"zero click":                 pipeview.Button("button", "", pipeview.Event[pipeview.Unit]{}),
		"zero bool event":            pipeview.Toggle("toggle", "", pipeview.State("enabled", true), pipeview.Event[bool]{}),
		"zero text event":            pipeview.TextField("field", pipeview.State("name", ""), pipeview.Event[string]{}),
		"invalid event identity":     pipeview.Button("button", "", pipeview.Click("invalid event")),
		"invalid text":               pipeview.StaticText("label", invalidText),
		"excessive text":             pipeview.StaticText("label", strings.Repeat("a", 4097)),
		"invalid state text":         pipeview.Label("label", "", pipeview.State("name", invalidText)),
		"excessive state text":       pipeview.Label("label", "", pipeview.State("name", strings.Repeat("a", 4097))),
		"conflicting defaults":       pipeview.Row("root", pipeview.Label("a", "", pipeview.State("same", int64(1))), pipeview.Label("b", "", pipeview.State("same", int64(2)))),
		"conflicting kinds":          pipeview.Row("root", pipeview.Label("a", "", pipeview.State("same", int64(1))), pipeview.Label("b", "", pipeview.State("same", true))),
		"conflicting event payloads": pipeview.Row("root", pipeview.Button("a", "", pipeview.Click("same")), pipeview.Toggle("b", "", pipeview.State("enabled", true), pipeview.Change[bool]("same"))),
		"conflicting event targets":  pipeview.Row("root", pipeview.Toggle("a", "", pipeview.State("first", true), pipeview.Change[bool]("same")), pipeview.Toggle("b", "", pipeview.State("second", true), pipeview.Change[bool]("same"))),
	}
	for name, root := range cases {
		t.Run(name, func(t *testing.T) {
			document, err := pipeview.Compile(root)
			if err == nil {
				t.Fatal("invalid declaration was accepted")
			}
			if _, err := document.Inspect(); err == nil {
				t.Fatal("failed compilation returned an inspectable document")
			}
		})
	}
	if _, err := (pipeview.Document{}).Inspect(); err == nil {
		t.Fatal("zero document was inspectable")
	}
}

func TestDeclarationCapacityBoundaries(t *testing.T) {
	for _, count := range []int{128, 129} {
		children := make([]pipeview.Node, 0, count-1)
		for index := 1; index < count; index++ {
			children = append(children, pipeview.StaticText(fmt.Sprintf("node-%d", index), ""))
		}
		_, err := pipeview.Compile(pipeview.Column("root", children...))
		if (err == nil) != (count == 128) {
			t.Fatalf("node count %d: %v", count, err)
		}
	}
	for _, depth := range []int{8, 9} {
		root := pipeview.StaticText("leaf", "")
		for level := 1; level < depth; level++ {
			root = pipeview.Column(fmt.Sprintf("level-%d", level), root)
		}
		_, err := pipeview.Compile(root)
		if (err == nil) != (depth == 8) {
			t.Fatalf("depth %d: %v", depth, err)
		}
	}
	exactText := strings.Repeat("é", 2048)
	if _, err := pipeview.Compile(pipeview.Label("label", exactText, pipeview.State("text", exactText))); err != nil {
		t.Fatalf("4096-byte text boundary refused: %v", err)
	}
}

func TestIdenticalSharedBindingsAndEventsAreAccepted(t *testing.T) {
	binding := pipeview.State("enabled", true)
	changed := pipeview.Change[bool]("changed")
	_, err := pipeview.Compile(pipeview.Column("root",
		pipeview.Toggle("first", "", binding, changed),
		pipeview.Toggle("second", "", binding, changed)))
	if err != nil {
		t.Fatal(err)
	}
}
