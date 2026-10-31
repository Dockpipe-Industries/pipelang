package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestViewParserCLISelectorRefusals(t *testing.T) {
	directory := t.TempDir()
	source := filepath.Join(directory, "view.pipe")
	output := filepath.Join(directory, "view.h")
	if err := os.WriteFile(source, []byte(`view V { Label(id:"title",text:"Settings"); }`), 0600); err != nil {
		t.Fatal(err)
	}
	base := []string{"--source", source, "--output", output}
	for _, selection := range [][]string{
		{"--cpp-view-parser", "/missing"},
		{"--cpp-view-parser-sha256", strings.Repeat("0", 64)},
		{"--cpp-view-parser", "/missing", "--cpp-view-parser-sha256", strings.Repeat("0", 64)},
		{"--cpp-view-parser", "/missing", "--cpp-view-parser-sha256", strings.Repeat("0", 64), "--syntax", "xml"},
		{"--cpp-view-parser", "/missing", "--cpp-view-parser-sha256", strings.Repeat("0", 64), "--emit", "xml"},
	} {
		if err := run(append(append([]string(nil), base...), selection...)); err == nil {
			t.Fatalf("invalid selection succeeded: %v", selection)
		}
		for _, path := range []string{output, output + ".json"} {
			if _, err := os.Stat(path); !os.IsNotExist(err) {
				t.Fatalf("refusal emitted output %s", path)
			}
		}
	}
}

func TestNativeViewParserCLIArtifacts(t *testing.T) {
	parserPath := os.Getenv("PIPELANG_CPP_VIEW_PARSER")
	if parserPath == "" {
		t.Skip("explicit native parser required")
	}
	parserBytes, err := os.ReadFile(parserPath)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(parserBytes)
	parserHash := hex.EncodeToString(digest[:])
	cases := []string{
		`view V { state volume:int=100; action edit(int)=>set volume; PercentStepper(id:"volume",label:"Volume",value:bind volume,changed:edit); }`,
		`view V { state b:bool=true; event e(bool); Toggle(id:"toggle",value:bind b,changed:e); }`,
		`view V { state s:string="Rider🚲"; action edit(string)=>set s; action saveSettings()=>save; action revertSettings()=>revert; Column(id:"root") { TextField(id:"s",value:bind s,changed:edit); Button(id:"save",clicked:saveSettings); Button(id:"revert",clicked:revertSettings); Label(id:"status",value:bind saveSettings.status); } }`,
	}
	for _, text := range cases {
		for _, format := range []string{"header", "pipelang"} {
			directory := t.TempDir()
			source := filepath.Join(directory, "view.pipe")
			if err := os.WriteFile(source, []byte(text), 0600); err != nil {
				t.Fatal(err)
			}
			goOutput := filepath.Join(directory, "go-output")
			nativeOutput := filepath.Join(directory, "native-output")
			if err := run([]string{"--source", source, "--output", goOutput, "--emit", format}); err != nil {
				t.Fatal(err)
			}
			if err := run([]string{"--source", source, "--output", nativeOutput, "--emit", format, "--cpp-view-parser", parserPath, "--cpp-view-parser-sha256", parserHash}); err != nil {
				t.Fatal(err)
			}
			expected, err := os.ReadFile(goOutput)
			if err != nil {
				t.Fatal(err)
			}
			actual, err := os.ReadFile(nativeOutput)
			if err != nil || string(expected) != string(actual) {
				t.Fatalf("generated %s changed: %v", format, err)
			}
			for _, path := range []string{goOutput, nativeOutput} {
				data, err := os.ReadFile(path + ".json")
				if err != nil {
					t.Fatal(err)
				}
				var receipt map[string]any
				if err := json.Unmarshal(data, &receipt); err != nil {
					t.Fatal(err)
				}
				identity, found := receipt["view_parser_executable_sha256"]
				if path == nativeOutput && (!found || identity != parserHash) {
					t.Fatal("selected parser identity missing from completion receipt")
				}
				if path == goOutput && found {
					t.Fatal("default receipt gained native parser identity")
				}
			}
		}
	}
}
