"""Exercise the real NR window callbacks against an isolated registry boundary."""

import importlib.util
from pathlib import Path
import subprocess
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

    def test_missing_coverage_defaults_to_100_without_startup_writes(self):
        self.registry.values.clear()
        app = self.app()
        self.assertEqual(app.variables[-1].get(), 100)
        self.assertEqual(self.registry.writes, [])

    def test_already_enabled_missing_coverage_warns_without_startup_write(self):
        self.registry.values.pop("FoveationSize")
        app = self.app()
        self.assertTrue(app.enabled.get())
        self.assertEqual(app.variables[-1].get(), 100)
        self.assertIn("not saved", app.status.get().lower())
        self.assertEqual(self.registry.writes, [])

    def test_malformed_nr_values_open_with_defaults_and_warning(self):
        for stored in [("bad", 1), ("50", 1), (101, 4), (b"bad", 3)]:
            with self.subTest(stored=stored):
                self.registry.values["DLSSNR_Intensity"] = stored
                try:
                    app = self.app()
                except (ValueError, TypeError) as error:
                    self.fail(f"Malformed registry value crashed startup: {error}")
                self.assertEqual(app.variables[1].get(), 100)
                self.assertIn("invalid", app.status.get().lower())
                self.assertEqual(self.registry.writes, [])

    def test_malformed_enabled_does_not_crash_or_enable_nr(self):
        for stored in [("bad", 1), (2, 4), ("1", 1)]:
            self.registry.values["DLSSNR_Enabled"] = stored
            try:
                app = self.app()
            except (ValueError, TypeError) as error:
                self.fail(f"Malformed enable value crashed startup: {error}")
            self.assertFalse(app.enabled.get())
            self.assertIn("invalid", app.status.get().lower())
        self.assertEqual(self.registry.writes, [])

    def test_denied_read_still_opens_without_writing(self):
        with mock.patch.object(self.registry, "OpenKey", side_effect=PermissionError("fixture read denial")):
            try:
                app = self.app()
            except OSError as error:
                self.fail(f"Registry read denial crashed startup: {error}")
        self.assertIn("read", app.status.get().lower())
        self.assertEqual(self.registry.writes, [])

    def test_reset_persists_100_immediately_and_cancels_pending_coverage(self):
        app = self.app()
        app.variables[-1].set(92)
        self.assertIsNotNone(app.foveation_save_id)
        app.reset_defaults()
        self.assertEqual(self.registry.values["FoveationSize"], (100, 4))
        self.assertIsNone(app.foveation_save_id)
        self.assertEqual([self.registry.values[name][0] for name in gui.VALUE_NAMES], list(gui.VALUE_DEFAULTS))
        self.assertFalse(app.enabled.get())

    def test_failed_reset_does_not_show_unsaved_disabled_state(self):
        app = self.app()
        self.registry.denied = True
        app.reset_defaults()
        self.assertTrue(app.enabled.get())
        self.assertNotIn("reset to defaults", app.status.get().lower())

    def test_failed_nr_toggle_restores_verified_state(self):
        app = self.app()
        self.registry.denied = True
        app.enabled.set(False)
        app.on_enabled_changed()
        self.assertTrue(app.enabled.get())
        self.assertIn("administrator", app.status.get().lower())

    def test_failed_nr_slider_readback_is_not_reported_as_saved(self):
        app = self.app()
        with mock.patch.object(self.registry, "SetValueEx", side_effect=lambda *_: None):
            app.variables[1].set(42)
        self.assertEqual(app.variables[1].get(), 100)
        self.assertIn("error", app.status.get().lower())

    def test_enabling_nr_saves_displayed_coverage_before_enable(self):
        self.registry.values.clear()
        app = self.app()
        app.enabled.set(True)
        app.on_enabled_changed()
        writes = [(name, value) for name, value, _ in self.registry.writes]
        expected = [("FoveationSize", 100), *zip(gui.VALUE_NAMES[:-1], gui.VALUE_DEFAULTS[:-1]), ("DLSSNR_Enabled", 1)]
        self.assertEqual(writes, expected)

    def test_enabling_nr_normalizes_displayed_invalid_parameters(self):
        self.registry.values["DLSSNR_Enabled"] = (0, 4)
        self.registry.values["DLSSNR_Style"] = (3, 4)
        app = self.app()
        self.assertEqual(self.registry.writes, [])
        self.assertEqual(app.variables[0].get(), 0)
        app.enabled.set(True)
        app.on_enabled_changed()
        self.assertEqual(self.registry.values["DLSSNR_Style"], (0, 4))
        self.assertEqual(self.registry.values["DLSSNR_Enabled"], (1, 4))

    def test_failed_parameter_normalization_prevents_enable(self):
        self.registry.values["DLSSNR_Enabled"] = (0, 4)
        self.registry.values["DLSSNR_Style"] = (3, 4)
        app = self.app()
        original = self.registry.SetValueEx
        def fail_style(key, name, reserved, kind, value):
            if name == "DLSSNR_Style":
                raise PermissionError("fixture style denial")
            original(key, name, reserved, kind, value)
        with mock.patch.object(self.registry, "SetValueEx", side_effect=fail_style):
            app.enabled.set(True)
            app.on_enabled_changed()
        self.assertFalse(app.enabled.get())
        self.assertEqual(self.registry.values["DLSSNR_Enabled"], (0, 4))

    def test_failed_coverage_save_prevents_nr_enable(self):
        self.registry.values.clear()
        app = self.app()
        app.variables[-1].set(94)
        original = self.registry.SetValueEx
        def fail_coverage(key, name, reserved, kind, value):
            if name == "FoveationSize":
                raise PermissionError("fixture coverage denial")
            original(key, name, reserved, kind, value)
        with mock.patch.object(self.registry, "SetValueEx", side_effect=fail_coverage):
            app.enabled.set(True)
            app.on_enabled_changed()
        self.assertFalse(app.enabled.get())
        self.assertEqual(app.variables[-1].get(), 100)
        self.assertIsNone(app.foveation_save_id)
        self.assertNotIn("DLSSNR_Enabled", self.registry.values)

    def test_close_flushes_pending_coverage(self):
        app = self.app()
        app.variables[-1].set(94)
        self.assertTrue(callable(getattr(app, "on_close", None)), "Missing close-save handler")
        with mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
        self.assertEqual(self.registry.values["FoveationSize"], (94, 4))
        self.assertIsNone(app.foveation_save_id)
        destroy.assert_called_once()

    def test_close_stays_open_after_pending_save_failure(self):
        app = self.app()
        app.variables[-1].set(94)
        self.registry.denied = True
        self.assertTrue(callable(getattr(app, "on_close", None)), "Missing close-save handler")
        with mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
        destroy.assert_not_called()
        self.assertIn("administrator", app.status.get().lower())


class ElevationTests(unittest.TestCase):
    def test_source_and_frozen_elevation_preserve_windows_arguments(self):
        args = [r"C:\Controls Folder\settings.py", "space value", 'quote"value', "trailing space \\", "Unicode é"]
        for frozen in (False, True):
            with self.subTest(frozen=frozen), mock.patch.object(gui.sys, "frozen", frozen, create=True), \
                    mock.patch.object(gui.sys, "argv", args), \
                    mock.patch.object(gui.ctypes.windll.shell32, "ShellExecuteW", return_value=42) as execute:
                gui.relaunch_as_admin()
            expected = args[1:] if frozen else [str(Path(args[0]).resolve()), *args[1:]]
            self.assertEqual(execute.call_args.args[3], subprocess.list2cmdline(expected))

    def test_denied_elevation_is_reported(self):
        with mock.patch.object(gui.ctypes.windll.shell32, "ShellExecuteW", return_value=5):
            with self.assertRaises(OSError):
                gui.relaunch_as_admin()


if __name__ == "__main__":
    unittest.main()
