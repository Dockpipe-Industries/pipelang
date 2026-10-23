// pipelang-native-cpp emits the existing bounded native C++ pilot from the real
// PipeLang semantic/HIR/Core pipeline. It neither parses a second language nor
// loads host libraries, discovers packages, or grants runtime capabilities.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/cppbackend"
)

const toolContract = "pipelang.native-cpp-export.v1"
const maximumSourceBytes = 65536
const maximumOutputBytes = 16 << 20

type bindingMetadata struct {
	Identity   coreir.SemanticIdentity `json:"identity"`
	Name       string                  `json:"name"`
	CppSymbol  string                  `json:"cpp_symbol"`
	ReturnType coreir.Type             `json:"return_type"`
	Parameters []coreir.Parameter      `json:"parameters"`
}

type metadata struct {
	Contract                 string            `json:"contract"`
	Profile                  string            `json:"profile"`
	LanguageContract         string            `json:"language_contract"`
	CompilerContract         string            `json:"compiler_contract"`
	SourceSHA256             string            `json:"source_sha256"`
	GeneratedSHA256          string            `json:"generated_sha256"`
	CompilerExecutableSHA256 string            `json:"compiler_executable_sha256"`
	Functions                []bindingMetadata `json:"functions"`
}

func digest(data []byte) string {
	hash := sha256.Sum256(data)
	return hex.EncodeToString(hash[:])
}

func compile(source []byte, sourceName, packageName, moduleName string) ([]byte, metadata, error) {
	if len(source) == 0 || len(source) > maximumSourceBytes {
		return nil, metadata{}, errors.New("source must contain 1..65536 bytes")
	}
	moduleID := pipelang.ModuleID(moduleName)
	module := pipelang.ModuleInput{
		ID:              moduleID,
		Namespace:       pipelang.SemanticID(moduleName),
		DeclarationSpan: pipelang.Span{File: pipelang.FileID(sourceName)},
		Sources:         []pipelang.SourceInput{{Path: sourceName, Data: source}},
	}
	packageID := pipelang.PackageID(packageName)
	input := pipelang.ModuleSetInput{
		LanguageContract: pipelang.PipeLangLanguageContractV1130,
		PackageID:        packageID,
		Root:             moduleID,
		Modules:          []pipelang.ModuleInput{module},
		Lock: pipelang.DependencyLock{Modules: []pipelang.LockedModule{{
			ID:             moduleID,
			SourceSHA256:   pipelang.ModuleSourceSHA256(module.Sources),
			SemanticSHA256: pipelang.ModuleSemanticSHA256(packageID, module.Namespace, nil),
		}}},
	}
	analysis := pipelang.AnalyzeSemanticModuleSet(input)
	if err := analysis.Error(); err != nil {
		return nil, metadata{}, err
	}
	program := coreir.Program{LanguageContract: string(input.LanguageContract), CompilerContract: coreir.CompilerContractV1}
	seen := make(map[string]bool)
	public := make(map[string]bool)
	for _, declaration := range analysis.SemanticIDs.Declarations() {
		if declaration.Kind != pipelang.SemanticMethod || declaration.Visibility != pipelang.VisibilityPublic {
			continue
		}
		hir, err := pipelang.LowerSemanticMethodToHIR(analysis, declaration.Identity)
		if err != nil {
			return nil, metadata{}, err
		}
		lowered, err := pipelang.LowerHIRToCore(hir)
		if err != nil {
			return nil, metadata{}, err
		}
		public[string(declaration.Identity.Path)] = true
		for _, function := range lowered.Functions {
			key := function.Identity.PackageID + "\x00" + function.Identity.Path
			if !seen[key] {
				seen[key] = true
				program.Functions = append(program.Functions, function)
			}
		}
	}
	if len(program.Functions) == 0 || len(program.Functions) > 128 {
		return nil, metadata{}, errors.New("native C++ profile requires 1..128 admitted functions")
	}
	sort.Slice(program.Functions, func(first, second int) bool {
		return program.Functions[first].Identity.Path < program.Functions[second].Identity.Path
	})
	generated, err := cppbackend.GenerateWithNames(program)
	if err != nil {
		return nil, metadata{}, err
	}
	if len(generated.Source) > maximumOutputBytes {
		return nil, metadata{}, errors.New("generated C++ exceeds the output capacity")
	}
	result := metadata{
		Contract:         toolContract,
		Profile:          cppbackend.Capability,
		LanguageContract: program.LanguageContract,
		CompilerContract: program.CompilerContract,
		SourceSHA256:     digest(source),
		GeneratedSHA256:  digest(generated.Source),
	}
	for _, binding := range generated.Functions {
		if !public[binding.Identity.Path] {
			continue
		}
		for _, function := range program.Functions {
			if function.Identity.Path == binding.Identity.Path && function.Identity.PackageID == binding.Identity.PackageID {
				result.Functions = append(result.Functions, bindingMetadata{
					Identity:   function.Identity,
					Name:       function.Name,
					CppSymbol:  "pipelang::" + binding.Name,
					ReturnType: function.ReturnType,
					Parameters: function.Parameters,
				})
			}
		}
	}
	return generated.Source, result, nil
}

func writeAtomic(directory, name string, bytes []byte) error {
	destination := filepath.Join(directory, name)
	if info, err := os.Lstat(destination); err == nil && !info.Mode().IsRegular() {
		return fmt.Errorf("output is not a regular file: %s", name)
	} else if err != nil && !os.IsNotExist(err) {
		return err
	}
	file, err := os.CreateTemp(directory, ".native-cpp-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := file.Write(bytes); err != nil {
		file.Close()
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), destination)
}

func run() error {
	sourcePath := flag.String("source", "", "PipeLang source file, at most 64 KiB")
	output := flag.String("output-dir", "", "Explicit absolute generated output directory")
	packageName := flag.String("package", "example.native", "Stable semantic package identity")
	moduleName := flag.String("module", "app.root", "Stable semantic module identity")
	expected := flag.String("source-sha256", "", "Optional exact source byte digest")
	flag.Parse()
	if flag.NArg() != 0 || *sourcePath == "" || !filepath.IsAbs(*output) {
		return errors.New("--source and absolute --output-dir are required; positional arguments are unsupported")
	}
	file, err := os.Open(*sourcePath)
	if err != nil {
		return err
	}
	bytes, err := io.ReadAll(io.LimitReader(file, maximumSourceBytes+1))
	file.Close()
	if err != nil {
		return err
	}
	if *expected != "" && *expected != digest(bytes) {
		return errors.New("source SHA-256 differs from the explicitly selected identity")
	}
	generated, receipt, err := compile(bytes, filepath.Base(*sourcePath), *packageName, *moduleName)
	if err != nil {
		return err
	}
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	compiler, err := os.Open(executable)
	if err != nil {
		return err
	}
	hash := sha256.New()
	compilerBytes, err := io.Copy(hash, io.LimitReader(compiler, (256<<20)+1))
	compiler.Close()
	if err != nil {
		return err
	}
	if compilerBytes > 256<<20 {
		return errors.New("compiler executable exceeds its identity hashing capacity")
	}
	receipt.CompilerExecutableSHA256 = hex.EncodeToString(hash.Sum(nil))
	metadata, err := json.MarshalIndent(receipt, "", "  ")
	if err != nil || len(metadata) > maximumOutputBytes {
		return errors.New("native C++ identity metadata exceeded its output contract")
	}
	if err := os.MkdirAll(*output, 0700); err != nil {
		return err
	}
	if err := writeAtomic(*output, "generated.hpp", generated); err != nil {
		return err
	}
	// Metadata is the completion marker and contains the exact generated digest.
	return writeAtomic(*output, "bindings.json", append(metadata, '\n'))
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "PipeLang native C++ export:", err)
		os.Exit(2)
	}
}
