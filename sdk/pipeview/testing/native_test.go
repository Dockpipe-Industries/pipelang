package testing_test

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/inspect"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	viewtest "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/testing"
)

// The private native fixture is explicitly selected by the contained coordinator.
// It uses real Session inputs; this oracle uses typed Go widget handles over the
// same admitted .pipe declaration and the unchanged reference state model.
func TestNativeInspectionMatchesGo(t *testing.T) {
	tool := os.Getenv("PIPEVIEW_NATIVE_INSPECTOR")
	if tool == "" {
		t.Skip("explicit native inspection fixture required")
	}
	if !filepath.IsAbs(tool) {
		t.Fatal("native fixture path must be absolute")
	}
	binary, err := os.ReadFile(tool)
	if err != nil {
		t.Fatal(err)
	}
	expectedDigest := sha256.Sum256(binary)
	widgets := mountSettings(t, viewtest.Options{})
	exposed := inspect.Policy{ExposeBindings: map[string]bool{"playerName": true, "vsync": true, "volume": true, "save.status": true}}
	report := make(map[string]inspect.Frame)
	capture := func(name string, policy inspect.Policy) {
		frame, err := widgets.host.Inspect(policy)
		if err != nil {
			t.Fatal(err)
		}
		report[name] = frame
	}
	capture("initial", exposed)
	capture("default", inspect.Policy{})
	if err := widgets.name.Change("RiderX"); err != nil {
		t.Fatal(err)
	}
	if err := widgets.vsync.Change(false); err != nil {
		t.Fatal(err)
	}
	volume, err := widgets.host.PercentStepper("volume-stepper")
	if err != nil {
		t.Fatal(err)
	}
	if err := volume.Change(99); err != nil {
		t.Fatal(err)
	}
	capture("edited", exposed)
	request, err := widgets.save.Invoke()
	if err != nil || request == nil {
		t.Fatalf("Save did not issue a request: %v", err)
	}
	capture("saving", exposed)
	if err := widgets.host.Complete(request.ID, state.Success); err != nil {
		t.Fatal(err)
	}
	capture("saved", exposed)
	if err := widgets.name.Change("RiderXZ"); err != nil {
		t.Fatal(err)
	}
	if _, err := widgets.revert.Invoke(); err != nil {
		t.Fatal(err)
	}
	capture("reverted", exposed)
	expected, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	nativePath := filepath.Join(directory, "native.json")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, tool, directory, nativePath)
	output, err := command.CombinedOutput()
	if err != nil || len(output) > 65536 {
		t.Fatalf("native inspection fixture: %v %s", err, output)
	}
	info, err := os.Stat(nativePath)
	if err != nil || info.Size() > 1<<20 {
		t.Fatalf("native report missing or excessive: %v", err)
	}
	actual, err := os.ReadFile(nativePath)
	if err != nil {
		t.Fatal(err)
	}
	var referenceValue, nativeValue any
	if err := json.Unmarshal(expected, &referenceValue); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(actual, &nativeValue); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(referenceValue, nativeValue) {
		t.Fatalf("native inspection differs\nGo: %s\nC++: %s", expected, actual)
	}
	after, err := os.ReadFile(tool)
	if err != nil || sha256.Sum256(after) != expectedDigest {
		t.Fatal("native inspection tool changed")
	}
	if retained := os.Getenv("PIPEVIEW_INSPECTION_REPORT_DIR"); retained != "" {
		if !filepath.IsAbs(retained) {
			t.Fatal("report directory must be absolute")
		}
		if err := os.MkdirAll(retained, 0700); err != nil {
			t.Fatal(err)
		}
		for name, data := range map[string][]byte{"go.json": expected, "native.json": actual} {
			if err := os.WriteFile(filepath.Join(retained, name), data, 0600); err != nil {
				t.Fatal(err)
			}
		}
	}
}
