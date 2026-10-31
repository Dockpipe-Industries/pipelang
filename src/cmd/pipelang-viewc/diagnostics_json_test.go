package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func diagnosticsFixture(t *testing.T, name, text string) ([]string, string) {
	t.Helper()
	directory := t.TempDir()
	input := filepath.Join(directory, name)
	output := filepath.Join(directory, "artifact.output")
	if err := os.WriteFile(input, []byte(text), 0600); err != nil {
		t.Fatal(err)
	}
	return []string{"--source", input, "--output", output}, output
}

func requireNoDiagnosticArtifacts(t *testing.T, output string) {
	t.Helper()
	for _, path := range []string{output, output + ".json"} {
		if _, err := os.Lstat(path); !os.IsNotExist(err) {
			t.Fatalf("failure published an artifact %s: %v", path, err)
		}
	}
}

func decodeDiagnosticReport(t *testing.T, output []byte) diagnosticReport {
	t.Helper()
	decoder := json.NewDecoder(bytes.NewReader(output))
	decoder.DisallowUnknownFields()
	var report diagnosticReport
	if err := decoder.Decode(&report); err != nil {
		t.Fatalf("stderr is not a diagnostics object: %q: %v", output, err)
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		t.Fatalf("stderr contains more than one JSON object: %q: %v", output, err)
	}
	if report.Contract != diagnosticsContract || report.Diagnostics == nil {
		t.Fatalf("incorrect diagnostics envelope: %#v", report)
	}
	return report
}

func requireToolReport(t *testing.T, report diagnosticReport, message string) {
	t.Helper()
	if report.FailureKind != "tool" || len(report.Diagnostics) != 0 || report.Message == nil ||
		!strings.Contains(*report.Message, message) {
		t.Fatalf("ordinary failure acquired a source diagnostic or lost its message: %#v", report)
	}
}

func TestJSONDiagnosticsReportMalformedXMLAsAuthoredSourceFailure(t *testing.T) {
	text := "<View name=\"Broken\" profile=\"pipelang.pipeview.source.v1\">\n    <Label id=\"title\"/>\n</View"
	arguments, output := diagnosticsFixture(t, "Broken.xml", text)
	arguments = append(arguments, "--syntax", "xml", "--diagnostics", "json")
	var stderr bytes.Buffer
	if code := runCLI(arguments, &stderr); code != 2 {
		t.Fatalf("malformed XML returned exit %d", code)
	}
	report := decodeDiagnosticReport(t, stderr.Bytes())
	if report.FailureKind != "source" || report.Message != nil || len(report.Diagnostics) != 1 {
		t.Fatalf("malformed XML lost its structured source failure: %#v", report)
	}
	diagnostic := report.Diagnostics[0]
	if diagnostic.Code != "PV1001" || diagnostic.Category != pipelang.CategorySource ||
		diagnostic.Severity != pipelang.SeverityError || diagnostic.Primary.File != "Broken.xml" ||
		diagnostic.Primary.Start.Offset < 0 || diagnostic.Primary.End.Offset > len(text) ||
		diagnostic.Primary.Start.Line < 1 || diagnostic.Primary.Start.Column < 1 || len(diagnostic.Related) != 0 {
		t.Fatalf("malformed XML diagnostic has incorrect authored provenance: %+v", diagnostic)
	}
	requireNoDiagnosticArtifacts(t, output)
}

func TestJSONDiagnosticsPreserveAuthoredAndGeneratedSemanticRanges(t *testing.T) {
	text := `<View name="Settings" profile="pipelang.pipeview.source.v2">
    <State name="enabled" type="bool" value="true"/>
    <Action name="edit" payload="string" operation="set" target="enabled"/>
    <Toggle id="enabled" label="Enabled" value="{bind enabled}" changed="{edit}"/>
</View>`
	for _, format := range []string{"header", "pipelang", "xml"} {
		t.Run(format, func(t *testing.T) {
			arguments, output := diagnosticsFixture(t, "Settings.xml", text)
			arguments = append(arguments, "--syntax", "xml", "--emit", format, "--diagnostics", "json")
			var sourceFailure *pipelang.DiagnosticError
			if err := run(arguments); !errors.As(err, &sourceFailure) {
				t.Fatalf("fixture did not produce a structured source failure: %v", err)
			}
			var stderr bytes.Buffer
			if code := runCLI(arguments, &stderr); code != 2 {
				t.Fatalf("semantic failure returned exit %d", code)
			}
			report := decodeDiagnosticReport(t, stderr.Bytes())
			if report.FailureKind != "source" || report.Message != nil ||
				!reflect.DeepEqual(report.Diagnostics, pipelang.ResolveDiagnostics(sourceFailure.Sources, sourceFailure.Diagnostics)) {
				t.Fatalf("JSON diagnostics diverged from core resolution: %#v", report)
			}
			if len(report.Diagnostics) != 1 {
				t.Fatalf("expected one diagnostic, got %#v", report.Diagnostics)
			}
			diagnostic := report.Diagnostics[0]
			if diagnostic.Code != pipelang.CodeExpressionType || diagnostic.Category != pipelang.CategorySemantic ||
				diagnostic.Severity != pipelang.SeverityError || diagnostic.Primary.File != "Settings.xml" ||
				diagnostic.Primary.Start.Line != 3 || diagnostic.Primary.Start.Column != 5 ||
				!strings.Contains(diagnostic.Message, "set action payload must match target state type") ||
				len(diagnostic.Related) == 0 {
				t.Fatalf("typed XML failure has incorrect authored provenance: %+v", diagnostic)
			}
			related := diagnostic.Related[len(diagnostic.Related)-1]
			if related.Range.File != "Settings.xml.generated.pipe" ||
				!strings.Contains(related.Message, "containing authored start tag") {
				t.Fatalf("typed XML failure lost generated related provenance: %+v", related)
			}
			requireNoDiagnosticArtifacts(t, output)
		})
	}
}

func TestJSONDiagnosticsClassifyOrdinaryFailuresAsTools(t *testing.T) {
	valid := `view V { Label(id:"title",text:"Settings"); }`
	for _, kind := range []string{"missing input", "unknown flag", "missing flag value", "native component"} {
		t.Run(kind, func(t *testing.T) {
			arguments, output := diagnosticsFixture(t, "Settings.pipe", valid)
			arguments = append([]string{"--diagnostics", "json"}, arguments...)
			message := ""
			switch kind {
			case "missing input":
				if err := os.Remove(arguments[3]); err != nil {
					t.Fatal(err)
				}
				message = "Settings.pipe"
			case "unknown flag":
				arguments = append(arguments, "--unsupported-diagnostic-flag")
				message = "flag provided but not defined"
			case "missing flag value":
				arguments = append(arguments, "--emit")
				message = "flag needs an argument"
			case "native component":
				arguments = append(arguments, "--cpp-lexer", filepath.Join(t.TempDir(), "missing-lexer"),
					"--cpp-lexer-sha256", strings.Repeat("a", 64))
				message = "native lexer"
			}
			var stderr bytes.Buffer
			if code := runCLI(arguments, &stderr); code != 2 {
				t.Fatalf("ordinary failure returned exit %d", code)
			}
			requireToolReport(t, decodeDiagnosticReport(t, stderr.Bytes()), message)
			requireNoDiagnosticArtifacts(t, output)
		})
	}
}

func TestDiagnosticsUseTheParsedModeWithoutRescanning(t *testing.T) {
	for _, arguments := range [][]string{
		{"--unsupported-diagnostic-flag", "--diagnostics", "json"},
		{"--diagnostics", "json", "--diagnostics", "text", "--unsupported-diagnostic-flag"},
		{"--diagnostics", "xml"},
		{"--diagnostics", "json", "--diagnostics", "xml"},
	} {
		var stderr bytes.Buffer
		if code := runCLI(arguments, &stderr); code != 2 || json.Valid(stderr.Bytes()) ||
			!strings.Contains(stderr.String(), "PipeLang view export:") {
			t.Fatalf("unparsed, overridden or invalid mode did not use text: args=%q stderr=%q exit=%d",
				arguments, stderr.String(), code)
		}
	}
	var stderr bytes.Buffer
	if code := runCLI([]string{"--diagnostics", "text", "--diagnostics", "json", "--unsupported-diagnostic-flag"}, &stderr); code != 2 {
		t.Fatalf("overridden JSON mode returned exit %d", code)
	}
	requireToolReport(t, decodeDiagnosticReport(t, stderr.Bytes()), "flag provided but not defined")
}

func TestTextDiagnosticsAndHelpRemainUseful(t *testing.T) {
	arguments, output := diagnosticsFixture(t, "Broken.pipe", `view V { Label(id:"title") }`)
	var stderr bytes.Buffer
	if code := runCLI(arguments, &stderr); code != 2 || !strings.HasPrefix(stderr.String(), "PipeLang view export:") ||
		!strings.Contains(stderr.String(), "Broken.pipe:") || json.Valid(stderr.Bytes()) {
		t.Fatalf("default text error behavior changed: stderr=%q exit=%d", stderr.String(), code)
	}
	requireNoDiagnosticArtifacts(t, output)
	stderr.Reset()
	if code := runCLI([]string{"--help"}, &stderr); code != 2 ||
		!strings.Contains(stderr.String(), "Usage of pipelang-viewc:") ||
		!strings.Contains(stderr.String(), "-diagnostics") || !strings.Contains(stderr.String(), "-syntax") {
		t.Fatalf("text help is no longer useful: stderr=%q exit=%d", stderr.String(), code)
	}
	stderr.Reset()
	if code := runCLI([]string{"--diagnostics", "json", "--help"}, &stderr); code != 2 {
		t.Fatalf("JSON help refusal returned exit %d", code)
	}
	requireToolReport(t, decodeDiagnosticReport(t, stderr.Bytes()), "help requested")
}

func TestSuccessfulDiagnosticsModesStaySilentAndPreserveArtifacts(t *testing.T) {
	const text = `view V { Label(id:"title",text:"Settings"); }`
	var baselineHeader, baselineReceipt []byte
	for _, mode := range []string{"text", "json"} {
		arguments, output := diagnosticsFixture(t, "Settings.pipe", text)
		arguments = append(arguments, "--diagnostics", mode)
		var stderr bytes.Buffer
		if code := runCLI(arguments, &stderr); code != 0 || stderr.Len() != 0 {
			t.Fatalf("successful %s export produced diagnostics: stderr=%q exit=%d", mode, stderr.String(), code)
		}
		header, err := os.ReadFile(output)
		if err != nil {
			t.Fatal(err)
		}
		receipt, err := os.ReadFile(output + ".json")
		if err != nil {
			t.Fatal(err)
		}
		if baselineHeader == nil {
			baselineHeader = header
			baselineReceipt = receipt
		} else if !bytes.Equal(header, baselineHeader) || !bytes.Equal(receipt, baselineReceipt) {
			t.Fatal("diagnostics mode changed a successful header or receipt")
		}
	}
}

func TestJSONOutputRefusalPreservesArtifactsBeforePublication(t *testing.T) {
	arguments, output := diagnosticsFixture(t, "Settings.pipe", `view V { Label(id:"title",text:"Settings"); }`)
	arguments = append(arguments, "--diagnostics", "json")
	if err := os.WriteFile(output, []byte("preserved-header"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(output+".json", 0700); err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	if code := runCLI(arguments, &stderr); code != 2 {
		t.Fatalf("nonregular receipt destination returned exit %d", code)
	}
	requireToolReport(t, decodeDiagnosticReport(t, stderr.Bytes()), "output is not a regular file")
	header, err := os.ReadFile(output)
	if err != nil || string(header) != "preserved-header" {
		t.Fatalf("output refusal replaced an existing artifact: %q: %v", header, err)
	}
	info, err := os.Lstat(output + ".json")
	if err != nil || !info.IsDir() {
		t.Fatalf("output refusal replaced the nonregular destination: %v", err)
	}
}

func TestJSONProjectionKeepsWrappedGeneratedLexerDiagnostics(t *testing.T) {
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: "Native.xml.generated.pipe", Data: []byte("view Native {}")}})
	if diagnostics.HasErrors() {
		t.Fatal(diagnostics)
	}
	failure := &pipelang.DiagnosticError{
		Sources: sources,
		Diagnostics: pipelang.Diagnostics{{
			Code: pipelang.CodeUnexpectedChar, Category: pipelang.CategoryLexical, Severity: pipelang.SeverityError,
			Message: "Explicit native lexer diagnostic", Primary: pipelang.Span{File: "Native.xml.generated.pipe", Start: 0, End: 4},
		}},
	}
	var stderr bytes.Buffer
	writeJSONFailure(&stderr, fmt.Errorf("generated PipeLang diagnostic from XML: %w", failure))
	report := decodeDiagnosticReport(t, stderr.Bytes())
	if report.FailureKind != "source" || report.Message != nil ||
		!reflect.DeepEqual(report.Diagnostics, pipelang.ResolveDiagnostics(sources, failure.Diagnostics)) ||
		report.Diagnostics[0].Primary.File != "Native.xml.generated.pipe" || len(report.Diagnostics[0].Related) != 0 {
		t.Fatalf("wrapped native diagnostic acquired an authored origin: %#v", report)
	}
}
