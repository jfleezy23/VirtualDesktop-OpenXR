"""Executable profiles must never rewrite global NR/FG settings."""
import importlib.util
import pathlib
import sys
import unittest
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec("vdxr_game_profiles"),
                             "runtime/Controls executable profile storage is missing")
        import vdxr_game_profiles as profiles
        self.p = profiles
        import winreg
        self.reg = winreg
        self.base = "Software\\VDXRProfileTests\\" + uuid.uuid4().hex
        self.store = profiles.ProfileStore(winreg.HKEY_CURRENT_USER, self.base)
        self.exe = r"C:\ProfileFixtures\One\fixture.exe"

    def tearDown(self):
        if hasattr(self, "store"):
            # Clean only this test's UUID-owned hive, including deliberately corrupt keys.
            def remove_fixture(path):
                try:
                    with self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, path, 0,
                                          self.reg.KEY_READ | self.reg.KEY_WOW64_64KEY) as key:
                        children = [self.reg.EnumKey(key, index) for index in range(self.reg.QueryInfoKey(key)[0])]
                    for child in children:
                        remove_fixture(path + "\\" + child)
                    self.reg.DeleteKeyEx(self.reg.HKEY_CURRENT_USER, path, self.reg.KEY_WOW64_64KEY)
                except FileNotFoundError:
                    pass
            remove_fixture(self.base)

    def test_identity_normalizes_case_slashes_dots_but_not_directory(self):
        self.assertEqual(self.p.profile_id(self.exe), "40d1fe279f0e8a92")
        self.assertEqual(self.p.profile_id(self.exe),
                         self.p.profile_id(r"c:/profilefixtures/one/other/../FIXTURE.EXE"))
        self.assertNotEqual(self.p.profile_id(self.exe),
                            self.p.profile_id(r"C:\ProfileFixtures\Two\fixture.exe"))
        with self.assertRaises(ValueError):
            self.p.profile_id("fixture.exe")
        self.assertEqual(self.p.profile_id(r"\\?\UNC\server\share\fixture.exe"),
                         self.p.profile_id(r"\\?\unc\SERVER\SHARE\FIXTURE.EXE"))
        self.assertEqual(self.p.profile_id(r"\\?\C:\ProfileFixtures\One\fixture.exe"),
                         self.p.profile_id(self.exe))

    def test_complete_roundtrip_inherits_individual_missing_values_and_reset(self):
        globals_ = dict(self.p.DEFAULTS, DLSSNR_Intensity=87, FoveationSize=95)
        self.store.save(None, globals_)
        self.store.save(self.exe, {"FrameGenerationMode": 4, "DLSSNR_Enabled": 1})
        values, sources = self.store.load(self.exe)
        self.assertEqual(values["FrameGenerationMode"], 4)
        self.assertEqual(values["DLSSNR_Intensity"], 87)
        self.assertEqual(sources["DLSSNR_Intensity"], "global")
        all_values = dict(values, DLSSNR_Style=2, DLSSNR_LocalToneStrength=98,
                          DLSSNR_LocalStructureStrength=96, DLSSNR_SkinStructureStrength=94,
                          DLSSNR_ProcessingResolution=75, FrameGenerationBackend=1)
        self.store.save(self.exe, all_values)
        self.assertEqual(self.store.load(self.exe)[0], all_values)
        self.assertEqual(self.store.load(None)[0], globals_)
        self.assertEqual(self.store.list_profiles(), [self.p.normalize_executable(self.exe)])
        self.store.reset(self.exe)
        self.assertEqual(self.store.load(self.exe)[0], globals_)

    def test_missing_coverage_uses_full_coverage_without_overwriting_saved_90(self):
        self.assertEqual(self.store.load()[0]["FoveationSize"], 100)
        self.assertEqual(self.store.load(self.exe)[0]["FoveationSize"], 100)
        self.store.save(self.exe, {})
        self.assertEqual(self.store.load(self.exe)[0]["FoveationSize"], 100)
        self.store.save(None, {"FoveationSize": 90})
        self.assertEqual(self.store.load(self.exe)[0]["FoveationSize"], 90)
        self.assertEqual(self.store.load()[0]["FoveationSize"], 90)

    def test_bad_profile_type_never_inherits_enabled_global(self):
        self.store.save(None, {"DLSSNR_Enabled": 1, "FrameGenerationMode": 3})
        self.store.save(self.exe, {"DLSSNR_Intensity": 50})
        with self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, self.store.path(self.exe), 0,
                              self.reg.KEY_SET_VALUE | self.reg.KEY_WOW64_64KEY) as key:
            self.reg.SetValueEx(key, "DLSSNR_Intensity", 0, self.reg.REG_SZ, "100")
        with self.assertRaisesRegex(ValueError, "DLSSNR_Intensity"):
            self.store.load(self.exe)
        self.assertEqual(self.store.load(None)[0]["DLSSNR_Enabled"], 1)

    def test_hash_collision_metadata_is_rejected(self):
        self.store.save(self.exe, {"FrameGenerationMode": 4})
        with self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, self.store.path(self.exe), 0,
                              self.reg.KEY_SET_VALUE | self.reg.KEY_WOW64_64KEY) as key:
            self.reg.SetValueEx(key, "ExecutablePath", 0, self.reg.REG_SZ, r"c:\other\fixture.exe")
        with self.assertRaisesRegex(ValueError, "identity"):
            self.store.load(self.exe)
        with self.assertRaises(ValueError):
            self.store.save(self.exe, {"FrameGenerationMode": 2})
        with self.assertRaises(ValueError):
            self.store.reset(self.exe)
        # Repair only this isolated fixture, then normal teardown can delete it.
        with self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, self.store.path(self.exe), 0,
                              self.reg.KEY_SET_VALUE | self.reg.KEY_WOW64_64KEY) as key:
            self.reg.SetValueEx(key, "ExecutablePath", 0, self.reg.REG_SZ,
                                self.p.normalize_executable(self.exe))

    def test_reset_recovers_missing_identity_without_touching_other_scope(self):
        other = r"C:\ProfileFixtures\Two\fixture.exe"
        self.store.save(None, {"DLSSNR_Intensity": 89})
        self.store.save(self.exe, {"FrameGenerationMode": 4})
        self.store.save(other, {"DLSSNR_Intensity": 73})
        with self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, self.store.path(self.exe), 0,
                              self.reg.KEY_SET_VALUE | self.reg.KEY_WOW64_64KEY) as key:
            self.reg.DeleteValue(key, "ExecutablePath")
        with self.assertRaisesRegex(ValueError, "identity"):
            self.store.load(self.exe)
        with self.assertRaises(ValueError):
            self.store.save(self.exe, {"FrameGenerationMode": 2})
        try:
            self.store.reset(self.exe)
        except ValueError as error:
            self.fail(f"Reset cannot recover an interrupted profile creation: {error}")
        with self.assertRaises(FileNotFoundError):
            self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, self.store.path(self.exe))
        self.assertEqual(self.store.load(None)[0]["DLSSNR_Intensity"], 89)
        self.assertEqual(self.store.load(other)[0]["DLSSNR_Intensity"], 73)

    def test_corrupt_entry_does_not_hide_valid_profile_from_selector(self):
        import tkinter as tk
        import vdxr_registry_gui as gui
        other = r"C:\ProfileFixtures\Two\fixture.exe"
        self.store.save(other, {"DLSSNR_Intensity": 73})
        for metadata in (("relative.exe", self.reg.REG_SZ), (19, self.reg.REG_DWORD),
                         (r"c:\unrelated\fixture.exe", self.reg.REG_SZ)):
            with self.subTest(metadata_type=metadata[1]):
                self.store.save(self.exe, {})
                with self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, self.store.path(self.exe), 0,
                                      self.reg.KEY_SET_VALUE | self.reg.KEY_WOW64_64KEY) as key:
                    self.reg.SetValueEx(key, "ExecutablePath", 0, metadata[1], metadata[0])
                root = tk.Tk()
                root.withdraw()
                try:
                    app = gui.VDXRRegistryApp(root, profile_store=self.store)
                    self.assertIn(self.p.normalize_executable(other), app.scope_selector.cget("values"),
                                  "One corrupt entry hid a different valid game profile")
                    self.assertIn("skipped", app.status.get().lower())
                    self.assertEqual(self.store.load(other)[0]["DLSSNR_Intensity"], 73)
                    with self.assertRaises(ValueError):
                        self.store.reset(self.exe)
                finally:
                    root.destroy()
                    # Repair this fixture so the next corruption starts from valid metadata.
                    with self.reg.OpenKey(self.reg.HKEY_CURRENT_USER, self.store.path(self.exe), 0,
                                          self.reg.KEY_SET_VALUE | self.reg.KEY_WOW64_64KEY) as key:
                        self.reg.SetValueEx(key, "ExecutablePath", 0, self.reg.REG_SZ,
                                            self.p.normalize_executable(self.exe))

    def test_invalid_save_is_validated_before_any_write(self):
        self.store.save(self.exe, {"FrameGenerationMode": 2})
        for invalid in ({"FrameGenerationMode": 1}, {"DLSSNR_Enabled": True},
                        {"DLSSNR_ProcessingResolution": 49}, {"unexpected": 1}):
            with self.assertRaises(ValueError):
                self.store.save(self.exe, invalid)
        self.assertEqual(self.store.load(self.exe)[0]["FrameGenerationMode"], 2)

    def test_controls_game_edits_are_drafts_then_explicit_save_and_reset(self):
        import tkinter as tk
        import vdxr_registry_gui as gui
        self.assertTrue(callable(getattr(gui.VDXRRegistryApp, "load_scope", None)),
                        "Controls cannot select an executable profile")
        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        self.store.save(None, dict(self.p.DEFAULTS, DLSSNR_Enabled=1, DLSSNR_Intensity=89))
        app = gui.VDXRRegistryApp(root, profile_store=self.store)
        root.update_idletasks()
        self.assertLess(root.winfo_reqheight(), root.winfo_screenheight(),
                        "Controls must fit the screen with all settings reachable")
        app.load_scope(self.exe)
        app.variables[1].set(73)
        app.processing_resolution.set(80)
        app.frame_generation.set("OFXR 4×")
        app.on_frame_generation_changed()
        self.assertEqual(self.store.load(self.exe)[0]["DLSSNR_Intensity"], 89)
        self.assertEqual(self.store.list_profiles(), [])
        app.save_profile()
        values, sources = self.store.load(self.exe)
        self.assertEqual(values["DLSSNR_Intensity"], 73)
        self.assertEqual(values["DLSSNR_ProcessingResolution"], 80)
        self.assertEqual(values["FrameGenerationMode"], 4)
        self.assertEqual(set(sources.values()), {"profile"})
        self.assertEqual(self.store.load(None)[0]["DLSSNR_Intensity"], 89)
        app.reset_defaults()
        self.assertEqual(self.store.load(self.exe)[0]["DLSSNR_Intensity"], 89)
        self.assertEqual(self.store.list_profiles(), [])

    def test_saving_unchanged_game_setup_locks_all_fields_until_reset(self):
        import tkinter as tk
        import vdxr_registry_gui as gui
        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        original = dict(self.p.DEFAULTS, DLSSNR_Enabled=1, DLSSNR_Style=2,
                        DLSSNR_Intensity=89, FrameGenerationMode=4, FrameGenerationBackend=1)
        self.store.save(None, original)
        app = gui.VDXRRegistryApp(root, profile_store=self.store)
        app.load_scope(self.exe)
        self.assertEqual(str(app.save_profile_button.cget("state")), "normal")
        self.assertFalse(app.profile_dirty)
        app.save_profile_button.invoke()
        changed_globals = dict(original, DLSSNR_Intensity=11, FrameGenerationMode=2,
                               FrameGenerationBackend=0, FoveationSize=90)
        self.store.save(None, changed_globals)
        saved, sources = self.store.load(self.exe)
        self.assertEqual(saved, original, "Saved game setup drifted when Globals changed")
        self.assertEqual(set(sources.values()), {"profile"})
        self.assertEqual(self.store.load(None)[0], changed_globals)
        app.reset_defaults()
        inherited, sources = self.store.load(self.exe)
        self.assertEqual(inherited, changed_globals)
        self.assertEqual(set(sources.values()), {"global"})

    def test_scope_switch_flushes_pending_global_sliders(self):
        import tkinter as tk
        from unittest import mock
        from test_fg_controls import Registry, gui
        registry = Registry()
        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        with mock.patch.object(gui, "winreg", registry), \
                mock.patch.object(gui.runtime_selection, "winreg", registry):
            app = gui.VDXRRegistryApp(root, profile_store=self.store)
            app.variables[-1].set(95)
            app.processing_resolution.set(80)
            app.load_scope(self.exe)
            self.assertEqual(registry.values["FoveationSize"], (95, 4))
            self.assertEqual(registry.values["DLSSNR_ProcessingResolution"], (80, 4))


if __name__ == "__main__":
    unittest.main()
