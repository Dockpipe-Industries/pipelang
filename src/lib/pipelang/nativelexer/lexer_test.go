package nativelexer_test

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"math/rand"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/nativelexer"
)

const maximumSourceBytes = 65536

func TestNativeLexerDifferential(t *testing.T) {
	lexer := configuredLexer(t)
	cases := []struct {
		name string
		text string
	}{
		{"empty", ""},
		{"whitespace", " \t\n\r\v\f"},
		{"keywords", "Interface Class Struct public private true false"},
		{"keyword_prefixes", "InterfaceX Class2 Struct_ publicize private_ true1 falsehood interface class struct Public Private TRUE FALSE"},
		{"identifiers", "_ _a a0 A_Z z9 _123"},
		{"every_operator", "{ } ( ) [ ] : ; , . ? = => + - * / ! == != < <= > >= && ||"},
		{"maximal_munch", "====>=>==!!=!<<=>>=&&&&||||+-*/{}()[]:;,.?"},
		{"single_ampersand", "prefix && & suffix"},
		{"single_pipe", "prefix || | suffix"},
		{"comments", "// first\r\nname// second\n42 // trailing"},
		{"comment_cr_without_lf", "name // comment\r ignored"},
		{"comment_markers_in_text", `"// text" / / // comment`},
		{"comment_unusual_bytes", "// \x00\x7f café\r\nnext"},
		{"integers", "0 000 1234567890123456789012345678901234567890"},
		{"decimals", "0.0 000.123 12.345 1.2.3 .5 1e3 0xFF"},
		{"decimal_eof", "prefix 12."},
		{"decimal_non_digit", "prefix 12.x"},
		{"decimal_space", "prefix 12. "},
		{"decimal_second_dot", "prefix 12..5"},
		{"empty_text", `""`},
		{"text_then_identifier", `"abc"next`},
		{"unterminated_text", `prefix "abc`},
		{"unterminated_escape", "prefix \"abc\\"},
		{"hex_escape", `"\x41"`},
		{"unicode_escape", `"\u0041"`},
		{"unicode_long_escape", `"\U00000041"`},
		{"octal_escape", `"\141"`},
		{"utf8_text", `"café 日本語 🏍"`},
		{"escaped_utf8_two_bytes", `"\é"`},
		{"escaped_utf8_three_bytes", `"\日"`},
		{"escaped_utf8_four_bytes", `"\🏍"`},
		{"utf8_identifier", "name café"},
		{"utf8_whitespace", "name\u00a0next"},
		{"utf8_bom", "\ufeffname"},
		{"embedded_nul", "name\x00next"},
		{"embedded_nul_in_text", "\"before\x00after\""},
		{"maximum_whitespace", strings.Repeat(" ", maximumSourceBytes)},
		{"maximum_identifier", strings.Repeat("a", maximumSourceBytes)},
		{"maximum_dense_operators", strings.Repeat("+", maximumSourceBytes)},
		{"maximum_text", "\"" + strings.Repeat("a", maximumSourceBytes-2) + "\""},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			compareLexers(t, lexer, []byte(testCase.text))
		})
	}

	// The Go oracle decides which two-byte escapes are accepted. This includes
	// every ASCII byte, so DEL and escaped control characters cannot be missed.
	for escapedByte := 0; escapedByte <= 127; escapedByte++ {
		t.Run(fmt.Sprintf("escape_%02x", escapedByte), func(t *testing.T) {
			source := []byte{'"', 'p', '\\', byte(escapedByte), 'q', '"'}
			compareLexers(t, lexer, source)
		})
	}

	controlBytes := []byte{127}
	for controlByte := byte(0); controlByte < 32; controlByte++ {
		controlBytes = append(controlBytes, controlByte)
	}
	for _, controlByte := range controlBytes {
		t.Run(fmt.Sprintf("control_outside_%02x", controlByte), func(t *testing.T) {
			compareLexers(t, lexer, []byte{'a', ' ', controlByte, ' ', 'b'})
		})
		t.Run(fmt.Sprintf("control_inside_%02x", controlByte), func(t *testing.T) {
			compareLexers(t, lexer, []byte{'"', 'a', controlByte, 'b', '"'})
		})
	}

	// A reproducible mutation corpus mixes valid and invalid lexical contexts.
	// Keep execution sequential and short: each comparison starts one child.
	random := rand.New(rand.NewSource(482719))
	seeds := []string{
		`Class Bike { public speed: Float = 12.5; }`,
		`Interface Ride { turn(left: Bool) => Text; }`,
		"// track\r\nname != \"a\\n\" && true",
		`0.1 + value_2 <= 42 || false`,
	}
	alphabet := []byte("abcXYZ_0129.\"\\/\r\n\t :=>!&|{}[]()+-*?;,@\x00\x7f")
	for index := 0; index < 96; index++ {
		source := []byte(seeds[index%len(seeds)])
		mutationCount := 1 + random.Intn(5)
		for mutation := 0; mutation < mutationCount; mutation++ {
			position := random.Intn(len(source) + 1)
			switch random.Intn(3) {
			case 0:
				source = append(source, 0)
				copy(source[position+1:], source[position:])
				source[position] = alphabet[random.Intn(len(alphabet))]
			case 1:
				if position < len(source) {
					source[position] = alphabet[random.Intn(len(alphabet))]
				}
			case 2:
				if position < len(source) {
					source = append(source[:position], source[position+1:]...)
				}
			}
		}
		t.Run(fmt.Sprintf("mutation_%03d", index), func(t *testing.T) {
			compareLexers(t, lexer, source)
		})
	}
}

func TestNativeLexerFixtureDifferential(t *testing.T) {
	root := os.Getenv("PIPELANG_LEXER_FIXTURES")
	if root == "" {
		t.Skip("set PIPELANG_LEXER_FIXTURES to opt into a .pipe fixture corpus")
	}
	lexer := configuredLexer(t)
	var paths []string
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, walkError error) error {
		if walkError != nil {
			return walkError
		}
		if entry.IsDir() {
			if entry.Name() == ".git" || entry.Name() == "bin" {
				return filepath.SkipDir
			}
			return nil
		}
		if entry.Type().IsRegular() && filepath.Ext(path) == ".pipe" {
			paths = append(paths, path)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	sort.Strings(paths)
	compared := 0
	for _, path := range paths {
		file, err := os.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		source, readError := io.ReadAll(io.LimitReader(file, maximumSourceBytes+1))
		closeError := file.Close()
		if readError != nil || closeError != nil {
			t.Fatalf("read fixture %s: read=%v close=%v", path, readError, closeError)
		}
		if len(source) > maximumSourceBytes {
			t.Logf("skip fixture above native source bound: %s", path)
			continue
		}
		compared++
		relativePath, err := filepath.Rel(root, path)
		if err != nil {
			t.Fatal(err)
		}
		t.Run(filepath.ToSlash(relativePath), func(t *testing.T) {
			compareLexers(t, lexer, source)
		})
	}
	if compared == 0 {
		t.Fatal("fixture root contained no regular .pipe files within the source bound")
	}
	t.Logf("compared %d fixture files", compared)
}

func TestNativeLexerRefusesMissingIdentity(t *testing.T) {
	file := sourceFile(t, []byte("name"))
	missingPath := filepath.Join(t.TempDir(), "missing-lexer")
	cases := []nativelexer.Lexer{
		{Path: "relative-lexer", SHA256: strings.Repeat("a", 64)},
		{Path: missingPath},
		{Path: missingPath, SHA256: strings.Repeat("A", 64)},
		{Path: missingPath, SHA256: strings.Repeat("a", 64)},
	}
	for index, lexer := range cases {
		t.Run(fmt.Sprintf("identity_%d", index), func(t *testing.T) {
			assertInfrastructureFailure(t, lexer, file)
		})
	}
}

func TestNativeLexerRefusesChangedIdentityAndCancellation(t *testing.T) {
	lexer := configuredLexer(t)
	file := sourceFile(t, []byte("name"))
	t.Run("checksum_mismatch", func(t *testing.T) {
		changed := lexer
		if changed.SHA256[0] == '0' {
			changed.SHA256 = "1" + changed.SHA256[1:]
		} else {
			changed.SHA256 = "0" + changed.SHA256[1:]
		}
		assertInfrastructureFailure(t, changed, file)
	})
	t.Run("cancelled_context", func(t *testing.T) {
		cancelled := lexer
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		cancelled.Context = ctx
		assertInfrastructureFailure(t, cancelled, file)
	})
	t.Run("source_above_limit", func(t *testing.T) {
		assertInfrastructureFailure(t, lexer, sourceFile(t, []byte(strings.Repeat("a", maximumSourceBytes+1))))
	})
}

func TestMalformedUTF8IsRejectedBeforeLexing(t *testing.T) {
	for _, data := range [][]byte{{0xff}, {'a', 0xc3}, {'"', 0x80, '"'}, {0xed, 0xa0, 0x80}} {
		sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: "invalid.pipe", Data: data}})
		if len(sources.Files()) != 0 || len(diagnostics) != 1 || diagnostics[0].Code != pipelang.CodeInvalidUTF8 {
			t.Fatalf("invalid UTF-8 %x reached lexer admission: files=%d diagnostics=%v", data, len(sources.Files()), diagnostics)
		}
	}
}

func configuredLexer(t *testing.T) nativelexer.Lexer {
	t.Helper()
	path := os.Getenv("PIPELANG_CPP_LEXER")
	if path == "" {
		t.Skip("set PIPELANG_CPP_LEXER to opt into native lexer subprocess tests")
	}
	if !filepath.IsAbs(path) {
		t.Fatal("PIPELANG_CPP_LEXER must be an absolute executable path")
	}
	binary, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.New()
	_, hashError := io.Copy(digest, binary)
	closeError := binary.Close()
	if hashError != nil || closeError != nil {
		t.Fatalf("hash native lexer: hash=%v close=%v", hashError, closeError)
	}
	return nativelexer.Lexer{Path: path, SHA256: hex.EncodeToString(digest.Sum(nil)), Context: context.Background()}
}

func sourceFile(t *testing.T, data []byte) *pipelang.SourceFile {
	t.Helper()
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: "differential.pipe", Data: data}})
	if len(diagnostics) != 0 || len(sources.Files()) != 1 {
		t.Fatalf("source admission failed: %v", diagnostics)
	}
	return sources.Files()[0]
}

func compareLexers(t *testing.T, lexer nativelexer.Lexer, data []byte) {
	t.Helper()
	file := sourceFile(t, data)
	expected, err := pipelang.TokenizeSource(file)
	if err != nil {
		t.Fatalf("Go lexical oracle infrastructure failure: %v", err)
	}
	actual, err := lexer.Tokenize(file)
	if err != nil {
		t.Fatalf("native lexer infrastructure failure for %d source bytes: %v", len(data), err)
	}
	if reflect.DeepEqual(expected, actual) {
		return
	}
	if !reflect.DeepEqual(expected.Failure, actual.Failure) {
		t.Fatalf("lexical failure differs: Go=%+v native=%+v", expected.Failure, actual.Failure)
	}
	if len(expected.Tokens) != len(actual.Tokens) {
		t.Fatalf("token counts differ: Go=%d native=%d", len(expected.Tokens), len(actual.Tokens))
	}
	for index := range expected.Tokens {
		if !reflect.DeepEqual(expected.Tokens[index], actual.Tokens[index]) {
			t.Fatalf("token %d differs: Go=%+v native=%+v", index, expected.Tokens[index], actual.Tokens[index])
		}
	}
	t.Fatalf("lexical result representation differs: Go tokens nil=%t native tokens nil=%t", expected.Tokens == nil, actual.Tokens == nil)
}

func assertInfrastructureFailure(t *testing.T, lexer nativelexer.Lexer, file *pipelang.SourceFile) {
	t.Helper()
	result, err := lexer.Tokenize(file)
	if err == nil {
		t.Fatalf("native refusal returned no infrastructure error (fallback may have occurred): %+v", result)
	}
	if result.Failure != nil || len(result.Tokens) != 0 {
		t.Fatalf("native refusal returned lexical output: %+v", result)
	}
}
