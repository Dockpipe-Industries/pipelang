package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe"
	viewsource "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func settingsActionFixture(t *testing.T) []byte {
	t.Helper()
	path := filepath.Join("..", "..", "..", "examples", "settings", "SettingsView.pipe")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestActionExportIncludesNativeModelAndRuntimeProvenance(t *testing.T) {
	directory := t.TempDir()
	sourcePath := filepath.Join(directory, "SettingsView.pipe")
	outputPath := filepath.Join(directory, "PipeViewDocument.hpp")
	text := settingsActionFixture(t)
	if err := os.WriteFile(sourcePath, text, 0600); err != nil {
		t.Fatal(err)
	}
	arguments := []string{"--source", sourcePath, "--output", outputPath}
	if err := run(arguments); err != nil {
		t.Fatal(err)
	}
	generated, err := os.ReadFile(outputPath)
	if err != nil {
		t.Fatal(err)
	}
	for _, expected := range []string{
		"inline std::string view()",
		"inline pipelang::pipeview::state::Definition model_definition()",
		"inline std::map<std::string, std::string> status_bindings()",
		"model::ValueKind::Text, model::Operation::Set",
		"model::ValueKind::Boolean, model::Operation::Set",
		"model::ValueKind::Invocation, model::Operation::Save",
		"model::ValueKind::Invocation, model::Operation::Revert",
		"definition.initial.emplace(" + cppString("playerName") + ", " + cppString("Rider") + ");",
		"definition.initial.emplace(" + cppString("vsync") + ", true);",
		"{" + cppString("save.status") + ", " + cppString("save") + "}",
	} {
		if !bytes.Contains(generated, []byte(expected)) {
			t.Fatalf("generated action artifact is missing %q", expected)
		}
	}
	if !bytes.Contains(generated, []byte(state.NativeRuntime())) {
		t.Fatal("generated artifact does not embed the SDK native runtime verbatim")
	}
	if bytes.Contains(generated, []byte("definition.initial.emplace("+cppString("save.status"))) {
		t.Fatal("read-only status became persisted model state")
	}
	receiptBytes, err := os.ReadFile(outputPath + ".json")
	if err != nil {
		t.Fatal(err)
	}
	var receipt metadata
	if err := json.Unmarshal(receiptBytes, &receipt); err != nil {
		t.Fatal(err)
	}
	if receipt.Contract != exportContract || receipt.SourceContract != viewsource.ActionContract || receipt.ActionContract != state.Contract || receipt.Adapter != forgepipe.PercentSchema {
		t.Fatalf("incorrect action profile receipt: %#v", receipt)
	}
	if receipt.StateRuntimeSHA256 != digest([]byte(state.NativeRuntime())) || receipt.SourceSHA256 != digest(text) || receipt.GeneratedSHA256 != digest(generated) {
		t.Fatalf("action artifact digest provenance is incorrect: %#v", receipt)
	}
	if len(receipt.CompilerExecutableSHA256) != 64 || receipt.LexerExecutableSHA256 != "" {
		t.Fatalf("incorrect compiler/lexer provenance: %#v", receipt)
	}
	if err := run(arguments); err != nil {
		t.Fatal(err)
	}
	repeated, err := os.ReadFile(outputPath)
	if err != nil || !bytes.Equal(repeated, generated) {
		t.Fatalf("action export is not deterministic: %v", err)
	}
	original, err := os.ReadFile(sourcePath)
	if err != nil || !bytes.Equal(original, text) {
		t.Fatalf("action export changed its source: %v", err)
	}
}

func TestLegacyExportKeepsHeaderAndOmitsActionRuntimeMetadata(t *testing.T) {
	directory := t.TempDir()
	sourcePath := filepath.Join(directory, "Legacy.pipe")
	outputPath := filepath.Join(directory, "Legacy.hpp")
	text := []byte(`view Legacy { state enabled:bool=true; event edited(bool); Toggle(id:"toggle",value:bind enabled,changed:edited); }`)
	if err := os.WriteFile(sourcePath, text, 0600); err != nil {
		t.Fatal(err)
	}
	if err := run([]string{"--source", sourcePath, "--output", outputPath}); err != nil {
		t.Fatal(err)
	}
	generated, err := os.ReadFile(outputPath)
	if err != nil {
		t.Fatal(err)
	}
	document, err := viewsource.Compile(&pipelang.SourceFile{ID: "Legacy.pipe", Path: "Legacy.pipe", Text: string(text)}, nil)
	if err != nil {
		t.Fatal(err)
	}
	transport, err := forgepipe.Encode(document, forgepipe.Options{Scale: 1.125, Focus: "#FFAD45FF"})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(generated, header(transport)) {
		t.Fatal("v1 export changed its portable declaration header")
	}
	for _, unexpected := range []string{"model_definition", "status_bindings", "pipelang::pipeview::state"} {
		if bytes.Contains(generated, []byte(unexpected)) {
			t.Fatalf("legacy artifact acquired action runtime %q", unexpected)
		}
	}
	receiptBytes, err := os.ReadFile(outputPath + ".json")
	if err != nil {
		t.Fatal(err)
	}
	var receipt metadata
	if err := json.Unmarshal(receiptBytes, &receipt); err != nil {
		t.Fatal(err)
	}
	if receipt.SourceContract != viewsource.Contract || receipt.ActionContract != "" || receipt.StateRuntimeSHA256 != "" {
		t.Fatalf("legacy receipt acquired action metadata: %#v", receipt)
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(receiptBytes, &fields); err != nil {
		t.Fatal(err)
	}
	for _, field := range []string{"action_contract", "state_runtime_sha256"} {
		if _, exists := fields[field]; exists {
			t.Fatalf("legacy receipt contains %q", field)
		}
	}
}

func TestActionExportRefusalsPreserveSourceAndArtifacts(t *testing.T) {
	fixture := string(settingsActionFixture(t))
	cases := []struct {
		name    string
		text    string
		message string
	}{
		{"bad set payload", strings.Replace(fixture, "nameEdited(string)", "nameEdited(bool)", 1), "set action payload"},
		{"bad save payload", strings.Replace(fixture, "save() => save", "save(string) => save", 1), "require no payload"},
		{"unknown handler", strings.Replace(fixture, "=> save;", "=> launch;", 1), "unsupported view action handler"},
		{"read-only status", strings.Replace(fixture, "value: bind playerName", "value: bind save.status", 1), "read-only"},
		{"mixed event profile", strings.Replace(fixture, "action save() => save;", "event old(); action save() => save;", 1), "cannot mix"},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			for _, existing := range []bool{false, true} {
				directory := t.TempDir()
				sourcePath := filepath.Join(directory, "SettingsView.pipe")
				outputPath := filepath.Join(directory, "PipeViewDocument.hpp")
				if err := os.WriteFile(sourcePath, []byte(test.text), 0600); err != nil {
					t.Fatal(err)
				}
				if existing {
					for _, path := range []string{outputPath, outputPath + ".json"} {
						if err := os.WriteFile(path, []byte("preserved-artifact"), 0600); err != nil {
							t.Fatal(err)
						}
					}
				}
				err := run([]string{"--source", sourcePath, "--output", outputPath})
				if err == nil || !strings.Contains(err.Error(), test.message) {
					t.Fatalf("expected %q refusal, got %v", test.message, err)
				}
				for _, path := range []string{outputPath, outputPath + ".json"} {
					if existing {
						actual, err := os.ReadFile(path)
						if err != nil || string(actual) != "preserved-artifact" {
							t.Fatalf("refusal changed %s: %q, %v", path, actual, err)
						}
					} else if _, err := os.Stat(path); !os.IsNotExist(err) {
						t.Fatalf("refusal created %s: %v", path, err)
					}
				}
				original, err := os.ReadFile(sourcePath)
				if err != nil || string(original) != test.text {
					t.Fatalf("refusal changed source: %q, %v", original, err)
				}
			}
		})
	}
}
