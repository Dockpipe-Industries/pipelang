// pipelang-viewc is an opt-in compiler entrypoint for the bounded PipeView
// source profile. It emits an inert document artifact, never a UI process.
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
	"strings"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/markup"
	viewsource "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativelexer"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativeviewparser"
)

const exportContract = "pipelang.pipeview.export.v1"
const conversionContract = "pipelang.pipeview.convert.v1"
const diagnosticsContract = "pipelang.pipeview.diagnostics.v1"

type diagnosticReport struct {
	Contract    string                        `json:"contract"`
	FailureKind string                        `json:"failure_kind"`
	Diagnostics []pipelang.ResolvedDiagnostic `json:"diagnostics"`
	Message     *string                       `json:"message,omitempty"`
}

func writeJSONFailure(output io.Writer, err error) {
	report := diagnosticReport{
		Contract: diagnosticsContract, FailureKind: "tool",
		Diagnostics: make([]pipelang.ResolvedDiagnostic, 0),
	}
	var sourceFailure *pipelang.DiagnosticError
	if errors.As(err, &sourceFailure) && sourceFailure != nil {
		report.FailureKind = "source"
		report.Diagnostics = pipelang.ResolveDiagnostics(sourceFailure.Sources, sourceFailure.Diagnostics)
	} else {
		message := err.Error()
		report.Message = &message
	}
	// Encoding this closed diagnostic projection cannot require host effects.
	// A failed stderr write still leaves the original command unsuccessful.
	json.NewEncoder(output).Encode(report)
}

// The same FlagSet owns diagnostics selection and ordinary command options.
// Buffer its messages until the selected mode is known, including parse errors
// after a successfully parsed --diagnostics flag.
func runCLI(arguments []string, stderr io.Writer) int {
	mode := "text"
	var flagOutput strings.Builder
	err := runWithDiagnostics(arguments, &mode, &flagOutput)
	if err == nil {
		return 0
	}
	if mode == "json" {
		writeJSONFailure(stderr, err)
	} else {
		fmt.Fprint(stderr, flagOutput.String())
		fmt.Fprintln(stderr, "PipeLang view export:", err)
	}
	return 2
}

type metadata struct {
	SourceSyntax               string `json:"source_syntax,omitempty"`
	OutputFormat               string `json:"output_format,omitempty"`
	ActionContract             string `json:"action_contract,omitempty"`
	StateRuntimeSHA256         string `json:"state_runtime_sha256,omitempty"`
	Contract                   string `json:"contract"`
	SourceContract             string `json:"source_contract"`
	Adapter                    string `json:"adapter"`
	SourceSHA256               string `json:"source_sha256"`
	GeneratedSHA256            string `json:"generated_sha256"`
	CompilerExecutableSHA256   string `json:"compiler_executable_sha256"`
	LexerExecutableSHA256      string `json:"lexer_executable_sha256,omitempty"`
	ViewParserExecutableSHA256 string `json:"view_parser_executable_sha256,omitempty"`
}

// Conversion receipts describe source artifacts. They deliberately carry no
// native adapter or runtime fields, which apply only to generated headers.
type conversionMetadata struct {
	Contract                   string `json:"contract"`
	SourceContract             string `json:"source_contract"`
	SourceSyntax               string `json:"source_syntax"`
	OutputFormat               string `json:"output_format"`
	SourceSHA256               string `json:"source_sha256"`
	GeneratedSHA256            string `json:"generated_sha256"`
	CompilerExecutableSHA256   string `json:"compiler_executable_sha256"`
	LexerExecutableSHA256      string `json:"lexer_executable_sha256,omitempty"`
	ViewParserExecutableSHA256 string `json:"view_parser_executable_sha256,omitempty"`
}

func compileArtifact(file *pipelang.SourceFile, syntax, format string, lexer pipelang.SourceLexer) ([]byte, viewsource.Program, error) {
	return compileArtifactWithParser(file, syntax, format, lexer, nil)
}

func compileArtifactWithParser(file *pipelang.SourceFile, syntax, format string, lexer pipelang.SourceLexer, parser pipelang.ViewSourceParser) ([]byte, viewsource.Program, error) {
	if parser != nil && (syntax == "xml" || format == "xml") {
		return nil, viewsource.Program{}, errors.New("selected native view parser does not support XML input or output")
	}
	var program viewsource.Program
	var err error
	if syntax == "xml" {
		program, err = markup.CompileProgram(file, lexer)
	} else {
		program, err = viewsource.CompileProgramWithParser(file, lexer, parser)
	}
	if err != nil {
		return nil, viewsource.Program{}, err
	}
	if format == "header" {
		transport, err := forgepipe.Encode(program.Document, forgepipe.Options{Scale: 1.125, Focus: "#FFAD45FF"})
		if err != nil {
			return nil, viewsource.Program{}, err
		}
		generated := header(transport)
		model, err := modelHeader(program)
		if err != nil {
			return nil, viewsource.Program{}, err
		}
		return append(generated, model...), program, nil
	}

	// All conversions first pass the same portable profile admission as header
	// generation. XML remains an explicit frontend to that source contract.
	canonicalInput := file
	if syntax == "xml" {
		text, err := markup.ToSource(file)
		if err != nil {
			return nil, viewsource.Program{}, err
		}
		converted, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: file.Path + ".generated.pipe", Data: text}})
		if diagnostics.HasErrors() {
			return nil, viewsource.Program{}, &pipelang.DiagnosticError{Sources: converted, Diagnostics: diagnostics}
		}
		canonicalInput = converted.Files()[0]
	}
	var generated []byte
	if format == "xml" {
		generated, err = markup.FromSource(canonicalInput, lexer)
	} else {
		var declaration *pipelang.ViewDeclaration
		declaration, err = pipelang.ParseViewSourceWithParser(canonicalInput, lexer, parser)
		if err == nil {
			generated, err = pipelang.FormatViewSource(declaration)
		}
	}
	if err != nil {
		return nil, viewsource.Program{}, err
	}
	return generated, program, nil
}

func digest(data []byte) string {
	hash := sha256.Sum256(data)
	return hex.EncodeToString(hash[:])
}

// Every transport byte is escaped independently, so UTF-8/control bytes cannot
// acquire C++ escape semantics or depend on a compiler's source encoding.
func header(transport []byte) []byte {
	var result strings.Builder
	result.WriteString("#pragma once\n#include <string>\nnamespace compiled_ui {\ninline std::string view()\n{\n    return std::string(\n")
	for start := 0; start < len(transport); start += 16 {
		end := min(start+16, len(transport))
		result.WriteString("        \"")
		for _, value := range transport[start:end] {
			fmt.Fprintf(&result, "\\x%02x", value)
		}
		result.WriteString("\"\n")
	}
	fmt.Fprintf(&result, "        , %d);\n}\n}\n", len(transport))
	return []byte(result.String())
}

func atomicWrite(path string, data []byte) error {
	file, err := os.CreateTemp(filepath.Dir(path), ".pipeview-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := file.Write(data); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}

func regularOutput(path string) error {
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("output is not a regular file: %s", path)
	}
	return nil
}

func executableDigest() (string, error) {
	executable, err := os.Executable()
	if err != nil {
		return "", err
	}
	file, err := os.Open(executable)
	if err != nil {
		return "", err
	}
	defer file.Close()
	hash := sha256.New()
	count, err := io.Copy(hash, io.LimitReader(file, (256<<20)+1))
	if err != nil {
		return "", err
	}
	if count > 256<<20 {
		return "", errors.New("compiler exceeds executable hashing capacity")
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func run(arguments []string) error {
	mode := "text"
	return runWithDiagnostics(arguments, &mode, io.Discard)
}

func runWithDiagnostics(arguments []string, diagnosticMode *string, flagOutput io.Writer) error {
	flags := flag.NewFlagSet("pipelang-viewc", flag.ContinueOnError)
	flags.SetOutput(flagOutput)
	flags.StringVar(diagnosticMode, "diagnostics", "text", "Failure diagnostics: text or json")
	sourcePath := flags.String("source", "", "Absolute view source (at most 64 KiB)")
	output := flags.String("output", "", "Absolute generated artifact path")
	syntax := flags.String("syntax", "pipelang", "Explicit input syntax: pipelang or xml")
	format := flags.String("emit", "header", "Output format: header, pipelang or xml")
	lexerPath := flags.String("cpp-lexer", "", "Explicit absolute experimental native lexer")
	lexerHash := flags.String("cpp-lexer-sha256", "", "Exact selected lexer SHA-256")
	parserPath := flags.String("cpp-view-parser", "", "Explicit absolute experimental native view syntax parser")
	parserHash := flags.String("cpp-view-parser-sha256", "", "Exact selected view parser SHA-256")
	if err := flags.Parse(arguments); err != nil {
		return err
	}
	if *diagnosticMode != "text" && *diagnosticMode != "json" {
		return errors.New("--diagnostics must be text or json")
	}
	if *syntax != "pipelang" && *syntax != "xml" {
		return errors.New("--syntax must be pipelang or xml")
	}
	if *format != "header" && *format != "pipelang" && *format != "xml" {
		return errors.New("--emit must be header, pipelang or xml")
	}
	if flags.NArg() != 0 || !filepath.IsAbs(*sourcePath) || !filepath.IsAbs(*output) {
		return errors.New("absolute --source and --output are required; no positional arguments")
	}
	if (*lexerPath == "") != (*lexerHash == "") {
		return errors.New("--cpp-lexer and --cpp-lexer-sha256 must be supplied together")
	}
	if (*parserPath == "") != (*parserHash == "") {
		return errors.New("--cpp-view-parser and --cpp-view-parser-sha256 must be supplied together")
	}
	if *parserPath != "" && (*syntax == "xml" || *format == "xml") {
		return errors.New("selected native view parser does not support XML input or output")
	}
	if filepath.Clean(*sourcePath) == filepath.Clean(*output) || filepath.Clean(*sourcePath) == filepath.Clean(*output+".json") {
		return errors.New("source and generated outputs must be distinct")
	}
	file, err := os.Open(*sourcePath)
	if err != nil {
		return err
	}
	data, err := io.ReadAll(io.LimitReader(file, 65537))
	file.Close()
	if err != nil {
		return err
	}
	if len(data) == 0 || len(data) > 65536 {
		return errors.New("view source must contain 1..65536 bytes")
	}
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: filepath.Base(*sourcePath), Data: data}})
	if diagnostics.HasErrors() {
		return &pipelang.DiagnosticError{Sources: sources, Diagnostics: diagnostics}
	}
	var lexer pipelang.SourceLexer
	if *lexerPath != "" {
		selected := nativelexer.Lexer{Path: *lexerPath, SHA256: *lexerHash}
		lexer = selected.Tokenize
	}
	var parser pipelang.ViewSourceParser
	if *parserPath != "" {
		selected := nativeviewparser.Parser{Path: *parserPath, SHA256: *parserHash}
		parser = selected.Parse
	}
	generated, program, err := compileArtifactWithParser(sources.Files()[0], *syntax, *format, lexer, parser)
	if err != nil {
		return err
	}
	compilerHash, err := executableDigest()
	if err != nil {
		return err
	}
	sourceContract := viewsource.Contract
	if program.Definition != nil {
		sourceContract = viewsource.ActionContract
	}
	var receipt any
	if *format == "header" {
		adapterSchema, err := forgepipe.SchemaFor(program.Document)
		if err != nil {
			return err
		}
		headerReceipt := metadata{Contract: exportContract, SourceContract: sourceContract, Adapter: adapterSchema,
			SourceSHA256: digest(data), GeneratedSHA256: digest(generated), CompilerExecutableSHA256: compilerHash,
			LexerExecutableSHA256: *lexerHash, ViewParserExecutableSHA256: *parserHash}
		if *syntax == "xml" {
			headerReceipt.SourceSyntax = *syntax
			headerReceipt.OutputFormat = *format
		}
		if program.Definition != nil {
			headerReceipt.ActionContract = state.Contract
			headerReceipt.StateRuntimeSHA256 = digest([]byte(state.NativeRuntime()))
		}
		receipt = headerReceipt
	} else {
		receipt = conversionMetadata{Contract: conversionContract, SourceContract: sourceContract,
			SourceSyntax: *syntax, OutputFormat: *format, SourceSHA256: digest(data), GeneratedSHA256: digest(generated),
			CompilerExecutableSHA256: compilerHash, LexerExecutableSHA256: *lexerHash, ViewParserExecutableSHA256: *parserHash}
	}
	encoded, err := json.MarshalIndent(receipt, "", "  ")
	if err != nil {
		return err
	}
	// Validate both destinations before publishing. Metadata is the completion
	// marker; consumers must compare its digest with the generated artifact.
	if err := regularOutput(*output); err != nil {
		return err
	}
	if err := regularOutput(*output + ".json"); err != nil {
		return err
	}
	if err := atomicWrite(*output, generated); err != nil {
		return err
	}
	return atomicWrite(*output+".json", append(encoded, '\n'))
}

func main() {
	os.Exit(runCLI(os.Args[1:], os.Stderr))
}
