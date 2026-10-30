package source_test

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativeviewparser"
)

func TestPercentNativeParserSpanAndAdmissionParity(t *testing.T) {
	path := os.Getenv("PIPELANG_CPP_VIEW_PARSER")
	if path == "" {
		t.Skip("explicit native view parser required")
	}
	binary, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(binary)
	parser := nativeviewparser.Parser{Path: path, SHA256: hex.EncodeToString(hash[:])}
	valid := `view V { state volume:int=100; action edit(int)=>set volume; PercentStepper(id:"volume",label:"Volume",value:bind volume,changed:edit); }`
	cases := []string{valid,
		strings.Replace(valid, "=100", "=-9223372036854775808", 1),
		strings.Replace(valid, "edit(int)", "edit(bool)", 1),
		strings.Replace(valid, "value:bind volume", "value:100", 1),
		strings.Replace(valid, "label:\"Volume\"", "maximum:100", 1),
		strings.Replace(valid, "changed:edit", "changed:missing", 1),
		strings.TrimSuffix(valid, "}"),
	}
	for _, text := range cases {
		file := &pipelang.SourceFile{ID: "percent.pipe", Text: text}
		expected, expectedError := pipelang.ParseViewSource(file, nil)
		actual, actualError := pipelang.ParseViewSourceWithParser(file, nil, parser.Parse)
		if !reflect.DeepEqual(expected, actual) || fmt.Sprint(expectedError) != fmt.Sprint(actualError) {
			t.Fatal("percent syntax/span/failure changed through native parser")
		}
		reference, referenceError := source.CompileProgram(file, nil)
		native, nativeError := source.CompileProgramWithParser(file, nil, parser.Parse)
		if fmt.Sprint(referenceError) != fmt.Sprint(nativeError) {
			t.Fatal("percent semantic refusal changed")
		}
		if referenceError != nil {
			continue
		}
		if !reflect.DeepEqual(reference.Definition, native.Definition) || !reflect.DeepEqual(reference.StatusBindings, native.StatusBindings) {
			t.Fatal("percent model plan changed")
		}
		options := forgepipe.Options{Scale: 1, Focus: "#FFAD45FF"}
		first, err := forgepipe.Encode(reference.Document, options)
		if err != nil {
			t.Fatal(err)
		}
		second, err := forgepipe.Encode(native.Document, options)
		if err != nil || string(first) != string(second) {
			t.Fatal("percent generated transport changed")
		}
	}
}
