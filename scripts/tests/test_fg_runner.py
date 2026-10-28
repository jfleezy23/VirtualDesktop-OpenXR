import importlib.util
from pathlib import Path
import unittest
import tempfile
import uuid
import winreg
from vdxr_game_profiles import ProfileStore


class ResultTests(unittest.TestCase):
    def test_profile_fixture_guard_cleans_after_child_failure_and_preserves_existing(self):
        runner = self.runner()
        base = r"SOFTWARE\VDXR-Test-" + uuid.uuid4().hex
        store = ProfileStore(winreg.HKEY_CURRENT_USER, base)
        executable = r"C:\Fixture\Game.exe"
        try:
            with self.assertRaisesRegex(RuntimeError, "child failed"):
                with runner.profile_fixture_guard(executable, "bgra-profile-4", store):
                    store.save(executable, {"FrameGenerationMode": 4})
                    raise RuntimeError("child failed")
            self.assertIsNone(store._read(store.path(executable), "ExecutablePath"))
            store.save(executable, {"FrameGenerationMode": 3})
            with self.assertRaisesRegex(ValueError, "already exists"):
                with runner.profile_fixture_guard(executable, "bgra-profile-4", store):
                    self.fail("existing profile must prevent fixture launch")
            self.assertEqual(store.load(executable)[0]["FrameGenerationMode"], 3)
            store.reset(executable)
        finally:
            for key in (store.path(executable), base + r"\Profiles", base):
                try:
                    winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, key, winreg.KEY_WOW64_64KEY)
                except FileNotFoundError:
                    pass

    def runner(self):
        path = Path(__file__).parents[2] / "tests/run-fg-regressions.py"
        spec = importlib.util.spec_from_file_location("fg_clock_runner", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_spatial_profiles_pin_cold30_and_steady120_despite_inherited_clock(self):
        runner = self.runner()
        cold = runner.select_clock("spatial-combined-bgra-3", inherited="120")
        steady = runner.select_clock("spatial-steady-90-80-bgra-srgb-3", inherited="30")
        self.assertEqual(cold["selected_hz"], 30)
        self.assertEqual(cold["profile"], "cold-transition")
        self.assertEqual(cold["inherited_env"], "120")
        self.assertEqual(steady["selected_hz"], 120)
        self.assertEqual(steady["profile"], "steady-fixed")
        self.assertEqual(steady["inherited_env"], "30")
        self.assertEqual(steady["selection"], "profile")
        self.assertEqual(runner.select_clock("native")["selected_hz"], 120)

    def test_explicit_clock_override_is_recorded_separately_from_intended_profile(self):
        runner = self.runner()
        clock = runner.select_clock("spatial-steady-95-80-bgra-2", inherited="30", override=60)
        self.assertEqual(clock["profile_hz"], 120)
        self.assertEqual(clock["selected_hz"], 60)
        self.assertEqual(clock["selection"], "cli-override")
        self.assertEqual(clock["inherited_env"], "30")
        with self.assertRaises(ValueError):
            runner.select_clock("spatial-combined-bgra-2", override=90)

    def test_explicit_144hz_clock_is_selected_and_verified_for_all_multipliers(self):
        runner = self.runner()
        for multiplier in (2, 3, 4):
            selected = runner.select_clock(f"spatial-steady-90-85-bgra-{multiplier}", inherited="120", override=144)
            self.assertEqual(selected["selected_hz"], 144)
            self.assertEqual(selected["profile_hz"], 120)
            self.assertEqual(selected["selection"], "cli-override")
            self.assertFalse(runner.clock_receipt(selected, "private backend test clock=120Hz\n")["matched"])
            self.assertTrue(runner.clock_receipt(selected, "private backend test clock=144Hz\n")["matched"])

    def test_clock_receipt_rejects_missing_duplicate_and_mismatching_actual_rate(self):
        runner = self.runner()
        selected = runner.select_clock("spatial-steady-90-85-bgra-3", inherited="30")
        for output in ("PASS: pixels\n", "private backend test clock=30Hz\nPASS: pixels\n",
                       "private backend test clock=120Hz\nprivate backend test clock=120Hz\nPASS: pixels\n"):
            self.assertFalse(runner.clock_receipt(selected, output)["matched"])
        receipt = runner.clock_receipt(selected, "private backend test clock=120Hz\nPASS: pixels\n")
        self.assertTrue(receipt["matched"])
        self.assertEqual(receipt["reported_hz"], 120)
        self.assertEqual(receipt["inherited_env"], "30")

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
