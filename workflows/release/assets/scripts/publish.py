#!/usr/bin/env python3
"""Publish only exact, attested, natively accepted staging bytes; never replace assets."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import urllib.error
import urllib.request

import release_contract as contract
import qualify


def run(arguments):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=300)
    if result.returncode:
        raise ValueError('Release operation failed: ' + ' '.join(arguments[:3]))
    if len(result.stdout) > 2 << 20:
        raise ValueError('Release response exceeds capacity')
    return result.stdout


def public_bytes(url, maximum):
    # Deliberately anonymous: no GH_TOKEN, Authorization header or ambient credentials.
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(maximum + 1)
    if len(data) > maximum:
        raise ValueError('Anonymous release response exceeds capacity')
    return data


def validate(directory, manifest_sha, source):
    metadata = contract.manifest(directory, manifest_sha, source)
    names = {'release-manifest.json', 'SHA256SUMS.txt'}
    for target in contract.TARGETS:
        contract.verify_archive(directory, metadata, target)
        names.add(metadata['artifacts'][target]['name'])
        receipt_name = 'native-' + target.replace('/', '-') + '.json'
        receipt = json.loads(contract.bounded(directory / receipt_name, 16 << 10))
        required = {'schema': 1, 'source_commit': source, 'version': metadata['version'], 'platform': target,
                    'manifest_sha256': manifest_sha, 'archive_sha256': metadata['artifacts'][target]['sha256'],
                    'fixture_sha256': contract.digest(qualify.FIXTURE.encode()), 'checks': contract.CHECKS,
                    'scope': 'native-standalone-acceptance-not-full-conformance'}
        if receipt != required:
            raise ValueError('Native qualification differs from the exact release input')
        names.add(receipt_name)
    checksum = ''.join(entry['sha256'] + '  ' + entry['name'] + '\n' for entry in metadata['artifacts'].values())
    if contract.bounded(directory / 'SHA256SUMS.txt', 4096).decode() != checksum:
        raise ValueError('Checksum inventory differs')
    if directory.is_symlink() or {path.name for path in directory.iterdir()} != names:
        raise ValueError('Release directory has unexpected or missing files')
    assets = {}
    for name in sorted(names):
        data = contract.bounded(directory / name, contract.MAX_ARCHIVE)
        assets[name] = {'size': len(data), 'digest': 'sha256:' + contract.digest(data)}
    return metadata, assets


def verify_release(release, tag, source, assets):
    if (release.get('tag_name') != tag or release.get('target_commitish') != source
            or release.get('prerelease') is not True):
        raise ValueError('Existing release source identity differs')
    remote = release.get('assets', [])
    if len(remote) != len(assets) or {item.get('name') for item in remote} != set(assets):
        raise ValueError('Existing release inventory differs; assets are never replaced')
    for item in remote:
        if any(item.get(key) != value for key, value in assets[item['name']].items()):
            raise ValueError('Existing release bytes differ; assets are never replaced')


def verify_tag(tag, source):
    reference = json.loads(run(['gh', 'api', f'repos/{contract.REPOSITORY}/git/ref/tags/{tag}']))['object']
    for _ in range(3):
        if reference.get('type') == 'commit' and reference.get('sha') == source:
            return
        if reference.get('type') != 'tag':
            break
        reference = json.loads(run(['gh', 'api', f'repos/{contract.REPOSITORY}/git/tags/{reference["sha"]}']))['object']
    raise ValueError('Release tag does not resolve to the accepted source commit')


def find_release(tag):
    # A bounded page query avoids treating network/auth failures as a missing release.
    releases = json.loads(run(['gh', 'api', f'repos/{contract.REPOSITORY}/releases?per_page=100']))
    matches = [item for item in releases if item.get('tag_name') == tag]
    if len(matches) > 1 or len(releases) == 100 and not matches:
        raise ValueError('Release inventory is ambiguous or exceeds this query capacity')
    return matches[0] if matches else None


def publish(directory, manifest_sha):
    source = os.environ.get('GITHUB_SHA', '')
    metadata, assets = validate(directory, manifest_sha, source)
    tag = 'v' + metadata['version']
    if (os.environ.get('GITHUB_REPOSITORY') != contract.REPOSITORY
            or os.environ.get('GITHUB_EVENT_NAME') != 'push'
            or os.environ.get('GITHUB_REF') != 'refs/tags/' + tag):
        raise ValueError('Publication requires the exact fresh staging source-tag push')
    verify_tag(tag, source)
    for name in assets:
        run(['gh', 'attestation', 'verify', str(directory / name), '--repo', contract.REPOSITORY,
             '--signer-workflow', contract.REPOSITORY + '/.github/workflows/release.yml',
             '--source-digest', source, '--source-ref', 'refs/tags/' + tag,
             '--signer-digest', source, '--deny-self-hosted-runners'])
    release = find_release(tag)
    if release is None:
        run(['gh', 'release', 'create', tag, *[str(directory / name) for name in assets],
             '--repo', contract.REPOSITORY, '--verify-tag', '--target', source,
             '--draft', '--prerelease', '--latest=false', '--title', 'PipeLang ' + tag,
             '--notes', 'Source-bound staging compiler archives, accepted on five native platforms. '
                        'Baseline acceptance only; full compiler conformance remains separate.'])
        release = find_release(tag)
    if release is None:
        raise ValueError('Created release is absent from inventory')
    verify_release(release, tag, source, assets)
    verify_tag(tag, source)
    if release.get('draft') is True:
        run(['gh', 'release', 'edit', tag, '--repo', contract.REPOSITORY,
             '--draft=false', '--prerelease', '--latest=false'])
    final = json.loads(run(['gh', 'api', f'repos/{contract.REPOSITORY}/releases/{release["id"]}']))
    verify_release(final, tag, source, assets)
    if final.get('draft') is not False:
        raise ValueError('Release remains a draft')
    verify_tag(tag, source)
    for name, expected in assets.items():
        url = f'https://github.com/{contract.REPOSITORY}/releases/download/{tag}/{name}'
        data = public_bytes(url, expected['size'])
        if len(data) != expected['size'] or 'sha256:' + contract.digest(data) != expected['digest']:
            raise ValueError('Anonymous release readback differs')
    print(final['html_url'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    options = parser.parse_args()
    publish(options.directory, options.manifest_sha256)
