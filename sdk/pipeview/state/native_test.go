package state_test

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
)

type nativeStep struct {
	wire    string
	action  string
	payload *pipeview.Value
	id      uint64
	outcome state.Outcome
}

// TestNativeStateReference invokes an explicitly supplied, prebuilt native
// fixture once. Compilation and containment belong to the owning workflow.
func TestNativeStateReference(t *testing.T) {
	path := os.Getenv("PIPEVIEW_NATIVE_MODEL")
	if path == "" {
		t.Skip("explicit native state model fixture required")
	}
	if !filepath.IsAbs(path) {
		t.Fatal("PIPEVIEW_NATIVE_MODEL must name an absolute prebuilt fixture path")
	}
	definition := state.Definition{
		Initial: state.Values{
			"name":    {Kind: pipeview.Text, Text: "Rider"},
			"enabled": {Kind: pipeview.Boolean, Boolean: true},
		},
		Actions: []state.Action{
			{Name: "nameEdited", Payload: pipeview.Text, Operation: state.Set, Target: "name"},
			{Name: "enabledEdited", Payload: pipeview.Boolean, Operation: state.Set, Target: "enabled"},
			{Name: "save", Payload: pipeview.Invocation, Operation: state.Save},
			{Name: "revert", Payload: pipeview.Invocation, Operation: state.Revert},
		},
	}
	model, err := state.New(definition, nil, func(values state.Values) bool {
		return values["name"].Text != ""
	})
	if err != nil {
		t.Fatal(err)
	}
	steps := nativeReferenceSteps()
	commands := make([]string, 0, len(steps))
	expected := make([]string, 0, len(steps))
	for _, step := range steps {
		commands = append(commands, step.wire)
		var request *state.SaveRequest
		var operationError error
		if step.outcome != "" {
			operationError = model.Complete(step.id, step.outcome)
		} else {
			request, operationError = model.Dispatch(step.action, step.payload)
		}
		expected = append(expected, nativeSnapshotLine(operationError == nil, model.Snapshot(), request))
	}
	before := nativeFixtureDigest(t, path)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, path)
	command.Stdin = strings.NewReader(strings.Join(commands, "\n") + "\n")
	output, runError := command.Output()
	after := nativeFixtureDigest(t, path)
	if before != after {
		t.Fatal("native state model fixture changed during execution")
	}
	if ctx.Err() != nil {
		t.Fatalf("native state model exceeded its deadline: %v", ctx.Err())
	}
	if runError != nil {
		t.Fatalf("native state model failed: %v", runError)
	}
	if len(output) > 64*1024 {
		t.Fatal("native state model output exceeded 64 KiB")
	}
	actual := strings.Split(strings.TrimSuffix(string(output), "\n"), "\n")
	if len(actual) != len(expected) {
		t.Fatalf("native output contained %d lines; expected %d", len(actual), len(expected))
	}
	for index, line := range expected {
		if actual[index] != line {
			t.Fatalf("native state differs at step %d (%q):\nGo: %s\nnative: %s", index, steps[index].wire, line, actual[index])
		}
	}
}

func nativeReferenceSteps() []nativeStep {
	return []nativeStep{
		{wire: "unknown", action: "unknown"},
		{wire: "save", action: "save"},
		nativeNameStep("Rider"),
		nativeNameStep(""),
		{wire: "save", action: "save"},
		nativeNameStep("Jamie"),
		{
			wire:    "wrong-type",
			action:  "nameEdited",
			payload: &pipeview.Value{Kind: pipeview.Boolean, Boolean: true},
		},
		{wire: "save", action: "save"},
		nativeNameStep("Busy edit"),
		nativeEnabledStep(false),
		{wire: "save", action: "save"},
		{wire: "revert", action: "revert"},
		{wire: "unknown", action: "unknown"},
		nativeCompleteStep(0, state.Success),
		nativeCompleteStep(2, state.Success),
		nativeCompleteStep(1, state.Outcome("unknown")),
		nativeCompleteStep(1, state.Failure),
		nativeCompleteStep(1, state.Success),
		{wire: "save", action: "save"},
		nativeCompleteStep(1, state.Success),
		nativeCompleteStep(2, state.Cancelled),
		{wire: "save", action: "save"},
		nativeCompleteStep(3, state.Success),
		nativeCompleteStep(3, state.Success),
		nativeEnabledStep(false),
		{wire: "revert", action: "revert"},
		nativeNameStep("Renée\tライダー\n\x00"),
		{wire: "save", action: "save"},
		nativeCompleteStep(4, state.Success),
		nativeNameStep(string([]byte{0xff})),
		nativeNameStep(string([]byte{0xc0, 0xaf})),
		nativeNameStep(string([]byte{0xe0, 0x80, 0x80})),
		nativeNameStep(string([]byte{0xed, 0xa0, 0x80})),
		nativeNameStep(string([]byte{0xf4, 0x90, 0x80, 0x80})),
		nativeNameStep(string([]byte{0xf0, 0x9f})),
		nativeNameStep("\U0010ffff"),
		nativeNameStep(strings.Repeat("x", 4097)),
		nativeNameStep(strings.Repeat("é", 2048)),
		{wire: "revert", action: "revert"},
		nativeEnabledStep(false),
		{wire: "save", action: "save"},
		nativeCompleteStep(5, state.Success),
		nativeNameStep("Final draft"),
		{wire: "save", action: "save"},
		nativeCompleteStep(6, state.Cancelled),
		{wire: "revert", action: "revert"},
	}
}

func nativeNameStep(name string) nativeStep {
	return nativeStep{
		wire:    "name " + hex.EncodeToString([]byte(name)),
		action:  "nameEdited",
		payload: &pipeview.Value{Kind: pipeview.Text, Text: name},
	}
}

func nativeEnabledStep(enabled bool) nativeStep {
	return nativeStep{
		wire:    fmt.Sprintf("enabled %d", nativeBit(enabled)),
		action:  "enabledEdited",
		payload: &pipeview.Value{Kind: pipeview.Boolean, Boolean: enabled},
	}
}

func nativeCompleteStep(id uint64, outcome state.Outcome) nativeStep {
	return nativeStep{
		wire:    fmt.Sprintf("complete %d %s", id, outcome),
		id:      id,
		outcome: outcome,
	}
}

func nativeSnapshotLine(accepted bool, snapshot state.Snapshot, request *state.SaveRequest) string {
	var requestID uint64
	if request != nil {
		requestID = request.ID
	}
	return fmt.Sprintf("%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%s\t%d\t%d",
		nativeBit(accepted), nativeBit(snapshot.Dirty), nativeBit(snapshot.Valid),
		nativeBit(snapshot.Saving), nativeBit(snapshot.CanSave),
		hex.EncodeToString([]byte(snapshot.Status)), hex.EncodeToString([]byte(snapshot.Draft["name"].Text)),
		nativeBit(snapshot.Draft["enabled"].Boolean), hex.EncodeToString([]byte(snapshot.Saved["name"].Text)),
		nativeBit(snapshot.Saved["enabled"].Boolean), requestID)
}

func nativeBit(value bool) int {
	if value {
		return 1
	}
	return 0
}

func nativeFixtureDigest(t *testing.T, path string) [sha256.Size]byte {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	digest := sha256.New()
	if _, err := io.Copy(digest, file); err != nil {
		t.Fatal(err)
	}
	var result [sha256.Size]byte
	copy(result[:], digest.Sum(nil))
	return result
}
