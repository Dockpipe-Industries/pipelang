// Package markup provides optional, bounded XML conversion for PipeView views.
// XML lowers to compiler-owned view syntax and the shared source checker; it
// introduces no runtime, expression evaluator or external-resource loader.
package markup

import (
	"fmt"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

const MaximumBytes = 65536

// CodeInvalidXML identifies PipeView XML syntax and profile admission failures.
// Semantic view diagnostics retain their shared checker codes and categories.
const CodeInvalidXML pipelang.DiagnosticCode = "PV1001"

type loweredView struct {
	generated []byte
	authored  *pipelang.ViewDeclaration
}

// ToSource converts strict XML to canonical, checked PipeLang view source.
// Comments and formatting are not preserved. XML declarations and all other
// processing instructions are outside this strict frontend profile.
// XML admission failures use CodeInvalidXML source diagnostics; malformed UTF-8
// retains the compiler's source-admission diagnostic instead.
func ToSource(file *pipelang.SourceFile) ([]byte, error) {
	lowered, err := lower(file)
	if err != nil {
		return nil, err
	}
	if _, err := checkGenerated(file, lowered, nil); err != nil {
		return nil, err
	}
	return lowered.generated, nil
}

// CompileProgram checks XML through the same typed component/action checker as
// ordinary view source. A selected lexer receives generated PipeLang, not XML.
func CompileProgram(file *pipelang.SourceFile, lexer pipelang.SourceLexer) (source.Program, error) {
	lowered, err := lower(file)
	if err != nil {
		return source.Program{}, err
	}
	return checkGenerated(file, lowered, lexer)
}

// FromSource validates source before emitting semantic XML. XML 1.0 cannot
// represent every PipeLang string; unrepresentable characters are refused.
func FromSource(file *pipelang.SourceFile, lexer pipelang.SourceLexer) ([]byte, error) {
	if _, err := source.CompileProgram(file, lexer); err != nil {
		return nil, err
	}
	declaration, err := pipelang.ParseViewSource(file, lexer)
	if err != nil {
		return nil, err
	}
	return encode(file, declaration)
}

func lower(file *pipelang.SourceFile) (loweredView, error) {
	if file == nil || file.ID == "" {
		return loweredView{}, fmt.Errorf("markup conversion requires an admitted XML source file")
	}
	if len(file.Text) == 0 || len(file.Text) > MaximumBytes {
		return loweredView{}, xmlFailure(file, 0, "XML must contain between 1 and 65536 bytes")
	}
	if !utf8.ValidString(file.Text) {
		return loweredView{}, xmlFailure(file, 0, "XML is not valid UTF-8")
	}
	declaration, err := parse(file)
	if err != nil {
		return loweredView{}, err
	}
	generated, err := pipelang.FormatViewSource(declaration)
	if err != nil {
		return loweredView{}, xmlFailure(file, 0, "cannot represent XML as PipeLang: "+err.Error())
	}
	return loweredView{generated: generated, authored: declaration}, nil
}

func checkGenerated(original *pipelang.SourceFile, lowered loweredView, lexer pipelang.SourceLexer) (source.Program, error) {
	path := sourcePath(original) + ".generated.pipe"
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: path, Data: lowered.generated}})
	if diagnostics.HasErrors() {
		return source.Program{}, fmt.Errorf("generated PipeLang from XML %q: %v", sourcePath(original), diagnostics)
	}
	generated := sources.Files()[0]
	canonical, canonicalError := pipelang.ParseViewSource(generated, nil)
	var origins originMap
	if canonicalError == nil {
		origins, _ = buildOrigins(original, lowered.authored, generated, canonical)
	}
	checkedLexer := lexer
	lexerFailed := false
	if lexer != nil {
		checkedLexer = func(file *pipelang.SourceFile) (pipelang.LexicalResult, error) {
			result, err := lexer(file)
			lexerFailed = err != nil
			return result, err
		}
	}
	program, err := source.CompileProgram(generated, checkedLexer)
	if err != nil {
		if !lexerFailed {
			if mapped, ok := origins.remapError(original, generated, err); ok {
				return source.Program{}, mapped
			}
		}
		return source.Program{}, fmt.Errorf("generated PipeLang diagnostic from XML %q (generated source %q): %w",
			sourcePath(original), path, err)
	}
	return program, nil
}

func sourcePath(file *pipelang.SourceFile) string {
	if file.Path != "" {
		return file.Path
	}
	return string(file.ID)
}
