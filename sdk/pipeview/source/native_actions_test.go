package source_test

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"reflect"
	"testing"

	viewsource "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativelexer"
)

func TestNativeActionSourceReference(t *testing.T) {
	path := os.Getenv("PIPELANG_CPP_LEXER")
	if path == "" {
		t.Skip("explicit native lexer required")
	}
	bytes, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(bytes)
	lexer := nativelexer.Lexer{Path: path, SHA256: hex.EncodeToString(sum[:])}
	cases := []string{
		`view V { state s:string="Rider"; action edit(string)=>set s; action save()=>save; Column(id:"root"){ TextField(id:"name",value:bind s,changed:edit); Label(id:"status",value:bind save.status); Button(id:"save",clicked:save); } }`,
		`view V { state b:bool=true; action edit(bool)=>set b; Toggle(id:"toggle",value:bind b,changed:edit); }`,
		`view V { state n:int=-9223372036854775808; action save()=>save; Column(id:"root"){ Label(id:"n",value:bind n); Button(id:"save",clicked:save); } }`,
		`view V { state s:string="Rider"; action edit(bool)=>set s; TextField(id:"s",value:bind s,changed:edit); }`,
		`view V { action save(string)=>save; Button(id:"save",clicked:save); }`,
		`view V { state s:string="Rider"; action edit(string)=>set missing; TextField(id:"s",value:bind s,changed:edit); }`,
		`view V { action save()=>save; Label(id:"s",value:bind missing.status); }`,
		`view V { action save()=>save; Label(id:"s",value:bind save.unknown); }`,
		`view V { state s:string="Rider"; action edit(string)=>set s; action save()=>save; TextField(id:"s",value:bind save.status,changed:edit); }`,
		`view V { action save()=>unknown; Button(id:"save",clicked:save); }`,
		`view V { action save()=>save("unexpected"); Button(id:"save",clicked:save); }`,
		`view V { event e(); action save()=>save; Button(id:"save",clicked:save); }`,
	}
	for index, body := range cases {
		t.Run(fmt.Sprint(index), func(t *testing.T) {
			sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: "Actions.pipe", Data: []byte(body)}})
			if diagnostics.HasErrors() {
				t.Fatal(diagnostics)
			}
			file := sources.Files()[0]
			reference, referenceError := viewsource.CompileProgram(file, nil)
			native, nativeError := viewsource.CompileProgram(file, lexer.Tokenize)
			if fmt.Sprint(referenceError) != fmt.Sprint(nativeError) {
				t.Fatalf("diagnostic differs: %v / %v", referenceError, nativeError)
			}
			if referenceError != nil {
				return
			}
			expected, err := reference.Document.Inspect()
			if err != nil {
				t.Fatal(err)
			}
			actual, err := native.Document.Inspect()
			if err != nil || !reflect.DeepEqual(expected, actual) || !reflect.DeepEqual(reference.Definition, native.Definition) || !reflect.DeepEqual(reference.StatusBindings, native.StatusBindings) {
				t.Fatalf("native action source differs: %v", err)
			}
		})
	}
}
