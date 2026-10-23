package application

import (
	"time"

	"github.com/Dockpipe-Industries/pipelang/sdk/materialize"
)

type workflowTypeMapDoc struct {
	Types []string `yaml:"types"`
}

func dedupeAbsExistingDirs(paths []string) []string {
	return materialize.ExistingRoots(paths)
}

func detectPipeLangModuleRoot(startDir string) string {
	return materialize.DetectModuleRoot(startDir)
}

func readPipeFilesUnder(root string) (map[string][]byte, time.Time, error) {
	return materialize.ReadFilesUnder(root)
}

func materializePipeLangRoots(roots []string, force bool, outBase string) (int, error) {
	return materialize.MaterializeRoots(roots, force, outBase)
}
