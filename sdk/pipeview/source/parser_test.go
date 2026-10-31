package source_test

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"reflect"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe"
	viewsource "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativeviewparser"
)

func TestViewParserSDKFailurePropagation(t *testing.T) {
	file := &pipelang.SourceFile{ID: "v.pipe", Text: `view V { Label(id:"title",text:"Settings"); }`}
	failure := errors.New("selected syntax component failed")
	selected := func(*pipelang.SourceFile, []pipelang.LexicalToken) (pipelang.ViewSyntaxResult, error) {
		return pipelang.ViewSyntaxResult{}, failure
	}
	if _, err := viewsource.CompileWithParser(file, nil, selected); !errors.Is(err, failure) {
		t.Fatalf("Compile fallback: %v", err)
	}
	if _, err := viewsource.CompileProgramWithParser(file, nil, selected); !errors.Is(err, failure) {
		t.Fatalf("CompileProgram fallback: %v", err)
	}
}

func TestNativeViewParserProgramEquivalence(t *testing.T) {
	path := os.Getenv("PIPELANG_CPP_VIEW_PARSER")
	if path == "" {
		t.Skip("explicit native parser required")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(data)
	selected := nativeviewparser.Parser{Path: path, SHA256: hex.EncodeToString(hash[:])}
	cases := []string{
		`view V { Label(id:"title",text:"Settings"); }`,
		`view V { state b:bool=true; event e(bool); Toggle(id:"b",value:bind b,changed:e); }`,
		`view V { state n:int=-9223372036854775808; Label(id:"n",value:bind n); }`,
		`view V { state s:string="Rider🚲"; action edit(string)=>set s; action saveSettings()=>save; action revertSettings()=>revert; Column(id:"root") { TextField(id:"s",value:bind s,changed:edit); Button(id:"save",clicked:saveSettings); Button(id:"revert",clicked:revertSettings); Label(id:"status",value:bind saveSettings.status); } }`,
		`view V { Unknown(id:"x"); }`,
		`view V { state b:bool=1; Toggle(id:"x",value:bind b,changed:edit); }`,
		`view V { action edit(bool)=>set missing; Button(id:"x",clicked:edit); }`,
		`view V { action saveSettings(bool)=>save; Button(id:"x",clicked:saveSettings); }`,
		`view V { event e(); action a()=>revert; Button(id:"x",clicked:a); }`,
		`view V { state s:string="Rider"; action edit(bool)=>set s; TextField(id:"s",value:bind s,changed:edit); }`,
		`view V { action saveSettings()=>save; Column(id:"root") { Button(id:"save",clicked:saveSettings); TextField(id:"status",value:bind saveSettings.error,changed:saveSettings); } }`,
	}
	for index, text := range cases {
		t.Run(fmt.Sprint(index), func(t *testing.T) {
			file := &pipelang.SourceFile{ID: "v.pipe", Path: "v.pipe", Text: text}
			expected, expectedError := viewsource.CompileProgram(file, nil)
			actual, actualError := viewsource.CompileProgramWithParser(file, nil, selected.Parse)
			if index < 4 && expectedError != nil {
				t.Fatalf("valid fixture failed semantic admission: %v", expectedError)
			}
			if fmt.Sprint(expectedError) != fmt.Sprint(actualError) {
				t.Fatalf("semantic diagnostics differ: %v / %v", expectedError, actualError)
			}
			if expectedError != nil {
				return
			}
			if !reflect.DeepEqual(expected.Definition, actual.Definition) || !reflect.DeepEqual(expected.StatusBindings, actual.StatusBindings) {
				t.Fatal("typed model/action plan changed")
			}
			options := forgepipe.Options{Scale: 1.125, Focus: "#FFAD45FF"}
			expectedBytes, expectedError := forgepipe.Encode(expected.Document, options)
			actualBytes, actualError := forgepipe.Encode(actual.Document, options)
			if expectedError != nil || actualError != nil || string(expectedBytes) != string(actualBytes) {
				t.Fatalf("checked document projection changed: %v / %v", expectedError, actualError)
			}
			legacyExpected, legacyExpectedError := viewsource.Compile(file, nil)
			legacyActual, legacyActualError := viewsource.CompileWithParser(file, nil, selected.Parse)
			if fmt.Sprint(legacyExpectedError) != fmt.Sprint(legacyActualError) || !reflect.DeepEqual(legacyExpected, legacyActual) {
				t.Fatal("legacy Compile boundary changed")
			}
		})
	}
}
