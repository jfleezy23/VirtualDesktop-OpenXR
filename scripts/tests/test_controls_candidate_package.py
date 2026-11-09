"""Exercise candidate packaging without launching Tk, an EXE, or registry writes."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


class CandidatePackageTests(unittest.TestCase):
    def builder(self):
        path = Path(__file__).resolve().parents[1] / "build-controls.py"
        spec = importlib.util.spec_from_file_location("candidate_controls_builder", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_uncommitted_sources_record_base_only_and_freeze_all_candidate_inputs(self):
        builder = self.builder()
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "build"

            def compile_boundary(command, **_kwargs):
                # Emulate only PyInstaller's external artifact boundary. The
                # real builder must snapshot, copy, hash and archive the inputs.
                self.assertIn("PyInstaller", command)
                exe = output / "dist" / builder.NAME / (builder.NAME + ".exe")
                exe.parent.mkdir(parents=True)
                exe.write_bytes(b"packaging boundary fixture")

            with mock.patch.object(builder.importlib.metadata, "version", return_value="6.22.3"), \
                 mock.patch.object(builder.subprocess, "run", side_effect=compile_boundary), \
                 mock.patch.object(builder.subprocess, "check_output", return_value="c" * 40 + "\n") as git:
                builder.build(output)

            bundle = output / "dist" / builder.NAME
            metadata = json.loads((bundle / "BUILD.json").read_text())
            self.assertIsNone(metadata["source_revision"], "uncommitted input must not invent a release commit")
            self.assertNotIn("build_base_repository", metadata, "public metadata must not contain local checkout paths")
            self.assertEqual(metadata["build_base_revision"], "c" * 40)
            self.assertEqual(Path(git.call_args.kwargs["cwd"]), builder.ROOT)
            for name in ("scripts/tests/test_depth_controls.py", "scripts/tests/test_controls_candidate_package.py",
                         "scripts/tests/test_controls_self_test_depth.py", "docs/DEPTH-MODES.md",
                         "virtualdesktop-openxr/game_profiles.h"):
                self.assertIn(name, metadata["source_sha256"])
                self.assertEqual(builder.sha(bundle / "source" / name), metadata["source_sha256"][name])
            self.assertEqual((bundle / "DEPTH-MODES.md").read_bytes(), (builder.ROOT / "docs/DEPTH-MODES.md").read_bytes())
            self.assertIn("[reconstruction mode guide](DEPTH-MODES.md)", (bundle / "README.md").read_text())
            builder.verify_recorded_payload(output, bundle)

    def test_payload_blocks_models_and_all_non_allowlisted_runtime_dlls(self):
        builder = self.builder()
        for name in ("vdxr-openxr.dll", "vdxr-frame-generation.dll", "nvngx_dlssnr.dll", "payload.safetensors", "payload.gguf"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                file = Path(folder) / "nested" / name
                file.parent.mkdir()
                file.write_bytes(b"forbidden fixture")
                with self.assertRaises(ValueError):
                    builder.verify_payload(Path(folder))


if __name__ == "__main__":
    unittest.main()
