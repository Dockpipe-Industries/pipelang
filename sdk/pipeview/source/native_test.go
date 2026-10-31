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
	viewsource "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativelexer"
)

func TestNativeViewLexerEquivalence(t *testing.T) {
	path := os.Getenv("PIPELANG_CPP_LEXER")
	if path == "" {
		t.Skip("explicit native lexer required")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(data)
	lexer := nativelexer.Lexer{Path: path, SHA256: hex.EncodeToString(hash[:])}
	cases := []string{
		`view V { Label(id:"title",text:"Settings"); }`,
		`view V { state n:int=-9223372036854775808; Column(id:"root"){ Label(id:"n",value:bind n); } }`,
		`view V { state b:bool=true; event e(bool); Toggle(id:"b",value:bind b,changed:e); }`,
		`view V { state s:string="Rider"; event e(string); TextField(id:"s",value:bind s,changed:e); }`,
	}
	for index := 0; index < 32; index++ {
		cases = append(cases, fmt.Sprintf("// case %d\nview V { state n:int=%d; Label(id:\"label\",text:\"Count: \",value:bind n); }", index, index-16))
	}
	for _, invalid := range []string{
		``, `view`, `view V {}`, `view V { Unknown(id:"x"); }`,
		`view V { Label(id:"x",id:"y"); }`, `view V { Label(id:"x",value:bind missing); }`,
		`view V { state n:bool="wrong"; Label(id:"x",value:bind n); }`,
		`view V { state n:int=9223372036854775808; Label(id:"x",value:bind n); }`,
		`view V { state s:string="bad\q"; Label(id:"x",value:bind s); }`,
		`view V { Label(id:"x",text:"unterminated); }`,
		`view V { Label(id:"x"); } @`, `view V { state n:int=1+2; Label(id:"x",value:bind n); }`,
		"view V { Label(id:\"x\",text:\"" + strings.Repeat("a", 4097) + "\"); }",
	} {
		cases = append(cases, invalid)
	}
	for index, source := range cases {
		t.Run(fmt.Sprint(index), func(t *testing.T) {
			sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: "View.pipe", Data: []byte(source)}})
			if diagnostics.HasErrors() {
				t.Fatal(diagnostics)
			}
			file := sources.Files()[0]
			reference, referenceError := pipelang.ParseViewSource(file, nil)
			native, nativeError := pipelang.ParseViewSource(file, lexer.Tokenize)
			if fmt.Sprint(referenceError) != fmt.Sprint(nativeError) || !reflect.DeepEqual(reference, native) {
				t.Fatalf("native syntax differs: Go=%#v/%v native=%#v/%v", reference, referenceError, native, nativeError)
			}
			referenceDocument, referenceError := viewsource.Compile(file, nil)
			nativeDocument, nativeError := viewsource.Compile(file, lexer.Tokenize)
			if fmt.Sprint(referenceError) != fmt.Sprint(nativeError) {
				t.Fatalf("native semantic diagnostic differs: %v / %v", referenceError, nativeError)
			}
			if referenceError != nil {
				return
			}
			options := forgepipe.Options{Scale: 1.125, Focus: "#FFAD45FF"}
			expected, err := forgepipe.Encode(referenceDocument, options)
			if err != nil {
				t.Fatal(err)
			}
			actual, err := forgepipe.Encode(nativeDocument, options)
			if err != nil || string(expected) != string(actual) {
				t.Fatalf("native projection differs: %v", err)
			}
		})
	}
}
