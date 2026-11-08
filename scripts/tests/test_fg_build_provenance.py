"""Receipt coverage for source hooks and fixture build arguments."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("fg_build_provenance", ROOT / "scripts/fg-build-provenance.py")
PROVENANCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROVENANCE)


class FixtureProvenanceTests(unittest.TestCase):
    def test_hooks_and_builder_changes_invalidate_fixture_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / "tests").mkdir()
            hook = repo / "tests/overlap_access.inc"
            builder = repo / "tests/build-fg-runtime-fixtures.py"
            artifact = repo / "fixture.exe"
            hook.write_text("int observed = 1;\n", encoding="utf-8")
            builder.write_text("flags = ['original']\n", encoding="utf-8")
            artifact.write_bytes(b"test artifact")
            receipt = repo / "receipt.json"
            original = PROVENANCE.snapshot(repo, "fixture")
            PROVENANCE.write_receipt(receipt, repo, "fixture", original, [artifact])
            PROVENANCE.verify_receipt(receipt, repo, "fixture")
            for path in (hook, builder):
                with self.subTest(path=path.name):
                    before = path.read_bytes()
                    path.write_bytes(before + b"\n// changed\n")
                    with self.assertRaisesRegex(ValueError, "stale"):
                        PROVENANCE.verify_receipt(receipt, repo, "fixture")
                    path.write_bytes(before)
                    PROVENANCE.verify_receipt(receipt, repo, "fixture")


if __name__ == "__main__":
    unittest.main()
