"""Negative artifact checks for the documentation actually shipped."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('package_docs', Path(__file__).with_name('package-docs.py'))
docs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(docs)


class PackageDocumentationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve() / 'package'
        self.root.mkdir()
        (self.root / 'docs').mkdir()

    def check(self):
        with contextlib.redirect_stdout(io.StringIO()):
            docs.check(self.root)

    def test_valid_nested_reference(self):
        (self.root / 'docs/spec.md').write_text('spec', encoding='utf-8')
        (self.root / 'README.md').write_text('[spec](docs/spec.md#section)', encoding='utf-8')
        self.check()

    def test_missing_deep_reference_fails(self):
        (self.root / 'docs/spec.md').write_text('[missing](missing-profile.md)', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'unresolved packaged link'):
            self.check()

    def test_existing_file_outside_package_fails(self):
        (self.root.parent / 'private.md').write_text('outside', encoding='utf-8')
        (self.root / 'README.md').write_text('[outside](../private.md)', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.check()

    def test_reference_style_missing_target_fails(self):
        (self.root / 'README.md').write_text('[spec][norm]\n\n[norm]: absent.md', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.check()

    def test_external_evidence_and_literal_example(self):
        (self.root / 'README.md').write_text(
            '[evidence](https://example.org/raw)\n```md\n[example](not-a-link.md)\n```\n', encoding='utf-8')
        self.check()

    def test_source_tree_inline_reference_fails(self):
        (self.root / 'README.md').write_text('`../mcl-core/spec/core.md`', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.check()

    def test_missing_package_fails(self):
        with self.assertRaises(ValueError):
            docs.check(self.root / 'absent')


if __name__ == '__main__':
    unittest.main()
