package nativeviewparser

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func configuredParser(t *testing.T) Parser {
	t.Helper()
	path := os.Getenv("PIPELANG_CPP_VIEW_PARSER")
	if path == "" {
		t.Skip("set PIPELANG_CPP_VIEW_PARSER for contained native process tests")
	}
	if !filepath.IsAbs(path) {
		t.Fatal("native parser test path must be absolute")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(data)
	return Parser{Path: path, SHA256: hex.EncodeToString(hash[:])}
}

func compareParser(t *testing.T, selected Parser, text string) {
	t.Helper()
	file := &pipelang.SourceFile{ID: "parser-🚲.pipe", Path: "parser-🚲.pipe", Text: text}
	expected, expectedError := pipelang.ParseViewSource(file, nil)
	actual, actualError := pipelang.ParseViewSourceWithParser(file, nil, selected.Parse)
	if !reflect.DeepEqual(expected, actual) {
		t.Fatalf("declaration or span mismatch: expected=%#v actual=%#v error=%v", expected, actual, actualError)
	}
	if (expectedError == nil) != (actualError == nil) {
		t.Fatalf("error mismatch: %v / %v", expectedError, actualError)
	}
	if expectedError == nil {
		return
	}
	expectedDiagnostics, expectedSourceError := pipelang.AsDiagnostics(expectedError)
	actualDiagnostics, actualSourceError := pipelang.AsDiagnostics(actualError)
	if !expectedSourceError || !actualSourceError || !reflect.DeepEqual(expectedDiagnostics, actualDiagnostics) {
		t.Fatalf("diagnostics differ: Go=%v native=%v", expectedError, actualError)
	}
}

func TestNativeViewParserDifferential(t *testing.T) {
	parser := configuredParser(t)
	cases := []string{
		`view V { Label(); }`,
		`// heading
view V { Column() {} } // trailing`,
		`view V { state b:bool=true; state n:int=-9223372036854775808; state max:int=9223372036854775807; state s:string="Rider\n🚲\t\"\\"; event e(bool); event click(); Column(id:"root") { Toggle(value:bind b,changed:e); Label(text:"",value:bind n); Button(clicked:click); } }`,
		`view V { state b:bool=false; action edit(bool)=>set b; action saveSettings()=>save; action reset()=>revert; Column() { Toggle(value:bind b,changed:edit); Label(value:bind saveSettings.error); Button(clicked:reset); } }`,
		`view V { state n:int=-0; state k:int=00042; event e(unknown); action a(unknown)=>save; Unknown(value:reference); }`,
		`view V { state n:int=- 00042; Label(value:bind n); }`,
		``, ` `, `view`, `view V`, `view V {}`, `wrong V { Label(); }`,
		`view V { Label(a:1,a:2); }`, `view V { Label(a:1,); }`,
		`view V { Label(a:bind); }`, `view V { Label(a:bind n.); }`,
		`view V { Label(a:bind n.x.y); }`, `view V { Label(a:1.2); }`,
		`view V { Label(a:-); }`, `view V { Label(a:+1); }`,
		`view V { state n:int=value; Label(); }`,
		`view V { state n:int=9223372036854775808; Label(); }`,
		`view V { state n:int=-9223372036854775809; Label(); }`,
		`view V { action a()=>unknown; Label(); }`,
		`view V { action a()=>set; Label(); }`,
		`view V { Label(); Label(); }`, `view V { Column(){ Label(); }`,
		`view V { Label(); } trailing`, `view V { Label(text:"bad\q"); }`,
		`view V { Label(text:"unterminated); }`, `@`, "\xff",
	}
	cases = append(cases, "view V { action a()=>"+strings.Repeat("x", 60000)+"; Label(); }")
	for _, count := range []int{127, 128, 129} {
		cases = append(cases, "view V {"+strings.Repeat("state n:int=1;", count)+"Label();}")
		cases = append(cases, "view V {"+strings.Repeat("event e();", count)+"Label();}")
		cases = append(cases, "view V {"+strings.Repeat("action a()=>save;", count)+"Label();}")
		cases = append(cases, "view V {Column(){"+strings.Repeat("Label();", count-1)+"}}")
	}
	for _, depth := range []int{7, 8, 9} {
		cases = append(cases, "view V {"+strings.Repeat("Column(){", depth-1)+"Label();"+strings.Repeat("}", depth-1)+"}")
	}
	for _, length := range []int{65535, 65536, 65537} {
		base := `view V { Label(text:"") ; }`
		cases = append(cases, strings.Replace(base, `""`, `"`+strings.Repeat("x", length-len(base))+`"`, 1))
		cases = append(cases, strings.Repeat(" ", length))
	}
	// Every token truncation and single-token deletion from a declaration that
	// exercises all bounded productions supplies malformed and valid neighbors.
	seed := `view V { state b:bool=true; state s:string="é🚲"; event e(bool); action setB(bool)=>set b; action saveSettings()=>save; action revertSettings()=>revert; Column(id:"root") { Toggle(id:"b",value:bind b,changed:setB); Label(value:bind saveSettings.error); } }`
	file := &pipelang.SourceFile{ID: "seed.pipe", Text: seed}
	lexical, err := pipelang.TokenizeSource(file)
	if err != nil {
		t.Fatal(err)
	}
	for _, token := range lexical.Tokens {
		cases = append(cases, seed[:token.Start])
		if token.Kind != "eof" {
			cases = append(cases, seed[:token.Start]+seed[token.End:])
		}
	}
	for index, text := range cases {
		t.Run(fmt.Sprint(index), func(t *testing.T) { compareParser(t, parser, text) })
	}
}

func TestNativeViewParserHashAndCancellation(t *testing.T) {
	parser := configuredParser(t)
	file := &pipelang.SourceFile{ID: "view.pipe", Text: `view V { Label(); }`}
	lexical, _ := pipelang.TokenizeSource(file)
	badHash := parser
	badHash.SHA256 = strings.Repeat("0", 64)
	if result, err := badHash.Parse(file, lexical.Tokens); err == nil || result.Declaration != nil || result.Failure != nil {
		t.Fatalf("bad hash produced outcome: %#v / %v", result, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	parser.Context = ctx
	if result, err := parser.Parse(file, lexical.Tokens); !errors.Is(err, context.Canceled) || result.Declaration != nil || result.Failure != nil {
		t.Fatalf("cancelled parser produced outcome: %#v / %v", result, err)
	}
}
