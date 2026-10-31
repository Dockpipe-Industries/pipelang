package nativelexer_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/cppbackend"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/gobackend"
)

// This pipeline reuses the frozen pilot's independently checked vectors and
// adapters. It does not execute generated programs or regenerate trace adapters.
func TestNativeLexerFrozenPilotPipeline(t *testing.T) {
	root := os.Getenv("PIPELANG_LEXER_PILOT_CORPUS")
	if root == "" {
		t.Skip("set PIPELANG_LEXER_PILOT_CORPUS to compare the frozen eight-program pilot")
	}
	lexer := configuredLexer(t)
	emitterPath := os.Getenv("PIPELANG_CPP_EMITTER")
	if !filepath.IsAbs(emitterPath) {
		t.Fatal("PIPELANG_CPP_EMITTER must select an absolute native emitter")
	}
	emitter := cppbackend.NativeEmitter{Path: emitterPath, SHA256: pilotExecutableHash(t, emitterPath)}
	pilotRealPath(t, root)
	corpusBytes := pilotReadRegular(t, filepath.Join(root, "corpus.json"))
	var cases []struct {
		ID     string
		Source string
	}
	if err := json.Unmarshal(corpusBytes, &cases); err != nil {
		t.Fatal(err)
	}
	expectedIDs := map[string]bool{"N1": true, "N2": true, "C1": true, "C2": true, "A1": true, "A2": true, "A3": true, "A4": true}
	if len(cases) != len(expectedIDs) {
		t.Fatalf("frozen pilot requires eight cases, got %d", len(cases))
	}
	seen := map[string]bool{}
	verified := map[string]map[string][]byte{}
	copiedHashes := map[string]string{}
	for _, testCase := range cases {
		if !expectedIDs[testCase.ID] || seen[testCase.ID] {
			t.Fatalf("unexpected or duplicate pilot case %q", testCase.ID)
		}
		seen[testCase.ID] = true
		t.Run(testCase.ID, func(t *testing.T) {
			directory := filepath.Join(root, testCase.ID)
			pilotRealPath(t, directory)
			files := pilotCaseFiles(t, directory)
			if !bytes.Equal(files["source.pipe"], []byte(testCase.Source)) {
				t.Fatal("source.pipe differs from the frozen corpus source")
			}
			var expected coreir.Program
			if err := json.Unmarshal(files["core.json"], &expected); err != nil {
				t.Fatal(err)
			}
			if len(expected.Functions) == 0 {
				t.Fatal("frozen Core has no functions")
			}
			input := pilotModuleSet(testCase.ID, files["source.pipe"], expected)
			referenceAnalysis := pipelang.AnalyzeSemanticModuleSet(input)
			nativeAnalysis := pipelang.AnalyzeSemanticModuleSetWithLexer(input, lexer.Tokenize)
			if err := referenceAnalysis.Error(); err != nil {
				t.Fatalf("Go lexer analysis: %v", err)
			}
			if err := nativeAnalysis.Error(); err != nil {
				t.Fatalf("native lexer analysis: %v", err)
			}
			if !reflect.DeepEqual(referenceAnalysis.Diagnostics, nativeAnalysis.Diagnostics) {
				t.Fatal("analysis diagnostics differ")
			}
			if !reflect.DeepEqual(referenceAnalysis.SemanticIDs.Declarations(), nativeAnalysis.SemanticIDs.Declarations()) {
				t.Fatal("semantic declarations differ")
			}
			if !bytes.Equal(pilotJSON(t, referenceAnalysis.Program), pilotJSON(t, nativeAnalysis.Program)) {
				t.Fatal("parsed public AST JSON differs")
			}
			referenceCore := pilotLower(t, referenceAnalysis, expected)
			nativeCore := pilotLower(t, nativeAnalysis, expected)
			if !reflect.DeepEqual(referenceCore, nativeCore) || !reflect.DeepEqual(nativeCore, expected) {
				t.Fatal("Go/native lexer Core differs from each other or frozen Core")
			}
			referenceCPP, err := cppbackend.GenerateWithNames(referenceCore)
			if err != nil {
				t.Fatal(err)
			}
			nativeCPP, err := emitter.Generate(context.Background(), nativeCore)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(referenceCPP.Source, nativeCPP.Source) || !reflect.DeepEqual(referenceCPP.Functions, nativeCPP.Functions) {
				t.Fatal("native lexer/emitter source or bindings differ from Go reference")
			}
			if !bytes.Equal(nativeCPP.Source, files["generated.hpp"]) || !bytes.Equal(referenceCPP.Source, files["reference.hpp"]) {
				t.Fatal("fresh C++ differs from frozen generated.hpp or reference.hpp")
			}
			generatedGo, err := gobackend.GenerateWithNames(nativeCore)
			if err != nil {
				t.Fatal(err)
			}
			goSource := bytes.Replace(generatedGo.Source, []byte("package pipelanggenerated"), []byte("package main"), 1)
			if !bytes.Equal(goSource, files["generated.go"]) {
				t.Fatal("fresh ordinary Go backend output differs from frozen generated.go")
			}
			freshCore := pilotJSON(t, nativeCore)
			if !bytes.Equal(freshCore, files["core.json"]) {
				t.Fatal("fresh Core JSON encoding differs from frozen core.json")
			}
			for name, data := range files {
				if name == "core.json" || name == "generated.hpp" || name == "generated.go" {
					continue
				}
				digest := sha256.Sum256(data)
				copiedHashes[testCase.ID+"/"+name] = hex.EncodeToString(digest[:])
			}
			files["core.json"] = freshCore
			files["generated.hpp"] = nativeCPP.Source
			files["generated.go"] = goSource
			verified[testCase.ID] = files
		})
	}
	if t.Failed() {
		return
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 9 {
		t.Fatal("frozen pilot root must contain only corpus.json and the eight case directories")
	}
	for _, entry := range entries {
		if entry.Name() != "corpus.json" && !expectedIDs[entry.Name()] {
			t.Fatalf("unexpected pilot root entry %q", entry.Name())
		}
	}
	output := os.Getenv("PIPELANG_LEXER_PILOT_OUTPUT")
	if output == "" {
		return
	}
	if !filepath.IsAbs(output) {
		t.Fatal("PIPELANG_LEXER_PILOT_OUTPUT must be an absolute fresh directory")
	}
	pilotRealPath(t, filepath.Dir(output))
	if err := os.Mkdir(output, 0700); err != nil {
		t.Fatalf("create fresh proof output: %v", err)
	}
	for _, testCase := range cases {
		directory := filepath.Join(output, testCase.ID)
		if err := os.Mkdir(directory, 0700); err != nil {
			t.Fatal(err)
		}
		for name, data := range verified[testCase.ID] {
			pilotWriteFresh(t, filepath.Join(directory, name), data)
		}
	}
	pilotWriteFresh(t, filepath.Join(output, "corpus.json"), corpusBytes)
	provenance := struct {
		Contract           string            `json:"contract"`
		FrozenCorpus       string            `json:"frozen_corpus"`
		LexerSHA256        string            `json:"lexer_sha256"`
		EmitterSHA256      string            `json:"emitter_sha256"`
		FreshArtifacts     []string          `json:"fresh_artifacts_per_case"`
		CopiedArtifacts    map[string]string `json:"copied_frozen_artifact_sha256"`
		TraceProvenance    string            `json:"trace_provenance"`
		ExecutionPerformed bool              `json:"generated_execution_performed"`
	}{
		Contract: "pipelang.native-lexer-pilot-proof.v1", FrozenCorpus: root,
		LexerSHA256: lexer.SHA256, EmitterSHA256: emitter.SHA256,
		FreshArtifacts: []string{"core.json", "generated.hpp", "generated.go"}, CopiedArtifacts: copiedHashes,
		TraceProvenance: "traced.go and traced.hpp are copied frozen test instrumentation; traces are not freshly generated by this test",
	}
	pilotWriteFresh(t, filepath.Join(output, "lexer-provenance.json"), pilotJSON(t, provenance))
	t.Logf("fresh native lexer/emitter proof artifacts: %s", output)
}

func pilotModuleSet(caseID string, source []byte, expected coreir.Program) pipelang.ModuleSetInput {
	packageID := pipelang.PackageID(expected.Functions[0].Identity.PackageID)
	module := pipelang.ModuleInput{
		ID: "app.root", Namespace: "app.root",
		DeclarationSpan: pipelang.Span{File: pipelang.FileID(caseID + ".pipe"), Start: 0, End: 0},
		Sources:         []pipelang.SourceInput{{Path: caseID + ".pipe", Data: source}},
	}
	return pipelang.ModuleSetInput{
		LanguageContract: pipelang.LanguageContract(expected.LanguageContract), PackageID: packageID, Root: module.ID,
		Modules: []pipelang.ModuleInput{module},
		Lock: pipelang.DependencyLock{Modules: []pipelang.LockedModule{{
			ID: module.ID, SourceSHA256: pipelang.ModuleSourceSHA256(module.Sources),
			SemanticSHA256: pipelang.ModuleSemanticSHA256(packageID, module.Namespace, module.SemanticMigrations),
		}}},
	}
}

func pilotLower(t *testing.T, analysis *pipelang.Analysis, expected coreir.Program) coreir.Program {
	t.Helper()
	program := coreir.Program{LanguageContract: expected.LanguageContract, CompilerContract: expected.CompilerContract}
	wanted := map[string]bool{}
	for _, function := range expected.Functions {
		wanted[function.Identity.PackageID+":"+function.Identity.Path] = true
	}
	seen := map[string]bool{}
	for _, expectedFunction := range expected.Functions {
		var method *pipelang.SemanticDeclaration
		for _, declaration := range analysis.SemanticIDs.Declarations() {
			if string(declaration.Identity.PackageID) == expectedFunction.Identity.PackageID && string(declaration.Identity.Path) == expectedFunction.Identity.Path {
				copyOfDeclaration := declaration
				method = &copyOfDeclaration
				break
			}
		}
		if method == nil || method.Kind != pipelang.SemanticMethod {
			t.Fatalf("missing semantic method %s", expectedFunction.Identity.Path)
		}
		hir, err := pipelang.LowerSemanticMethodToHIR(analysis, method.Identity)
		if err != nil {
			t.Fatal(err)
		}
		lowered, err := pipelang.LowerHIRToCore(hir)
		if err != nil {
			t.Fatal(err)
		}
		for _, function := range lowered.Functions {
			key := function.Identity.PackageID + ":" + function.Identity.Path
			if wanted[key] && !seen[key] {
				program.Functions = append(program.Functions, function)
				seen[key] = true
			}
		}
	}
	sort.Slice(program.Functions, func(i, j int) bool { return program.Functions[i].Identity.Path < program.Functions[j].Identity.Path })
	return program
}

func pilotCaseFiles(t *testing.T, directory string) map[string][]byte {
	t.Helper()
	names := []string{"source.pipe", "core.json", "generated.hpp", "reference.hpp", "generated.go", "traced.hpp", "traced.go", "main.cpp", "main.go", "inputs.bin", "expected.json"}
	entries, err := os.ReadDir(directory)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != len(names) {
		t.Fatalf("case directory has unexpected artifact count: %s", directory)
	}
	files := map[string][]byte{}
	for _, name := range names {
		files[name] = pilotReadRegular(t, filepath.Join(directory, name))
	}
	return files
}

func pilotExecutableHash(t *testing.T, path string) string {
	t.Helper()
	data := pilotReadRegular(t, path)
	digest := sha256.Sum256(data)
	return hex.EncodeToString(digest[:])
}

func pilotReadRegular(t *testing.T, path string) []byte {
	t.Helper()
	pilotRealPath(t, path)
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() {
		t.Fatalf("pilot artifact must be a regular file: %s (%v)", path, err)
	}
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	const limit = 64 * 1024 * 1024
	data, readError := io.ReadAll(io.LimitReader(file, limit+1))
	closeError := file.Close()
	if readError != nil || closeError != nil || len(data) > limit {
		t.Fatalf("pilot artifact read failed or exceeded 64 MiB: %s read=%v close=%v", path, readError, closeError)
	}
	return data
}

func pilotRealPath(t *testing.T, path string) {
	t.Helper()
	absolute, err := filepath.Abs(path)
	if err != nil {
		t.Fatal(err)
	}
	for current := absolute; ; current = filepath.Dir(current) {
		info, err := os.Lstat(current)
		if err != nil {
			t.Fatal(err)
		}
		if info.Mode()&os.ModeSymlink != 0 {
			t.Fatalf("pilot paths must not traverse symlinks: %s", current)
		}
		if filepath.Dir(current) == current {
			break
		}
	}
}

func pilotJSON(t *testing.T, value any) []byte {
	t.Helper()
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func pilotWriteFresh(t *testing.T, path string, data []byte) {
	t.Helper()
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		t.Fatal(err)
	}
	_, writeError := file.Write(data)
	closeError := file.Close()
	if writeError != nil || closeError != nil {
		t.Fatalf("write fresh proof artifact: write=%v close=%v", writeError, closeError)
	}
}
