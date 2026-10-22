"""Reject unwanted DLLs at the standalone controls packaging boundary."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


class ControlsPackageTests(unittest.TestCase):
    def builder(self):
        source = Path(__file__).resolve().parents[1] / "build-controls.py"
        spec = importlib.util.spec_from_file_location("controls_documentation", source)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

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

    def test_packaged_guide_keeps_its_relative_images(self):
        module = self.builder()
        self.assertTrue(callable(getattr(module, "copy_documentation", None)), "Missing illustrated guide packaging")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            snapshot, bundle = root / "snapshot", root / "bundle"
            (snapshot / "docs/images").mkdir(parents=True)
            bundle.mkdir()
            guide = b"![Controls](images/controls-overview.png)\n![Backend](images/controls-fg-only.png)\n"
            (snapshot / "docs/NR-FG-CONTROLS.md").write_bytes(guide)
            images = {"controls-overview.png": b"overview\r\nbytes", "controls-fg-only.png": b"backend\r\nbytes"}
            for name, data in images.items():
                (snapshot / "docs/images" / name).write_bytes(data)
            module.copy_documentation(snapshot, bundle)
            self.assertEqual((bundle / "README.md").read_bytes(), guide)
            for name, data in images.items():
                self.assertEqual((bundle / "images" / name).read_bytes(), data)
                self.assertIn("docs/images/" + name, module.SOURCE_FILES)
            hashes = module.archive_bundle(bundle, root / "controls.zip")
            self.assertIn("images/controls-overview.png", hashes)
            self.assertIn("images/controls-fg-only.png", hashes)

    def test_revision_binding_compares_image_bytes_without_text_normalization(self):
        module = self.builder()
        image = "docs/images/controls-overview.png"
        module.SOURCE_FILES = (image,)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            bundle = output / "dist" / module.NAME
            source = bundle / "source" / image
            source.parent.mkdir(parents=True)
            source.write_bytes(b"image\r\npayload")
            executable = bundle / (module.NAME + ".exe")
            executable.write_bytes(b"fixture executable")
            metadata = {"exe_sha256": module.sha(executable), "source_sha256": {image: module.sha(source)}}
            (bundle / "BUILD.json").write_text(json.dumps(metadata))
            hashes = {p.relative_to(bundle).as_posix(): module.sha(p) for p in bundle.rglob("*") if p.is_file()}
            (output / "payload-sha256.json").write_text(json.dumps(hashes))
            # PNG and other binary assets must be byte-identical to the commit;
            # CRLF normalization is appropriate only for the text sources.
            with mock.patch.object(module.subprocess, "check_output", side_effect=["a" * 40, b"image\npayload"]):
                with self.assertRaises(ValueError):
                    module.bind_revision(output, "HEAD")
