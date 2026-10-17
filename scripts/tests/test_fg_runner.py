import importlib.util
from pathlib import Path
import unittest
import tempfile


class ResultTests(unittest.TestCase):
    def test_module_receipt_rejects_changed_packed_input_fixture(self):
        path = Path(__file__).parents[1] / "fg-build-provenance.py"
        spec = importlib.util.spec_from_file_location("fg_provenance_pack", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            source = repo / "tests/nr_fg_pack_regression.cpp"
            source.parent.mkdir()
            source.write_text("original native pack fixture")
            artifact = repo / "pack.exe"
            artifact.write_bytes(b"built fixture")
            receipt = repo / "receipt.json"
            before = module.snapshot(repo, "module")
            module.write_receipt(receipt, repo, "module", before, [artifact])
            module.verify_receipt(receipt, repo, "module")
            source.write_text("changed capture readback")
            with self.assertRaises(ValueError):
                module.verify_receipt(receipt, repo, "module")

    def test_build_receipt_rejects_stale_source_and_artifacts(self):
        path = Path(__file__).parents[1] / "fg-build-provenance.py"
        spec = importlib.util.spec_from_file_location("fg_provenance", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            source = repo / "virtualdesktop-openxr/test.h"
            source.parent.mkdir()
            source.write_text("original")
            artifact = repo / "test.dll"
            artifact.write_bytes(b"binary")
            receipt = repo / "receipt.json"
            before = module.snapshot(repo, "runtime")
            module.write_receipt(receipt, repo, "runtime", before, [artifact])
            module.verify_receipt(receipt, repo, "runtime")
            source.write_text("changed header")
            with self.assertRaises(ValueError):
                module.verify_receipt(receipt, repo, "runtime")
            with self.assertRaises(ValueError):
                module.write_receipt(receipt, repo, "runtime", before, [artifact])
            source.write_text("original")
            artifact.write_bytes(b"different binary")
            with self.assertRaises(ValueError):
                module.verify_receipt(receipt, repo, "runtime")

    def test_requires_success_and_a_positive_result(self):
        path = Path(__file__).parents[2] / "tests/run-fg-regressions.py"
        spec = importlib.util.spec_from_file_location("fg_runner", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for code, output in [(0, ""), (1, "PASS: test"), (0, "PASS: test\nFAIL: other"),
                             (0, "PASS: test\nASSERTION FAILED: x"), (0, "PASS: test\nERROR: x")]:
            self.assertFalse(module.successful(code, output))
        self.assertTrue(module.successful(0, "NVIDIA adapter: hardware\nPASS: actual pixels\n"))


if __name__ == "__main__":
    unittest.main()
