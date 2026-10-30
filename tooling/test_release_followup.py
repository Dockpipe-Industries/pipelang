"""Check the narrow CI transition without requiring a fake published CLI pin."""
import json
import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseFollowupTests(unittest.TestCase):
    def test_all_go_ci_jobs_use_exact_toolchain(self):
        for name, expected_count in [('ci.yml', 2), ('codeql.yml', 1), ('release.yml', 1)]:
            with self.subTest(workflow=name):
                text = (ROOT / '.github/workflows' / name).read_text()
                self.assertEqual(text.count("go-version: '1.26.10'"), expected_count)
                self.assertNotIn('1.26.9', text)

    def test_windows_invokes_owned_python_script_without_bash(self):
        text = (ROOT / 'workflows/ci.windows/config.yml').read_text()
        self.assertIn('run: assets/scripts/test.py', text)
        self.assertNotIn('test.sh', text)

    def test_byte_frozen_compatibility_inputs_have_lf_checkout_rules(self):
        rules = (ROOT / '.gitattributes').read_text().splitlines()
        inventory = 'docs/agents/tasks/fixtures/pipelang-vnext/legacy-v0.0.0.1-inventory.txt'
        self.assertIn(inventory + ' text eol=lf', rules)
        self.assertIn('tests/pipelangcompat/fixtures/** text eol=lf', rules)
        self.assertNotIn(b'\r', (ROOT / inventory).read_bytes())
        for path in (ROOT / 'tests/pipelangcompat/fixtures').rglob('*'):
            if path.is_file():
                with self.subTest(path=path.relative_to(ROOT)):
                    self.assertNotIn(b'\r', path.read_bytes())

    def test_public_bootstrap_uses_the_exact_lock_without_obsolete_pending_marker(self):
        self.assertFalse((ROOT / 'tooling/dockpipe-bootstrap-pending.json').exists())
        config = json.loads((ROOT / 'dockpipe.config.json').read_text())
        lock = json.loads((ROOT / 'dockpipe.lock.json').read_text())
        self.assertEqual(config['dockpipe_version'], lock['dockpipe']['version'])
        self.assertEqual(len(lock['dockpipe']['artifacts']), 5)

    def test_remote_actions_use_full_commit_shas(self):
        for workflow in sorted((ROOT / '.github/workflows').glob('*.yml')):
            for line_number, line in enumerate(workflow.read_text().splitlines(), 1):
                reference = re.search(r"\buses:\s*([^\s#]+)", line)
                if reference is None:
                    continue
                action = reference.group(1)
                if action.startswith('./') or action.startswith('docker://'):
                    continue
                with self.subTest(workflow=workflow.name, line=line_number):
                    self.assertRegex(action, r'^[^@]+@[0-9a-f]{40}$')

    def test_old_tag_cannot_be_published_again_by_replacement(self):
        workflow = (ROOT / 'workflows/release/assets/scripts/publish.py').read_text()
        release = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertIn('"v$(cat VERSION)"', release)
        self.assertIn("'gh', 'release', 'create'", workflow)
        self.assertIn("'--verify-tag'", workflow)
        self.assertIn("'--draft'", workflow)
        self.assertNotIn('--clobber', workflow)
        self.assertNotIn('gh release upload', workflow)


if __name__ == '__main__':
    unittest.main()
