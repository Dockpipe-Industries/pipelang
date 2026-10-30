#!/usr/bin/env python3
"""Check normal and unsafe floating compiler modes inside a contained unit."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/containedexec'))
from job import current_group, verify_job


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', type=Path, required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    args = parser.parse_args()
    verify_job()
    expected = {'memory.max': 1 << 30, 'memory.swap.max': 0, 'pids.max': 128}
    if {key: int((current_group() / key).read_text()) for key in expected} != expected:
        raise RuntimeError('Numeric compiler-mode tests require containedexec unit limits')
    command = [str(args.cxx), '-std=c++20', '-fsyntax-only', '-x', 'c++',
               '-I' + str(args.include), str(args.fixture)]
    for flags in ([], ['-ffast-math'], ['-ffinite-math-only']):
        result = subprocess.run([*command, *flags], capture_output=True, text=True, timeout=30)
        if not flags:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0, flags
            assert 'require strict IEEE' in result.stderr, result.stderr
    print('normal binary64 compilation passed; unsafe floating modes refused')


if __name__ == '__main__':
    main()
