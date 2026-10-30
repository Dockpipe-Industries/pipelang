package nativeviewparser

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"reflect"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativelexer"
)

func TestNativeViewParserWithNativeLexer(t *testing.T) {
	parser := configuredParser(t)
	path := os.Getenv("PIPELANG_CPP_LEXER")
	if path == "" {
		t.Skip("explicit native lexer required for combined component check")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(data)
	lexer := nativelexer.Lexer{Path: path, SHA256: hex.EncodeToString(hash[:])}
	for _, text := range []string{
		`view V { Label(text:"Rider🚲"); }`,
		`view V { state b:bool=true; event e(bool); Toggle(value:bind b,changed:e); }`,
		`view V { state n:int=-9223372036854775808; action edit(int)=>set n; action saveSettings()=>save; action reset()=>revert; Column() { Label(value:bind n); Label(value:bind saveSettings.error); Button(clicked:reset); } }`,
		`view V { Label(text:"bad\q"); }`,
		`view V { Label(value:bind n.); }`,
		`view V { Label(a:1,a:2); }`,
	} {
		file := &pipelang.SourceFile{ID: "v.pipe", Text: text}
		expected, expectedError := pipelang.ParseViewSource(file, nil)
		actual, actualError := pipelang.ParseViewSourceWithParser(file, lexer.Tokenize, parser.Parse)
		if !reflect.DeepEqual(expected, actual) || fmt.Sprint(expectedError) != fmt.Sprint(actualError) {
			t.Fatalf("combined components changed syntax: Go=%#v / %v native=%#v / %v", expected, expectedError, actual, actualError)
		}
	}
}
