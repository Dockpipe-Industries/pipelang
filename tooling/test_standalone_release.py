"""Exercise archive admission and exact no-replacement publication contracts offline."""
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'workflows/release/assets/scripts'
sys.path.insert(0, str(SCRIPTS))
import release_contract as contract
import publish
import qualify

SOURCE = 'a' * 40
VERSION = '0.1.1-staging.1'


def fixture(directory, *, native_binary=None):
    records = {}
    for target in contract.TARGETS:
        payload = {command + ('.exe' if target.startswith('windows/') else ''): b'fixture-' + command.encode()
                   for command in contract.COMMANDS}
        if target == 'linux/amd64' and native_binary:
            payload['pipelang'] = native_binary.read_bytes()
        payload.update(LICENSE=b'license', README=b'readme')
        payload['README.md'] = payload.pop('README')
        payload.update(VERSION=(VERSION + '\n').encode(), SOURCE_COMMIT=(SOURCE + '\n').encode())
        name = contract.archive_name(VERSION, target)
        path = directory / name
        if target.startswith('windows/'):
            with zipfile.ZipFile(path, 'w') as archive:
                for member, data in payload.items():
                    archive.writestr(member, data)
        else:
            with tarfile.open(path, 'w:gz') as archive:
                for member, data in payload.items():
                    entry = tarfile.TarInfo(member)
                    entry.size = len(data)
                    archive.addfile(entry, io.BytesIO(data))
        records[target] = {'name': name, 'size_bytes': path.stat().st_size,
                           'sha256': contract.digest(path.read_bytes()),
                           'files': {member: {'size_bytes': len(data), 'sha256': contract.digest(data)}
                                     for member, data in payload.items()}}
    metadata = {'schema': 2, 'version': VERSION, 'source_repository': contract.REPOSITORY,
                'source_commit': SOURCE, 'go_version': contract.GO_VERSION, 'artifacts': records}
    manifest = directory / 'release-manifest.json'
    manifest.write_text(json.dumps(metadata))
    manifest_sha = contract.digest(manifest.read_bytes())
    (directory / 'SHA256SUMS.txt').write_text(''.join(entry['sha256'] + '  ' + entry['name'] + '\n'
                                                    for entry in records.values()))
    for target, entry in records.items():
        receipt = {'schema': 1, 'source_commit': SOURCE, 'version': VERSION, 'platform': target,
                   'manifest_sha256': manifest_sha, 'archive_sha256': entry['sha256'],
                   'fixture_sha256': contract.digest(qualify.FIXTURE.encode()), 'checks': contract.CHECKS,
                   'scope': 'native-standalone-acceptance-not-full-conformance'}
        (directory / ('native-' + target.replace('/', '-') + '.json')).write_text(json.dumps(receipt))
    return metadata, manifest_sha


class StandaloneReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.metadata, self.manifest_sha = fixture(self.directory)

    def test_complete_source_bound_release(self):
        metadata, assets = publish.validate(self.directory, self.manifest_sha, SOURCE)
        self.assertEqual(metadata['version'], VERSION)
        self.assertEqual(len(assets), 12)

    def test_old_stable_version_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'fresh'):
            contract.identity('0.1.0', SOURCE)

    def test_manifest_and_source_substitution_are_rejected(self):
        for digest, source in [('0' * 64, SOURCE), (self.manifest_sha, 'b' * 40)]:
            with self.subTest(source=source, digest=digest):
                with self.assertRaises(ValueError):
                    publish.validate(self.directory, digest, source)

    def test_modified_archive_is_rejected(self):
        entry = self.metadata['artifacts']['linux/amd64']
        (self.directory / entry['name']).write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'checksum or size'):
            publish.validate(self.directory, self.manifest_sha, SOURCE)

    def test_missing_native_platform_is_rejected(self):
        (self.directory / 'native-darwin-arm64.json').unlink()
        with self.assertRaises(ValueError):
            publish.validate(self.directory, self.manifest_sha, SOURCE)

    def test_native_receipt_from_different_archive_is_rejected(self):
        path = self.directory / 'native-windows-amd64.json'
        receipt = json.loads(path.read_text())
        receipt['archive_sha256'] = 'b' * 64
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'qualification differs'):
            publish.validate(self.directory, self.manifest_sha, SOURCE)

    def test_native_platform_mismatch_rejected_before_execution(self):
        with patch.object(qualify, 'native_target', return_value='windows/amd64'):
            with patch.object(qualify.subprocess, 'run') as execute:
                with self.assertRaisesRegex(ValueError, 'exact native'):
                    qualify.qualify(self.directory, self.manifest_sha, SOURCE, 'linux/amd64', self.directory / 'out.json')
                execute.assert_not_called()

    def test_tar_link_and_zip_duplicate_rejected(self):
        path = self.directory / 'bad.tar.gz'
        with tarfile.open(path, 'w:gz') as archive:
            for name in list(contract.COMMANDS) + ['LICENSE', 'README.md', 'VERSION', 'SOURCE_COMMIT']:
                entry = tarfile.TarInfo(name)
                entry.type = tarfile.SYMTYPE
                entry.linkname = '/outside'
                archive.addfile(entry)
        with self.assertRaisesRegex(ValueError, 'bounded regular'):
            contract.members(path, 'linux/amd64')
        path = self.directory / 'bad.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('pipelang.exe', b'a')
            archive.writestr('other', b'a')
        with self.assertRaisesRegex(ValueError, 'inventory'):
            contract.members(path, 'windows/amd64')

    def release(self, assets, draft):
        return {'id': 3, 'tag_name': 'v' + VERSION, 'target_commitish': SOURCE, 'prerelease': True,
                'draft': draft, 'html_url': 'https://github.com/' + contract.REPOSITORY + '/releases/tag/v' + VERSION,
                'assets': [{'name': name, **entry} for name, entry in assets.items()]}

    def test_existing_asset_collision_is_never_replaced(self):
        _, assets = publish.validate(self.directory, self.manifest_sha, SOURCE)
        release = self.release(assets, False)
        release['assets'][0]['digest'] = 'sha256:' + 'b' * 64
        with self.assertRaisesRegex(ValueError, 'never replaced'):
            publish.verify_release(release, 'v' + VERSION, SOURCE, assets)

    def publication(self, *, existing, event='push', corrupt_readback=False, deny_attestation=False):
        _, assets = publish.validate(self.directory, self.manifest_sha, SOURCE)
        commands = []
        release = self.release(assets, True if existing is None else existing)
        final = self.release(assets, False)

        def run(arguments):
            commands.append(arguments)
            if deny_attestation and arguments[:3] == ['gh', 'attestation', 'verify']:
                raise ValueError('Attestation rejected')
            return json.dumps(final) if arguments[:2] == ['gh', 'api'] else ''

        def readback(url, maximum):
            if corrupt_readback:
                return b'corrupted'
            return (self.directory / url.rsplit('/', 1)[1]).read_bytes()

        environment = {'GITHUB_SHA': SOURCE, 'GITHUB_REPOSITORY': contract.REPOSITORY,
                       'GITHUB_REF': 'refs/tags/v' + VERSION, 'GITHUB_EVENT_NAME': event}
        with patch.dict(os.environ, environment), patch.object(publish, 'verify_tag') as tag:
            with patch.object(publish, 'find_release', side_effect=[None, release] if existing is None else [release]):
                with patch.object(publish, 'run', side_effect=run), patch.object(publish, 'public_bytes', side_effect=readback):
                    publish.publish(self.directory, self.manifest_sha)
                    self.assertGreaterEqual(tag.call_count, 3)
        return commands

    def test_create_has_explicit_nonlatest_and_source_bound_attestations(self):
        commands = self.publication(existing=None)
        create = next(command for command in commands if command[:3] == ['gh', 'release', 'create'])
        self.assertIn('--latest=false', create)
        self.assertIn('--verify-tag', create)
        attestations = [command for command in commands if command[:3] == ['gh', 'attestation', 'verify']]
        self.assertEqual(len(attestations), 12)
        for command in attestations:
            self.assertEqual(command[command.index('--source-digest') + 1], SOURCE)
            self.assertIn('--source-ref', command)

    def test_resume_existing_published_release_does_not_edit_or_upload(self):
        commands = self.publication(existing=False)
        self.assertFalse(any(command[:2] == ['gh', 'release'] for command in commands))

    def test_resume_exact_draft_publishes_without_asset_replacement(self):
        commands = self.publication(existing=True)
        mutations = [command for command in commands if command[:2] == ['gh', 'release']]
        self.assertEqual(len(mutations), 1)
        self.assertEqual(mutations[0][2], 'edit')
        self.assertIn('--latest=false', mutations[0])

    def test_anonymous_readback_rejects_changed_bytes(self):
        with self.assertRaisesRegex(ValueError, 'readback differs'):
            self.publication(existing=False, corrupt_readback=True)

    def test_failed_attestation_stops_publication(self):
        with self.assertRaisesRegex(ValueError, 'Attestation rejected'):
            self.publication(existing=None, deny_attestation=True)

    def test_manual_dispatch_cannot_publish(self):
        with self.assertRaisesRegex(ValueError, 'source-tag push'):
            self.publication(existing=False, event='workflow_dispatch')


if __name__ == '__main__':
    unittest.main()
