package pipelang

import (
	"encoding/json"
	"errors"
	"reflect"
	"testing"
)

func lexicalModuleInput(text string) ModuleSetInput {
	input := semanticTestModuleSet("app.root", []ModuleInput{testModule("app.root", "Root.pipe", text)}, nil)
	input.LanguageContract = PipeLangLanguageContractV1130
	return input
}

func TestSemanticExplicitLexerPreservesReferenceAnalysis(t *testing.T) {
	for _, source := range []string{
		`public Class Root { public int Value() => 42; }`,
		`public Class Root { public string Value() => "rider\nname"; }`,
		`public Class Root { public int Value() => 1.; }`,
		`public Class Root { public string Value() => "unterminated; }`,
		`public Class Root { public int Value() => @; }`,
	} {
		input := lexicalModuleInput(source)
		reference := AnalyzeSemanticModuleSet(input)
		calls := 0
		selected := AnalyzeSemanticModuleSetWithLexer(input, func(file *SourceFile) (LexicalResult, error) {
			calls++
			return TokenizeSource(file)
		})
		if calls != 1 {
			t.Fatal("selected lexer was not used once for the source")
		}
		if !reflect.DeepEqual(reference.Diagnostics, selected.Diagnostics) {
			t.Fatalf("selected lexer changed diagnostics: %v / %v", reference.Diagnostics, selected.Diagnostics)
		}
		referenceAST, err := json.Marshal(reference.Program)
		if err != nil {
			t.Fatal(err)
		}
		selectedAST, err := json.Marshal(selected.Program)
		if err != nil || string(referenceAST) != string(selectedAST) {
			t.Fatal("selected lexer changed the parsed program")
		}
		if reference.SemanticIDs != nil && !reflect.DeepEqual(reference.SemanticIDs.Declarations(), selected.SemanticIDs.Declarations()) {
			t.Fatal("selected lexer changed semantic identities")
		}
	}
}

func TestSemanticExplicitLexerRejectsMalformedTokensWithoutFallback(t *testing.T) {
	input := lexicalModuleInput(`public Class Root { public int Value() => 42; }`)
	if err := AnalyzeSemanticModuleSet(input).Error(); err != nil {
		t.Fatal(err)
	}
	cases := map[string]func(*LexicalResult){
		"unknown kind":         func(result *LexicalResult) { result.Tokens[0].Kind = "future" },
		"out of range":         func(result *LexicalResult) { result.Tokens[0].End = 1000000 },
		"overlapping":          func(result *LexicalResult) { result.Tokens[1].Start = 0 },
		"early EOF":            func(result *LexicalResult) { result.Tokens[0].Kind = "eof" },
		"wrong EOF":            func(result *LexicalResult) { result.Tokens[len(result.Tokens)-1].Start = 0 },
		"missing EOF":          func(result *LexicalResult) { result.Tokens = result.Tokens[:len(result.Tokens)-1] },
		"no tokens":            func(result *LexicalResult) { result.Tokens = nil },
		"changed source bytes": func(result *LexicalResult) { result.Tokens[0].Literal = "private" },
		"tokens and failure": func(result *LexicalResult) {
			result.Failure = &LexicalFailure{Code: CodeUnexpectedChar, Message: "unexpected character", Start: 0, End: 1}
		},
	}
	for name, change := range cases {
		t.Run(name, func(t *testing.T) {
			calls := 0
			analysis := AnalyzeSemanticModuleSetWithLexer(input, func(file *SourceFile) (LexicalResult, error) {
				calls++
				result, err := TokenizeSource(file)
				if err == nil {
					change(&result)
				}
				return result, err
			})
			if calls != 1 || analysis.Error() == nil {
				t.Fatal("malformed selected tokens were accepted or replaced by the Go lexer")
			}
		})
	}
}

func TestSemanticExplicitLexerFailureAndInvalidSourceDoNotFallback(t *testing.T) {
	input := lexicalModuleInput(`public Class Root { public int Value() => 42; }`)
	failure := errors.New("selected lexer failed")
	calls := 0
	analysis := AnalyzeSemanticModuleSetWithLexer(input, func(file *SourceFile) (LexicalResult, error) {
		calls++
		return LexicalResult{}, failure
	})
	if calls != 1 || analysis.Error() == nil {
		t.Fatal("selected component failure fell back to Go")
	}
	input = lexicalModuleInput(string([]byte{0xff}))
	reference := AnalyzeSemanticModuleSet(input)
	calls = 0
	analysis = AnalyzeSemanticModuleSetWithLexer(input, func(file *SourceFile) (LexicalResult, error) {
		calls++
		return LexicalResult{}, nil
	})
	if calls != 0 || !reflect.DeepEqual(reference.Diagnostics, analysis.Diagnostics) {
		t.Fatal("invalid UTF-8 bypassed source admission or reached the selected lexer")
	}
}
