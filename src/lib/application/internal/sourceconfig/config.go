// Package sourceconfig reads the optional Dockpipe workflow discovery contract.
// PipeLang only consumes compile.workflows; it does not load the Dockpipe engine.
package sourceconfig

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
)

type Config struct {
	Compile struct {
		Workflows []string `json:"workflows"`
	} `json:"compile"`
}

func Load(root string) (*Config, error) {
	data, err := os.ReadFile(filepath.Join(root, "dockpipe.config.json"))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var config Config
	if err := json.Unmarshal(data, &config); err != nil {
		return nil, err
	}
	return &config, nil
}

func WorkflowRoots(config *Config, root string) []string {
	paths := []string{"workflows"}
	if config != nil && config.Compile.Workflows != nil {
		paths = config.Compile.Workflows
	}
	roots := make([]string, 0, len(paths))
	for _, path := range paths {
		if !filepath.IsAbs(path) {
			path = filepath.Join(root, path)
		}
		roots = append(roots, filepath.Clean(path))
	}
	return roots
}
