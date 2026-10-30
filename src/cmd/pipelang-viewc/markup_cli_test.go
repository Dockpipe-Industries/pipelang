package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/markup"
	viewsource "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func markupSourceFile(text []byte) *pipelang.SourceFile {
	return &pipelang.SourceFile{ID: "view.input", Path: "view.input", Text: string(text)}
}

func runMarkupExport(t *testing.T, input []byte, syntax, format string) ([]byte, []byte) {
	t.Helper()
	directory := t.TempDir()
	sourcePath := filepath.Join(directory, "view.input")
	outputPath := filepath.Join(directory, "artifact.output")
	if err := os.WriteFile(sourcePath, input, 0600); err != nil {
		t.Fatal(err)
	}
	arguments := []string{"--source", sourcePath, "--output", outputPath}
	if syntax != "" {
		arguments = append(arguments, "--syntax", syntax)
	}
	if format != "" {
		arguments = append(arguments, "--emit", format)
	}
	if err := run(arguments); err != nil {
		t.Fatal(err)
	}
	generated, err := os.ReadFile(outputPath)
	if err != nil {
		t.Fatal(err)
	}
	receipt, err := os.ReadFile(outputPath + ".json")
	if err != nil {
		t.Fatal(err)
	}
	original, err := os.ReadFile(sourcePath)
	if err != nil || !bytes.Equal(original, input) {
		t.Fatalf("export modified source: %v", err)
	}
	return generated, receipt
}

func TestMarkupAndPipeLangSettingsProduceIdenticalHeaders(t *testing.T) {
	source := settingsActionFixture(t)
	xml, err := markup.FromSource(markupSourceFile(source), nil)
	if err != nil {
		t.Fatal(err)
	}
	pipeHeader, pipeReceiptBytes := runMarkupExport(t, source, "", "")
	xmlHeader, xmlReceiptBytes := runMarkupExport(t, xml, "xml", "header")
	if !bytes.Equal(pipeHeader, xmlHeader) {
		t.Fatal("explicit XML frontend changed the settings native artifact")
	}
	var pipeReceipt, xmlReceipt metadata
	if err := json.Unmarshal(pipeReceiptBytes, &pipeReceipt); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(xmlReceiptBytes, &xmlReceipt); err != nil {
		t.Fatal(err)
	}
	if pipeReceipt.SourceSyntax != "" || pipeReceipt.OutputFormat != "" {
		t.Fatalf("default receipt acquired format fields: %#v", pipeReceipt)
	}
	var defaultFields map[string]json.RawMessage
	if err := json.Unmarshal(pipeReceiptBytes, &defaultFields); err != nil {
		t.Fatal(err)
	}
	for _, field := range []string{"source_syntax", "output_format"} {
		if _, exists := defaultFields[field]; exists {
			t.Fatalf("default receipt acquired optional field %q", field)
		}
	}
	if xmlReceipt.Contract != exportContract || xmlReceipt.SourceContract != viewsource.ActionContract ||
		xmlReceipt.SourceSyntax != "xml" || xmlReceipt.OutputFormat != "header" ||
		xmlReceipt.SourceSHA256 != digest(xml) || xmlReceipt.GeneratedSHA256 != digest(xmlHeader) ||
		xmlReceipt.ActionContract != state.Contract || xmlReceipt.StateRuntimeSHA256 != digest([]byte(state.NativeRuntime())) ||
		xmlReceipt.CompilerExecutableSHA256 != pipeReceipt.CompilerExecutableSHA256 || xmlReceipt.LexerExecutableSHA256 != "" {
		t.Fatalf("XML header provenance differs: %#v", xmlReceipt)
	}
	if pipeReceipt.SourceSHA256 != digest(source) || pipeReceipt.GeneratedSHA256 != xmlReceipt.GeneratedSHA256 {
		t.Fatal("header receipts do not distinguish original inputs from identical generated artifacts")
	}
}

func TestSourceConversionsRoundTripProfilesAndCanonicalScalars(t *testing.T) {
	fixtures := []struct {
		name     string
		text     []byte
		contract string
	}{
		{"settings", settingsActionFixture(t), viewsource.ActionContract},
		{"legacy", []byte(`view Legacy { state enabled:bool=false; event edited(bool); Toggle(id:"toggle",value:bind enabled,changed:edited); }`), viewsource.Contract},
		{"empty and false", []byte(`view Scalars {
    state name:string="";
    state enabled:bool=false;
    state count:int=-9223372036854775808;
    action editName(string) => set name;
    action editEnabled(bool) => set enabled;
    Column(id:"root") {
        TextField(id:"name",value:bind name,changed:editName);
        Toggle(id:"enabled",value:bind enabled,changed:editEnabled);
        Label(id:"count",value:bind count);
    }
}`), viewsource.ActionContract},
	}
	for _, fixture := range fixtures {
		t.Run(fixture.name, func(t *testing.T) {
			canonical, receiptBytes := runMarkupExport(t, fixture.text, "pipelang", "pipelang")
			xml, _ := runMarkupExport(t, fixture.text, "pipelang", "xml")
			roundTrip, _ := runMarkupExport(t, xml, "xml", "pipelang")
			canonicalXML, _ := runMarkupExport(t, xml, "xml", "xml")
			if !bytes.Equal(canonical, roundTrip) || !bytes.Equal(xml, canonicalXML) {
				t.Fatal("source conversions did not converge to canonical bytes")
			}
			repeated, _ := runMarkupExport(t, canonical, "pipelang", "pipelang")
			if !bytes.Equal(canonical, repeated) {
				t.Fatal("canonical source formatting is not idempotent")
			}
			originalProgram, err := viewsource.CompileProgram(markupSourceFile(fixture.text), nil)
			if err != nil {
				t.Fatal(err)
			}
			convertedProgram, err := markup.CompileProgram(markupSourceFile(xml), nil)
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(originalProgram, convertedProgram) {
				t.Fatal("round trip changed portable document or typed action definitions")
			}
			if fixture.name == "empty and false" {
				initial := convertedProgram.Definition.Initial
				if initial["name"] != (pipeview.Value{Kind: pipeview.Text, Text: ""}) ||
					initial["enabled"] != (pipeview.Value{Kind: pipeview.Boolean, Boolean: false}) ||
					initial["count"] != (pipeview.Value{Kind: pipeview.Integer, Integer: -9223372036854775808}) {
					t.Fatalf("conversion lost canonical scalar zero/boundary values: %#v", initial)
				}
			}
			var receipt conversionMetadata
			if err := json.Unmarshal(receiptBytes, &receipt); err != nil {
				t.Fatal(err)
			}
			if receipt.Contract != conversionContract || receipt.SourceContract != fixture.contract ||
				receipt.SourceSyntax != "pipelang" || receipt.OutputFormat != "pipelang" ||
				receipt.SourceSHA256 != digest(fixture.text) || receipt.GeneratedSHA256 != digest(canonical) ||
				len(receipt.CompilerExecutableSHA256) != 64 || receipt.LexerExecutableSHA256 != "" {
				t.Fatalf("conversion receipt provenance differs: %#v", receipt)
			}
			var fields map[string]json.RawMessage
			if err := json.Unmarshal(receiptBytes, &fields); err != nil {
				t.Fatal(err)
			}
			for _, field := range []string{"adapter", "action_contract", "state_runtime_sha256", "lexer_executable_sha256"} {
				if _, exists := fields[field]; exists {
					t.Fatalf("source conversion receipt contains unrelated field %q", field)
				}
			}
		})
	}
}

func TestMarkupFlagsAndSemanticRefusalsPublishNoArtifacts(t *testing.T) {
	valid := []byte(`view X { Label(id:"title",text:"Settings"); }`)
	xml, err := markup.FromSource(markupSourceFile(valid), nil)
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name      string
		text      []byte
		arguments []string
	}{
		{"unknown syntax", valid, []string{"--syntax", "auto"}},
		{"empty syntax", valid, []string{"--syntax", ""}},
		{"unknown format", valid, []string{"--emit", "json"}},
		{"empty format", valid, []string{"--emit", ""}},
		{"XML requires explicit syntax", xml, nil},
		{"PipeLang cannot masquerade as XML", valid, []string{"--syntax", "xml"}},
		{"malformed XML", []byte(`<view`), []string{"--syntax", "xml"}},
		{"invalid profile to source", []byte(`view X { Button(id:"button",clicked:missing); }`), []string{"--emit", "pipelang"}},
		{"invalid profile to XML", []byte(`view X { Button(id:"button",clicked:missing); }`), []string{"--emit", "xml"}},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			for _, existing := range []bool{false, true} {
				directory := t.TempDir()
				sourcePath := filepath.Join(directory, "view.input")
				outputPath := filepath.Join(directory, "artifact.output")
				if err := os.WriteFile(sourcePath, test.text, 0600); err != nil {
					t.Fatal(err)
				}
				if existing {
					for _, path := range []string{outputPath, outputPath + ".json"} {
						if err := os.WriteFile(path, []byte("preserved"), 0600); err != nil {
							t.Fatal(err)
						}
					}
				}
				arguments := []string{"--source", sourcePath, "--output", outputPath}
				arguments = append(arguments, test.arguments...)
				if err := run(arguments); err == nil {
					t.Fatal("invalid conversion was accepted")
				}
				for _, path := range []string{outputPath, outputPath + ".json"} {
					if existing {
						actual, err := os.ReadFile(path)
						if err != nil || string(actual) != "preserved" {
							t.Fatalf("conversion refusal changed artifact %s: %q, %v", path, actual, err)
						}
					} else if _, err := os.Lstat(path); !os.IsNotExist(err) {
						t.Fatalf("conversion refusal created artifact %s: %v", path, err)
					}
				}
			}
		})
	}
}

func TestConversionUsesExistingOutputSafetyChecks(t *testing.T) {
	for _, format := range []string{"pipelang", "xml"} {
		for _, suffix := range []string{"", ".json"} {
			t.Run(format+suffix, func(t *testing.T) {
				directory := t.TempDir()
				sourcePath := filepath.Join(directory, "view.pipe")
				outputPath := filepath.Join(directory, "artifact.output")
				targetPath := filepath.Join(directory, "preserved.txt")
				if err := os.WriteFile(sourcePath, []byte(`view X { Label(id:"title"); }`), 0600); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(targetPath, []byte("preserved"), 0600); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(targetPath, outputPath+suffix); err != nil {
					t.Fatal(err)
				}
				if err := run([]string{"--source", sourcePath, "--output", outputPath, "--emit", format}); err == nil {
					t.Fatal("conversion accepted a symlink artifact destination")
				}
				actual, err := os.ReadFile(targetPath)
				if err != nil || string(actual) != "preserved" {
					t.Fatalf("conversion changed symlink target: %q, %v", actual, err)
				}
				if err := run([]string{"--source", sourcePath, "--output", sourcePath, "--emit", format}); err == nil ||
					!strings.Contains(err.Error(), "distinct") {
					t.Fatalf("conversion did not refuse source/output identity: %v", err)
				}
			})
		}
	}
}
