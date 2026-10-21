"""Reject unwanted DLLs at the standalone controls packaging boundary."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


class ControlsPackageTests(unittest.TestCase):
    def validator(self):
        source = Path(__file__).resolve().parents[1] / "build-controls.py"
        self.assertTrue(source.is_file(), "Missing standalone packaging boundary")
        spec = importlib.util.spec_from_file_location("build_controls", source)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.verify_payload

    def test_known_python_runtime_is_allowed(self):
        verify = self.validator()
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "python313.dll").write_bytes(b"fixture")
            verify(Path(folder))

    def test_graphics_or_nr_dll_is_rejected_even_when_nested(self):
        verify = self.validator()
        for name in ("nvngx_dlssnr.dll", "NVNGX_DLSSNR.DLL", "vdxr-frame-generation.dll", "unknown.dll"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                nested = Path(folder) / "_internal"
                nested.mkdir()
                (nested / name).write_bytes(b"fixture")
                with self.assertRaises(ValueError):
                    verify(Path(folder))

    def test_model_data_is_rejected(self):
        verify = self.validator()
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "network.onnx").write_bytes(b"fixture")
            with self.assertRaises(ValueError):
                verify(Path(folder))

    def test_changed_added_or_missing_runtime_prevents_final_binding(self):
        source = Path(__file__).resolve().parents[1] / "build-controls.py"
        spec = importlib.util.spec_from_file_location("controls_integrity", source)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertTrue(callable(getattr(module, "verify_recorded_payload", None)), "Missing tested-payload verification")
        for mutation in ("changed", "added", "missing"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as folder:
                output = Path(folder)
                bundle = output / "bundle"
                bundle.mkdir()
                runtime = bundle / "python313.dll"
                runtime.write_bytes(b"original")
                (output / "payload-sha256.json").write_text(json.dumps({"python313.dll": module.sha(runtime)}))
                module.verify_recorded_payload(output, bundle)
                if mutation == "changed":
                    runtime.write_bytes(b"changed")
                elif mutation == "added":
                    (bundle / "extra.txt").write_text("extra")
                else:
                    runtime.unlink()
                with self.assertRaises(ValueError):
                    module.verify_recorded_payload(output, bundle)
