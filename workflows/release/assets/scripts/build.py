#!/usr/bin/env python3
"""Cross-build source-bound archives; native qualification and publication are separate."""
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import zipfile

import release_contract as contract

ROOT = Path(__file__).resolve().parents[4]


def main():
    version = (ROOT / 'VERSION').read_text().strip()
    source = os.environ.get('GITHUB_SHA', '')
    contract.identity(version, source)
    actual_go = subprocess.check_output(['go', 'env', 'GOVERSION'], text=True, timeout=15).strip()
    if actual_go != contract.GO_VERSION:
        raise ValueError('Release requires the exact pinned Go toolchain')
    binary = os.environ['DOCKPIPE_BIN']
    output = Path(subprocess.check_output([binary, 'scope', 'artifacts', 'release'], text=True, timeout=15).strip())
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('Release output must be fresh; existing assets are never replaced')
    records = {}
    for target in contract.TARGETS:
        system, architecture = target.split('/')
        archive = output / contract.archive_name(version, target)
        environment = dict(os.environ, GOWORK='off', GOTOOLCHAIN='local', CGO_ENABLED='0', GOOS=system, GOARCH=architecture)
        with tempfile.TemporaryDirectory(prefix='pipelang-release-') as temporary:
            stage = Path(temporary)
            for command in contract.COMMANDS:
                executable = command + ('.exe' if system == 'windows' else '')
                arguments = ['go', 'build', '-buildvcs=false', '-trimpath']
                if command == 'pipelang':
                    arguments += ['-ldflags', f'-X main.releaseVersion={version} -X main.releaseSource={source}']
                arguments += ['-o', str(stage / executable), './src/cmd/' + command]
                subprocess.run(arguments, cwd=ROOT, env=environment, check=True, timeout=900)
            for filename in ('LICENSE', 'README.md'):
                (stage / filename).write_bytes((ROOT / filename).read_bytes())
            (stage / 'VERSION').write_text(version + '\n')
            (stage / 'SOURCE_COMMIT').write_text(source + '\n')
            inventory = {path.name: {'sha256': contract.digest(contract.bounded(path, contract.MAX_MEMBER)),
                                    'size_bytes': path.stat().st_size} for path in sorted(stage.iterdir())}
            if system == 'windows':
                with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as stream:
                    for path in sorted(stage.iterdir()):
                        stream.write(path, path.name)
            else:
                with tarfile.open(archive, 'w:gz') as stream:
                    for path in sorted(stage.iterdir()):
                        stream.add(path, arcname=path.name)
        data = contract.bounded(archive, contract.MAX_ARCHIVE)
        records[target] = {'name': archive.name, 'sha256': contract.digest(data),
                           'size_bytes': len(data), 'files': inventory}
    metadata = {'schema': 2, 'version': version, 'source_repository': contract.REPOSITORY,
                'source_commit': source, 'go_version': actual_go, 'artifacts': records}
    manifest = output / 'release-manifest.json'
    manifest.write_text(json.dumps(metadata, indent=2) + '\n')
    (output / 'SHA256SUMS.txt').write_text(''.join(entry['sha256'] + '  ' + entry['name'] + '\n'
                                                for entry in records.values()))
    environment_file = os.environ.get('GITHUB_ENV')
    if environment_file:
        with open(environment_file, 'a', encoding='utf-8') as stream:
            stream.write('PIPELANG_RELEASE_DIR=' + str(output) + '\n')
    output_file = os.environ.get('GITHUB_OUTPUT')
    if output_file:
        with open(output_file, 'a', encoding='utf-8') as stream:
            stream.write('manifest_sha256=' + contract.digest(manifest.read_bytes()) + '\n')
    print(output)


if __name__ == '__main__':
    main()
