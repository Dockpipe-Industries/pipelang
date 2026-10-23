"""Bounded standalone archive identity shared by build, native checks and publication."""
import hashlib
import json
from pathlib import Path
import re
import stat
import tarfile
import zipfile

REPOSITORY = 'Dockpipe-Industries/pipelang'
GO_VERSION = 'go1.26.10'
TARGETS = ('linux/amd64', 'linux/arm64', 'darwin/amd64', 'darwin/arm64', 'windows/amd64')
COMMANDS = ('pipelang', 'pipelang-streamc', 'pipelang-native-cpp')
MAX_ARCHIVE = 128 << 20
MAX_MEMBER = 64 << 20
CHECKS = ['frontend-help', 'version-source', 'valid-check', 'invalid-diagnostic', 'compile-artifacts', 'invoke-result']


def digest(data):
    return hashlib.sha256(data).hexdigest()


def bounded(path, limit):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise ValueError('Expected a bounded regular release file')
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError('Release file exceeds capacity')
    return data


def identity(version, source):
    if not re.fullmatch(r'[0-9a-f]{40}', source):
        raise ValueError('Expected an exact source commit')
    if not re.fullmatch(r'0\.1\.[1-9][0-9]*-staging\.[1-9][0-9]*', version):
        raise ValueError('Expected a fresh 0.1.x-staging.N compiler version')


def archive_name(version, target):
    system, architecture = target.split('/')
    return f'pipelang_{version}_{system}_{architecture}' + ('.zip' if system == 'windows' else '.tar.gz')


def members(path, target):
    expected = set(COMMANDS) | {'LICENSE', 'README.md', 'VERSION', 'SOURCE_COMMIT'}
    if target == 'windows/amd64':
        expected = {name + '.exe' if name in COMMANDS else name for name in expected}
        with zipfile.ZipFile(path) as archive:
            entries = archive.infolist()
            if len(entries) != len(expected) or {item.filename for item in entries} != expected:
                raise ValueError('Unexpected archive inventory')
            for item in entries:
                mode = item.external_attr >> 16
                if stat.S_ISLNK(mode) or item.is_dir() or not 0 < item.file_size <= MAX_MEMBER:
                    raise ValueError('Archive member must be a bounded regular file')
            return {item.filename: archive.read(item) for item in entries}
    with tarfile.open(path) as archive:
        entries = []
        for item in archive:
            entries.append(item)
            if len(entries) > len(expected):
                raise ValueError('Unexpected archive inventory')
        if len(entries) != len(expected) or {item.name for item in entries} != expected:
            raise ValueError('Unexpected archive inventory')
        for item in entries:
            if not item.isfile() or not 0 < item.size <= MAX_MEMBER:
                raise ValueError('Archive member must be a bounded regular file')
        return {item.name: archive.extractfile(item).read(MAX_MEMBER + 1) for item in entries}


def manifest(directory, expected_sha, source):
    data = bounded(directory / 'release-manifest.json', 64 << 10)
    if digest(data) != expected_sha:
        raise ValueError('Release manifest checksum mismatch')
    value = json.loads(data)
    if not isinstance(value, dict):
        raise ValueError('Release manifest must be an object')
    identity(value.get('version', ''), source)
    if (value.get('schema') != 2 or value.get('source_repository') != REPOSITORY
            or value.get('source_commit') != source or value.get('go_version') != GO_VERSION
            or not isinstance(value.get('artifacts'), dict) or set(value['artifacts']) != set(TARGETS)):
        raise ValueError('Release source/platform/toolchain identity mismatch')
    return value


def verify_archive(directory, metadata, target):
    entry = metadata['artifacts'][target]
    name = archive_name(metadata['version'], target)
    if not isinstance(entry, dict) or entry.get('name') != name:
        raise ValueError('Archive name differs from version/platform')
    path = directory / name
    data = bounded(path, MAX_ARCHIVE)
    if type(entry.get('size_bytes')) is not int or entry['size_bytes'] != len(data) or entry.get('sha256') != digest(data):
        raise ValueError('Archive checksum or size differs')
    payload = members(path, target)
    inventory = {name: {'sha256': digest(data), 'size_bytes': len(data)} for name, data in payload.items()}
    if entry.get('files') != inventory:
        raise ValueError('Archive payload differs from source-bound inventory')
    if (payload['VERSION'].decode().strip() != metadata['version']
            or payload['SOURCE_COMMIT'].decode().strip() != metadata['source_commit']):
        raise ValueError('Archive source/version differs')
    return payload
