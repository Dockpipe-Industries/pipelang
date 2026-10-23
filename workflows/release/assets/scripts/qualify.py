#!/usr/bin/env python3
"""Run exact downloaded standalone bytes outside the checkout on the native host."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import tempfile

import release_contract as contract

FIXTURE = '''public Class ReleaseFixture
{
    public string Image = "release-proof";
    public int Count = 7;
    public int Result() => Count + 5;
}
'''


def native_target():
    system = platform.system().lower()
    architecture = {'x86_64': 'amd64', 'amd64': 'amd64', 'aarch64': 'arm64', 'arm64': 'arm64'}.get(platform.machine().lower())
    return system + '/' + str(architecture)


def execute(executable, arguments, directory, *, succeeds=True):
    result = subprocess.run([str(executable), *arguments], cwd=directory, capture_output=True,
                            text=True, timeout=60)
    if len(result.stdout) + len(result.stderr) > 256 << 10:
        raise ValueError('Compiler acceptance output exceeds capacity')
    if (result.returncode == 0) != succeeds:
        raise ValueError('Compiler acceptance command failed: ' + ' '.join(arguments[:1]) + '\n' +
                         (result.stdout + result.stderr)[-4096:])
    return result.stdout


def qualify(directory, expected_manifest, source, target, output):
    if target != native_target():
        raise ValueError('Qualification must execute on the exact native platform')
    metadata = contract.manifest(directory, expected_manifest, source)
    payload = contract.verify_archive(directory, metadata, target)
    with tempfile.TemporaryDirectory(prefix='pipelang-native-') as temporary:
        work = Path(temporary)
        for name, data in payload.items():
            path = work / name
            path.write_bytes(data)
            path.chmod(0o755)
        executable = work / ('pipelang.exe' if target.startswith('windows/') else 'pipelang')
        for command in contract.COMMANDS:
            frontend = work / (command + ('.exe' if target.startswith('windows/') else ''))
            execute(frontend, ['--help'], work)
        actual = execute(executable, ['--version'], work).strip()
        if actual != metadata['version'] + ' ' + source:
            raise ValueError('Native executable version/source mismatch')
        fixture = work / 'fixture.pipe'
        fixture.write_text(FIXTURE)
        valid = json.loads(execute(executable, ['check', '--in', str(fixture), '--format', 'json'], work))
        if valid.get('schema') != 1 or valid.get('diagnostics'):
            raise ValueError('Valid fixture diagnostic contract differs')
        broken = work / 'invalid' / 'broken.pipe'
        broken.parent.mkdir()
        broken.write_text('public Class Broken { public int Value = ; }')
        invalid = json.loads(execute(executable, ['check', '--in', str(broken), '--format', 'json'], work, succeeds=False))
        if invalid.get('schema') != 1 or not any(item.get('code') == 'PL2001' for item in invalid.get('diagnostics', [])):
            raise ValueError('Invalid fixture diagnostic contract differs')
        # Compilation discovers source sets recursively; keep the rejected fixture separate.
        broken.unlink()
        broken.parent.rmdir()
        compiled = work / 'compiled'
        execute(executable, ['compile', '--in', str(fixture), '--entry', 'ReleaseFixture', '--out', str(compiled)], work)
        expected = {'ReleaseFixture.workflow.yml', 'ReleaseFixture.bindings.json', 'ReleaseFixture.bindings.env'}
        if {path.name for path in compiled.iterdir()} != expected:
            raise ValueError('Compiler output inventory differs')
        bindings = json.loads((compiled / 'ReleaseFixture.bindings.json').read_text())
        if not isinstance(bindings, dict) or 'release-proof' not in (compiled / 'ReleaseFixture.workflow.yml').read_text():
            raise ValueError('Compiled output did not retain fixture semantics')
        invoked = json.loads(execute(executable, ['invoke', '--in', str(fixture), '--class', 'ReleaseFixture',
                                                  '--method', 'Result', '--format', 'json'], work))
        if invoked != {'class': 'ReleaseFixture', 'method': 'Result', 'type': 'int', 'value': 12}:
            raise ValueError('Compiler invocation result differs')
    receipt = {'schema': 1, 'source_commit': source, 'version': metadata['version'], 'platform': target,
               'manifest_sha256': expected_manifest, 'archive_sha256': metadata['artifacts'][target]['sha256'],
               'fixture_sha256': contract.digest(FIXTURE.encode()), 'checks': contract.CHECKS,
               'scope': 'native-standalone-acceptance-not-full-conformance'}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--platform', required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    qualify(arguments.directory, arguments.manifest_sha256, arguments.source, arguments.platform, arguments.output)
