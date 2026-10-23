"""Check the narrow CI transition without requiring a fake published CLI pin."""
import json
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

    def test_pending_public_input_cannot_be_mistaken_for_lock(self):
        pending = json.loads((ROOT / 'tooling/dockpipe-bootstrap-pending.json').read_text())
        self.assertEqual(pending['state'], 'awaiting-public-core-cli')
        self.assertIs(pending['installable'], False)
        self.assertEqual(len(pending['required_platforms']), 5)
        self.assertNotIn('artifacts', pending)
        self.assertNotIn('dockpipe', pending)
        config = json.loads((ROOT / 'dockpipe.config.json').read_text())
        lock = json.loads((ROOT / 'dockpipe.lock.json').read_text())
        self.assertEqual(config['dockpipe_version'], lock['dockpipe']['version'])

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
