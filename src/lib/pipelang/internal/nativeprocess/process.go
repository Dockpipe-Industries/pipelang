// Package nativeprocess owns the bounded executable boundary for optional native
// compiler components. Language semantics and response schemas stay with callers.
package nativeprocess

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"time"
	"unicode/utf8"
)

const maximumExecutableBytes = 256 << 20

type Executable struct {
	Path   string
	SHA256 string
	Label  string
}

func (executable Executable) Verify() error {
	if !filepath.IsAbs(executable.Path) {
		return fmt.Errorf("%s requires an explicit absolute executable path", executable.Label)
	}
	expected, err := hex.DecodeString(executable.SHA256)
	if err != nil || len(expected) != sha256.Size || hex.EncodeToString(expected) != executable.SHA256 {
		return fmt.Errorf("%s requires an exact lowercase SHA-256", executable.Label)
	}
	file, err := os.Open(executable.Path)
	if err != nil {
		return fmt.Errorf("open %s: %w", executable.Label, err)
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() || info.Size() > maximumExecutableBytes {
		return fmt.Errorf("%s must be a regular executable within 256 MiB", executable.Label)
	}
	hash := sha256.New()
	count, err := io.Copy(hash, io.LimitReader(file, maximumExecutableBytes+1))
	if err != nil {
		return err
	}
	if count > maximumExecutableBytes || !bytes.Equal(hash.Sum(nil), expected) {
		return fmt.Errorf("%s executable SHA-256 mismatch", executable.Label)
	}
	return nil
}

// boundedOutput continues draining a pipe after its retained limit, preventing
// malformed child output from growing memory or leaving a blocked writer.
type boundedOutput struct {
	bytes.Buffer
	limit    int
	overflow bool
}

func (output *boundedOutput) Write(data []byte) (int, error) {
	count := len(data)
	remaining := output.limit - output.Len()
	if count > remaining {
		output.overflow = true
		data = data[:remaining]
	}
	_, _ = output.Buffer.Write(data)
	return count, nil
}

// Run verifies executable bytes before and after one bounded request. It never
// searches for another executable or substitutes an implementation on failure.
func Run(ctx context.Context, executable Executable, request []byte, responseLimit int) ([]byte, error) {
	if responseLimit <= 0 || responseLimit > 32<<20 {
		return nil, errors.New("native compiler response bound must be within 32 MiB")
	}
	if err := executable.Verify(); err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, executable.Path)
	command.Stdin = bytes.NewReader(request)
	output := &boundedOutput{limit: responseLimit}
	diagnostic := &boundedOutput{limit: 64 << 10}
	command.Stdout = output
	command.Stderr = diagnostic
	command.WaitDelay = time.Second
	executionError := command.Run()
	if err := executable.Verify(); err != nil {
		return nil, fmt.Errorf("%s changed during execution: %w", executable.Label, err)
	}
	if executionError != nil {
		if ctx.Err() != nil {
			return nil, fmt.Errorf("%s cancelled or timed out: %w", executable.Label, ctx.Err())
		}
		return nil, fmt.Errorf("%s failed: %w: %s", executable.Label, executionError, diagnostic.String())
	}
	if output.overflow || diagnostic.overflow || !utf8.Valid(output.Bytes()) {
		return nil, fmt.Errorf("%s exceeded its response contract", executable.Label)
	}
	return output.Bytes(), nil
}
