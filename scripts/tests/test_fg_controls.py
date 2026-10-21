"""Exercise the real NR window callbacks against an isolated registry boundary."""

import importlib.util
from pathlib import Path
import tkinter as tk
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[2] / "vdxr_registry_gui.py"
SPEC = importlib.util.spec_from_file_location("nr_controls", PATH)
gui = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gui)


class Key:
    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass


class Registry:
    HKEY_LOCAL_MACHINE = 1
    KEY_READ = 2
    KEY_SET_VALUE = 4
    KEY_WOW64_64KEY = 256
    REG_DWORD = 4
    REG_SZ = 1

    def __init__(self):
        self.values = {"FoveationSize": (100, self.REG_DWORD), "DLSSNR_Enabled": (1, self.REG_DWORD)}
        self.writes = []
        self.access = []
        self.paths = []
        self.denied = False

    def OpenKey(self, hive, path, reserved=0, access=2):
        self.paths.append(path)
        self.access.append(("read", access))
        return Key()

    def CreateKeyEx(self, hive, path, reserved=0, access=4):
        self.paths.append(path)
        if self.denied:
            raise PermissionError("fixture permission denial")
        self.access.append(("write", access))
        return Key()

    def QueryValueEx(self, key, name):
        if name not in self.values:
            raise FileNotFoundError(name)
        return self.values[name]

    def SetValueEx(self, key, name, reserved, kind, value):
        self.writes.append((name, value, kind))
        self.values[name] = (value, kind)


class FrameGenerationControlsTests(unittest.TestCase):
    def setUp(self):
        self.registry = Registry()
        self.patch = mock.patch.object(gui, "winreg", self.registry)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)

    def app(self):
        return gui.VDXRRegistryApp(self.root)

    def select(self, app, label):
        app.frame_generation.set(label)
        app.on_frame_generation_changed()

    def test_startup_off_is_read_only(self):
        app = self.app()
        self.assertEqual(app.frame_generation.get(), "Off")
        self.assertEqual(self.registry.writes, [])

    def test_controls_target_virtual_desktop_bundled_runtime(self):
        app = self.app()
        self.select(app, "OFXR 3×")
        self.assertTrue(self.registry.paths)
        self.assertEqual(set(self.registry.paths), {r"SOFTWARE\Virtual Desktop, Inc.\OpenXR"})

    def test_nr_and_fg_use_same_64bit_registry_view(self):
        app = self.app()
        gui.write_value("FoveationSize", 100)
        self.select(app, "OFXR 3×")
        self.assertTrue(self.registry.access)
        self.assertTrue(all(access & 256 for _, access in self.registry.access))

    def test_saving_three_modes_preserves_nr_settings_and_uses_64bit_view(self):
        app = self.app()
        for label, value in [("OFXR 2×", 2), ("OFXR 3×", 3), ("Off", 0)]:
            self.select(app, label)
            self.assertEqual(self.registry.values["FrameGenerationMode"], (value, 4))
            self.assertIn("restart", app.status.get().lower())
        self.assertEqual(self.registry.values["FoveationSize"], (100, 4))
        self.assertEqual(self.registry.values["DLSSNR_Enabled"], (1, 4))
        self.assertEqual([name for name, _, _ in self.registry.writes], ["FrameGenerationMode"] * 3)
        self.assertTrue(all(access & 256 for operation, access in self.registry.access if operation == "write"))

    def test_invalid_stored_type_or_mode_is_reported_without_writing(self):
        for stored in [(4, 4), ("3", 1)]:
            with self.subTest(stored=stored):
                self.registry.values["FrameGenerationMode"] = stored
                with self.assertRaises(ValueError):
                    gui.read_frame_generation_mode()
        app = self.app()
        self.assertIn("invalid", app.status.get().lower())
        self.assertEqual(self.registry.writes, [])

    def test_permission_failure_restores_previous_selection(self):
        self.registry.values["FrameGenerationMode"] = (2, 4)
        app = self.app()
        self.registry.denied = True
        self.select(app, "OFXR 3×")
        self.assertEqual(app.frame_generation.get(), "OFXR 2×")
        self.assertIn("administrator", app.status.get().lower())
        self.assertEqual(self.registry.values["FrameGenerationMode"], (2, 4))

    def test_readback_failure_is_not_reported_as_saved(self):
        app = self.app()
        with mock.patch.object(self.registry, "SetValueEx", side_effect=lambda *_: None):
            self.select(app, "OFXR 3×")
        self.assertEqual(app.frame_generation.get(), "Off")
        self.assertIn("error", app.status.get().lower())

    def test_nr_reset_does_not_reset_fg_mode(self):
        self.registry.values["FrameGenerationMode"] = (3, 4)
        app = self.app()
        app.reset_defaults()
        self.assertEqual(self.registry.values["FrameGenerationMode"], (3, 4))
        self.assertEqual(app.frame_generation.get(), "OFXR 3×")
        self.assertNotIn("FrameGenerationMode", [name for name, _, _ in self.registry.writes])

    def test_backend_defaults_to_nvidia_without_writing(self):
        app = self.app()
        self.assertEqual(app.frame_generation_backend.get(), "NVIDIA (default)")
        self.assertEqual(self.registry.writes, [])

    def test_backend_selection_preserves_nr_and_fg_settings(self):
        self.registry.values["FrameGenerationMode"] = (3, 4)
        app = self.app()
        app.frame_generation_backend.set("FidelityFX (full resolution)")
        app.on_frame_generation_backend_changed()
        self.assertEqual(self.registry.values["FrameGenerationBackend"], (1, 4))
        self.assertEqual(self.registry.values["FrameGenerationMode"], (3, 4))
        self.assertEqual(self.registry.values["FoveationSize"], (100, 4))
        self.assertEqual(self.registry.values["DLSSNR_Enabled"], (1, 4))
        self.assertIn("restart", app.status.get().lower())
        self.assertEqual([name for name, _, _ in self.registry.writes], ["FrameGenerationBackend"])

    def test_backend_permission_error_restores_saved_choice(self):
        app = self.app()
        self.registry.denied = True
        app.frame_generation_backend.set("FidelityFX (full resolution)")
        app.on_frame_generation_backend_changed()
        self.assertEqual(app.frame_generation_backend.get(), "NVIDIA (default)")
        self.assertIn("administrator", app.status.get().lower())

    def test_backend_readback_failure_is_not_reported_saved(self):
        app = self.app()
        with mock.patch.object(self.registry, "SetValueEx", side_effect=lambda *_: None):
            app.frame_generation_backend.set("FidelityFX (full resolution)")
            app.on_frame_generation_backend_changed()
        self.assertEqual(app.frame_generation_backend.get(), "NVIDIA (default)")
        self.assertIn("error", app.status.get().lower())

    def test_invalid_saved_backend_is_reported_without_writing(self):
        for value in [(2, 4), ("1", 1)]:
            self.registry.values["FrameGenerationBackend"] = value
            with self.assertRaises(ValueError):
                gui.read_frame_generation_backend()
        app = self.app()
        self.assertIn("invalid", app.status.get().lower())
        self.assertEqual(self.registry.writes, [])

    def test_nr_reset_leaves_backend_selected(self):
        self.registry.values["FrameGenerationBackend"] = (1, 4)
        app = self.app()
        app.reset_defaults()
        self.assertEqual(self.registry.values["FrameGenerationBackend"], (1, 4))
        self.assertEqual(app.frame_generation_backend.get(), "FidelityFX (full resolution)")
        self.assertNotIn("FrameGenerationBackend", [name for name, _, _ in self.registry.writes])


if __name__ == "__main__":
    unittest.main()
