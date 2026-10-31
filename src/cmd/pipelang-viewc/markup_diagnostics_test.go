package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func TestXMLTypeFailureReportsAuthoredElementWithoutPublishing(t *testing.T) {
	text := `<View name="Settings" profile="pipelang.pipeview.source.v2">
    <State name="enabled" type="bool" value="true"/>
    <Action name="edit" payload="string" operation="set" target="enabled"/>
    <Toggle id="enabled" label="Enabled" value="{bind enabled}" changed="{edit}"/>
</View>`
	for _, format := range []string{"header", "pipelang", "xml"} {
		t.Run(format, func(t *testing.T) {
			directory := t.TempDir()
			input := filepath.Join(directory, "Settings.xml")
			output := filepath.Join(directory, "artifact")
			if err := os.WriteFile(input, []byte(text), 0600); err != nil {
				t.Fatal(err)
			}
			err := run([]string{"--syntax", "xml", "--source", input, "--emit", format, "--output", output})
			var diagnosticError *pipelang.DiagnosticError
			if !errors.As(err, &diagnosticError) || len(diagnosticError.Diagnostics) != 1 {
				t.Fatalf("expected one structured authored diagnostic, got %v", err)
			}
			diagnostic := diagnosticError.Diagnostics[0]
			location := diagnosticError.Sources.Range(diagnostic.Primary)
			if location.File != "Settings.xml" || location.Start.Line != 3 || location.Start.Column != 5 {
				t.Fatalf("wrong authored element: %+v", location)
			}
			if !strings.HasPrefix(err.Error(), "Settings.xml:3:5:") {
				t.Fatalf("CLI error hides authored XML location: %v", err)
			}
			for _, path := range []string{output, output + ".json"} {
				if _, err := os.Stat(path); !os.IsNotExist(err) {
					t.Fatalf("type refusal published an artifact: %s: %v", path, err)
				}
			}
		})
	}
}
